/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "./te_compiler.h"

#include <tvm/driver/driver_api.h>
#include <tvm/ir/attrs.h>
#include <tvm/ir/function.h>
#include <tvm/relay/analysis.h>
#include <tvm/relay/attrs/annotation.h>
#include <tvm/relay/attrs/call.h>
#include <tvm/relay/attrs/device_copy.h>
#include <tvm/relay/expr.h>
#include <tvm/relay/expr_functor.h>
#include <tvm/relay/op.h>
#include <tvm/runtime/device_api.h>
#include <tvm/runtime/registry.h>
#include <tvm/te/schedule.h>
#include <tvm/te/schedule_pass.h>
#include <tvm/topi/tags.h>

#include <functional>
#include <limits>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../op/annotation/annotation.h"
#include "../op/call/call.h"
#include "../op/memory/device_copy.h"
#include "../transforms/device_aware_visitors.h"
#include "./te_compiler_cache.h"
#include "./utils.h"

namespace tvm {
namespace relay {
// TODO(@jroesch, @csullivan): declare directly elsewhere
backend::StaticMemoryPlan GraphPlanMemory(const Function& func);

namespace tec {

using namespace tvm::relay::transform;

TVM_REGISTER_OBJECT_TYPE(TECompilerNode);

class TECompilerImpl : public TECompilerNode {
 public:
  explicit TECompilerImpl(Optional<IRModule> opt_mod) {
    // Make sure we don't collide with any existing globals in the module.
    if (opt_mod) {
      for (const auto& kv : opt_mod.value()->functions) {
        name_map_[kv.first->name_hint] = 1;
      }
    }
  }

  // Lower the function.
  CachedFunc Lower(const CCacheKey& key, std::function<String(String)> mangle_fn) {
    return LowerInternal(key, mangle_fn)->cached_func;
  }

  CachedFunc Lower(const CCacheKey& key, const String mod_name) {
    auto mangle_fn = [mod_name](String name) { return runtime::get_name_mangled(mod_name, name); };

    return Lower(key, mangle_fn);
  }

  // For now, build one module per function.
  PackedFunc JIT(const CCacheKey& key) final {
    auto mangle_fn = [](String name) { return name; };
    CCacheValue value = LowerInternal(key, mangle_fn);
    if (value->packed_func != nullptr) {
      return value->packed_func;
    }
    auto m = build(value->cached_func->funcs, key->target, Target(nullptr));
    value->packed_func = m.GetFunction(value->cached_func->prim_fn_var->name_hint);
    return value->packed_func;
  }

  CachedFunc LowerShapeFunc(const CCacheKey& key) final {
    return LowerShapeFuncInternal(key)->cached_func;
  }

  IRModule GetLoweredFunctions() {
    IRModule mod;
    // Extract lowered functions from the cache
    for (const auto& it : cache_) {
      auto source_func = it.first;
      auto lowered_func = it.second;

      IRModule lowered_mod = lowered_func->cached_func->funcs;

      // Annotate functions with their target and put them in the return module
      for (const auto& kv : lowered_mod->functions) {
        const GlobalVar& var = kv.first;
        const BaseFunc& func = kv.second;

        // Only add functions that are not external functions
        if (!func->GetAttr<String>(attr::kCompiler).defined()) {
          ICHECK(func->IsInstance<tir::PrimFuncNode>())
              << "Expected all functions that are not external to be PrimFuncs, but found:"
              << std::endl
              << PrettyPrint(func);
          const tir::PrimFunc& prim_func = Downcast<tir::PrimFunc>(func);
          mod->Update(var, WithAttr(prim_func, tvm::attr::kTarget, source_func->target));
        }
      }
    }

    // Extract lowered dynamic shape functions from the shape cache
    for (const auto& it : shape_func_cache_) {
      auto source_func = it.first;
      auto lowered_func = it.second;
      auto target = source_func->target;
      IRModule lowered_mod = lowered_func->cached_func->funcs;

      // Annotate functions with their target and put them in the return module
      for (auto kv : lowered_mod->functions) {
        const GlobalVar& var = kv.first;
        const BaseFunc& func = kv.second;
        const tir::PrimFunc& prim_func = Downcast<tir::PrimFunc>(func);
        mod->Update(var, WithAttr(prim_func, tvm::attr::kTarget, source_func->target));
      }
    }

    return mod;
  }

  void AddExterns(IRModule module) {
    // Everything tagged with "Compiler" has been compiled, so remove those definitions.
    std::vector<GlobalVar> to_be_deleted;
    for (const auto& kv : module->functions) {
      if (kv.second->GetAttr<String>(attr::kCompiler).defined()) {
        to_be_deleted.push_back(kv.first);
      }
    }
    for (const auto& global_var : to_be_deleted) {
      module->Remove(global_var);
    }
    // HOWEVER we still need a Relay definition to go with those now external functions, so
    // retrieve them from the cache and mark them with "ExternalSymbol".
    for (const auto& kv1 : cache_) {
      auto src_func = kv1.first->source_func;
      ICHECK(src_func.defined());
      if (src_func->GetAttr<String>(attr::kCompiler).defined()) {
        for (const auto& kv2 : kv1.second->cached_func->funcs->functions) {
          if (const auto* function_node = kv2.second.as<FunctionNode>()) {
            // Abandon the existing function annotations.
            Function function(function_node->params, function_node->body, function_node->ret_type,
                              function_node->type_params, /*attrs=*/{}, function_node->span);
            // Mark function as 'extern' using the "ExternalSymbol" attribute.
            function = WithAttr(std::move(function), attr::kExternalSymbol, kv2.first->name_hint);
            module->Add(kv2.first, function);
          }
        }
      }
    }
  }

  Array<tvm::runtime::Module> LowerExternalFunctions() {
    Array<tvm::runtime::Module> ret;
    std::vector<CCacheKey> cached_ext_funcs;

    for (const auto& it : cache_) {
      auto src_func = it.first->source_func;
      ICHECK(src_func.defined());
      Optional<String> opt_compiler = src_func->GetAttr<String>(attr::kCompiler);
      if (opt_compiler.defined()) {
        Optional<String> opt_symbol_name = src_func->GetAttr<String>(tvm::attr::kGlobalSymbol);
        ICHECK(opt_symbol_name.defined()) << "No external symbol is set for:" << std::endl
                                          << PrettyPrint(src_func);
        VLOG(1) << "using external codegen '" << opt_compiler.value() << "' for name '"
                << opt_symbol_name.value() << "' and function:" << std::endl
                << PrettyPrint(src_func);
        cached_ext_funcs.push_back(it.first);

        std::string ext_name = "relay.ext." + opt_compiler.value();
        auto pf = tvm::runtime::Registry::Get(ext_name);
        ICHECK(pf) << "Failed to find the codegen tool for " << ext_name;
        // No need to keep compiler attribute at this point, functions have been
        // extracted for specific codegen.
        src_func = WithAttr(std::move(src_func), attr::kCompiler, NullValue<ObjectRef>());
        VLOG_CONTEXT << ext_name;
        runtime::Module ext_mod = (*pf)(src_func);
        if (ext_mod.defined()) {
          if (ext_mod->GetFunction(opt_symbol_name.value(), /*query_imports=*/true) == nullptr) {
            // It's possible the codegen yielded C or C++ tracked separately and thus the
            // returned runtime module can be empty.
            VLOG(1) << "Unable to find definition for the external function '"
                    << opt_symbol_name.value()
                    << "' in the runtime module generated by external codegen '"
                    << opt_compiler.value() << "'";
          }
          ret.push_back(ext_mod);
        } else {
          // A warning only so that we can write unit tests which can return an empty runtime
          // module.
          LOG(WARNING) << "No external runtime module was generated by external codegen '"
                       << opt_compiler.value() << "'";
        }
      }
    }

    // No need to cache external functions as we collected them all to create
    // external runtime modules.
    for (const auto& it : cached_ext_funcs) {
      cache_.erase(it);
    }
    return ret;
  }

  Map<GlobalVar, String> GetDeviceContexts() { return device_contexts_; }
  void SetDeviceContexts(const Map<GlobalVar, String>& device_contexts) {
    device_contexts_ = device_contexts;
  }

  void Clear() final { cache_.clear(); }

  // List all items in the cache.
  Array<ObjectRef> ListItems() {
    std::lock_guard<std::mutex> lock(mutex_);
    Array<ObjectRef> items;
    for (auto& kv : cache_) {
      items.push_back(kv.first);
      items.push_back(kv.second);
    }
    return items;
  }

  /*!
   * \brief Get the cache key of the function that is being lowered currently
   * \return the cache key
   */
  CCacheKey GetCurrentCCacheKey() { return cur_ccache_key_; }

 private:
  // implement lowered func
  CCacheValue LowerInternal(const CCacheKey& key, std::function<String(String)> mangle_fn) {
    VLOG(1) << "lowering:" << std::endl
            << PrettyPrint(key->source_func) << std::endl
            << "for target:" << std::endl
            << key->target->ToDebugString();
    std::lock_guard<std::mutex> lock(mutex_);
    CCacheValue value;
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      VLOG(1) << "already lowered to name:" << std::endl
              << PrettyPrint(it->second->cached_func->prim_fn_var);
      it->second->use_count += 1;
      if (it->second->cached_func.defined()) return it->second;
      value = it->second;
    } else {
      value = CCacheValue(make_object<CCacheValueNode>());
      value->use_count = 1;
      cache_[key] = value;
    }
    cur_ccache_key_ = key;

    Optional<String> opt_compiler = key->source_func->GetAttr<String>(attr::kCompiler);
    if (opt_compiler.defined()) {
      // Don't compile now since we don't have anywhere to put the resulting runtime module.
      // Instead place the original definition in the cache and wait for LowerExternalFunctions.
      IRModule ir_module;
      Optional<String> opt_global_symbol =
          key->source_func->GetAttr<String>(tvm::attr::kGlobalSymbol);
      ICHECK(opt_global_symbol.defined()) << "External function has not been attached a name yet.";
      // Note that the source_func may already be bound to a global function in the module
      // we are compiling, in which case we should not attempt to make its name unique w.r.t.
      // the module's globals. Furthermore, the external codegen tool must bind the compiled
      // function to the "global_symbol" attribute on the source_func. So do not use GetUniqueName
      // here.
      auto target = Target("ext_dev");
      auto global_var = GlobalVar(opt_global_symbol.value());
      global_var->checked_type_ = key->source_func->checked_type();
      ir_module->Add(global_var, key->source_func);
      value->cached_func = CachedFunc(target, global_var, {}, {}, te::Schedule{nullptr},
                                      tir::PrimFunc{nullptr}, {}, ir_module);
      // Collect these here as it's removed in LowerExternalFunctions()
      device_contexts_.Set(value->cached_func->prim_fn_var, opt_compiler.value());
      VLOG(1) << "preparing to use external codegen '" << opt_compiler.value()
              << "' with name:" << std::endl
              << PrettyPrint(value->cached_func->prim_fn_var) << std::endl
              << "and definitions:" << std::endl
              << PrettyPrint(value->cached_func->funcs);
      return value;
    }

    // Enforce use the target.
    With<Target> target_scope(key->target);

    ICHECK(!value->cached_func.defined());
    value->cached_func = PrimFuncFor(key->source_func, key->target, [&](std::string name) {
      auto mangled = mangle_fn(name);
      return GetUniqueName(mangled, &name_map_);
    });

    if (value->cached_func->prim_func.defined()) {
      VLOG(1) << "already have PrimFunc";
      value->cached_func->funcs->Add(value->cached_func->prim_fn_var,
                                     value->cached_func->prim_func.value());
    } else {
      // NOTE: array will copy on write.
      Array<te::Tensor> all_args = Array<te::Tensor>(value->cached_func->inputs);
      for (te::Tensor arg : value->cached_func->outputs) {
        all_args.push_back(arg);
      }
      // lower the function
      std::unordered_map<te::Tensor, tir::Buffer> binds;
      auto func_name = value->cached_func->prim_fn_var->name_hint;
      VLOG(1) << "scheduling";
      IRModule scheduled_module =
          tvm::LowerSchedule(value->cached_func->schedule, all_args, func_name, binds);
      // Unfortunately the above machinery creates its own GlobalVars instead of using *the*
      // GlobalVar we established above. Fix this before the confusion spreads any further.
      // TODO(mbs): LowerSchedule should be given prim_fn_gvar instead of func_name.
      for (const auto& kv : scheduled_module->functions) {
        GlobalVar global_var = kv.first->name_hint == value->cached_func->prim_fn_var->name_hint
                                   ? value->cached_func->prim_fn_var
                                   : kv.first;
        value->cached_func->funcs->Add(global_var, kv.second);
      }
      ICHECK(value->cached_func->funcs->Lookup(value->cached_func->prim_fn_var)
                 .as<tir::PrimFuncNode>());
    }
    VLOG(1) << "lowered to name:" << std::endl
            << PrettyPrint(value->cached_func->prim_fn_var) << std::endl
            << "with definitions:" << std::endl
            << PrettyPrint(value->cached_func->funcs);

    return value;
  }

  // implement lowered shape func
  CCacheValue LowerShapeFuncInternal(const CCacheKey& key) {
    VLOG(1) << "lowering dynamic shape function:" << std::endl
            << PrettyPrint(key->source_func) << std::endl
            << "for target:" << std::endl
            << key->target->ToDebugString();
    std::lock_guard<std::mutex> lock(mutex_);
    CCacheValue value;
    auto it = shape_func_cache_.find(key);
    if (it != shape_func_cache_.end()) {
      it->second->use_count += 1;
      if (it->second->cached_func.defined()) return it->second;
      value = it->second;
    } else {
      value = CCacheValue(make_object<CCacheValueNode>());
      value->use_count = 0;
      shape_func_cache_[key] = value;
    }
    // Enforce use the target.
    With<Target> target_scope(key->target);

    ICHECK(!value->cached_func.defined());

    using tvm::transform::PassContext;
    With<PassContext> fresh_pass_ctx_scope(PassContext::Create());
    value->cached_func = ShapeFuncFor(key->source_func, key->target, [&](std::string name) {
      return GetUniqueName(name, &name_map_);
    });

    ICHECK(
        value->cached_func->funcs->Lookup(value->cached_func->prim_fn_var).as<tir::PrimFuncNode>());

    VLOG(1) << "lowered to name:" << std::endl
            << PrettyPrint(value->cached_func->prim_fn_var) << std::endl
            << "with definitions:" << std::endl
            << PrettyPrint(value->cached_func->funcs);
    return value;
  }

  Map<String, Integer> GetOpWeights() const {
    Map<String, Integer> weights;
    for (const auto& kv : cache_) {
      auto value = kv.second;
      auto name = value->cached_func->prim_fn_var->name_hint;
      weights.Set(name, value->use_count);
    }
    return weights;
  }

  // TODO(mbs): Hold the output module here and reduce the cache_ to just be from
  // Function to GlobalVar.

  /*! \brief compiler cache lock*/
  std::mutex mutex_;
  /*! \brief internal name map to get an unique name */
  std::unordered_map<std::string, int> name_map_;
  /*! \brief internal compiler cache */
  std::unordered_map<CCacheKey, CCacheValue> cache_;
  /*! \brief internal compiler cache for shape funcs */
  std::unordered_map<CCacheKey, CCacheValue> shape_func_cache_;
  /*! \brief the cache key of the function that is being lowered currently*/
  CCacheKey cur_ccache_key_;
  /*! \brief Map of GlobalVar to C Device API context names */
  Map<GlobalVar, String> device_contexts_;
};

TECompiler::TECompiler(Optional<IRModule> opt_mod) {
  auto object = make_object<TECompilerImpl>(std::move(opt_mod));
  data_ = object;
}

/*! \brief The global TE compiler */
// TODO(mbs): To be terminated with extreme prejudice.
TECompiler& TECompiler::Global() {
  static TECompiler* inst = new TECompiler(make_object<TECompilerImpl>(Optional<IRModule>()));
  return *inst;
}
TVM_REGISTER_PASS_CONFIG_OPTION("relay.backend.use_auto_scheduler", Bool);
TVM_REGISTER_PASS_CONFIG_OPTION("relay.backend.use_meta_schedule", Bool);

TVM_REGISTER_GLOBAL("relay.backend._TECompilerGlobal").set_body_typed([]() {
  return TECompiler::Global();
});

TVM_REGISTER_GLOBAL("relay.backend._make_CCacheKey")
    .set_body_typed([](Function source_func, Target target) {
      return CCacheKey(source_func, target);
    });

TVM_REGISTER_GLOBAL("relay.backend._make_LoweredOutput")
    .set_body_typed([](tvm::Array<te::Tensor> outputs, OpImplementation impl) {
      return LoweredOutput(outputs, impl);
    });

TVM_REGISTER_GLOBAL("relay.backend._TECompilerClear").set_body_typed([](TECompiler self) {
  self->Clear();
});

TVM_REGISTER_GLOBAL("relay.backend._TECompilerLower")
    .set_body_typed([](TECompiler self, CCacheKey key, const String mod_name) {
      return self->Lower(key, mod_name);
    });

TVM_REGISTER_GLOBAL("relay.backend._TECompilerJIT")
    .set_body_typed([](TECompiler self, CCacheKey key) { return self->JIT(key); });

TVM_REGISTER_GLOBAL("relay.backend._TECompilerListItems").set_body_typed([](TECompiler self) {
  TECompilerImpl* ptr = dynamic_cast<TECompilerImpl*>(self.operator->());
  ICHECK(ptr != nullptr);
  return ptr->ListItems();
});

using AnalysisRemapping = std::unordered_map<Expr, Expr, ObjectHash, ObjectEqual>;

/*!
 * \brief Rewrites call expressions to Relay Functions marked as "primitive"
 * to calls to the corresponding TIR PrimFunc for the appropriate target.
 *
 * \code
 * %0 = fn(...) { prim_op(...) }     OR   let %p = fn(...) { prim_op(...) }
 * ... %0(...) ...                        ... %p(...) ...
 * ==>
 * def @q(..., target=<target>) { <tir body> }
 * ... @q(...) ...
 * \endcode
 *
 * Requires FuseOps, ToANormalForm, EtaExpand and InferType to have run.
 *
 * FuseOps is needed to identify and lift all prim op calls:
 * \code
 * ... prim_op(...) ...
 * ==>
 * %0 = fn(...) { prim_op(...) }
 * ... %0(...) ...
 * \endcode
 *
 * ToANormalForm is needed so we only need to consider vars and function literals as the call
 * target.
 *
 * EtaExpand is needed to ensures all calls to primitives are direct:
 * \code
 * let %p1 = fn(...) { prim_op1(...) }
 * let %p2 = fn(...) { prim_op2(...) }
 * let %p = if (...) { %p1 } else { %p2 }
 * ... %p(...) ...
 * ==>
 * let %p1 = fn(...) { prim_op1(...) }
 * let %p2 = fn(...) { prim_op2(...) }
 * let %p = fn(...) { if (...) { %p1(...) } else { %p2(...) } }
 * ... %p(...) ...
 * \endcode
 */
class LowerTensorExprMutator : public DeviceAwareExprMutator {
 public:
  LowerTensorExprMutator(const IRModule& module, ProcessFn process_fn, String module_name,
                         TECompiler compiler, SEScope host_se_scope)
      : DeviceAwareExprMutator(module),
        module_(module),
        process_fn_(std::move(process_fn)),
        module_name_(std::move(module_name)),
        compiler_(std::move(compiler)),
        host_se_scope_(std::move(host_se_scope)),
        debug_op_(Op::Get("debug")) {}

  /*!
   *  \brief Returns the primitive function associated with \p expr, or nullptr if none.
   */
  BaseFunc ResolveToPrimitive(const Expr& expr) {
    // NOTE: We can't assume expr->checked_type_ is defined, so can't early exit for first-order
    // expressions.
    if (const auto* global_var_node = expr.as<GlobalVarNode>()) {
      if (!module_->ContainGlobalVar(global_var_node->name_hint)) {
        // TODO(mbs): extern function cleanup
        // Assume the function is extern and thus no longer in the IRModule.
        return {};
      } else {
        BaseFunc base_func = module_->Lookup(GetRef<GlobalVar>(global_var_node));
        return ResolveToPrimitive(base_func);
      }
    } else if (const auto* prim_func_node = expr.as<tir::PrimFuncNode>()) {
      return GetRef<tir::PrimFunc>(prim_func_node);
    } else if (const auto* var_node = expr.as<VarNode>()) {
      auto itr = primitive_functions_.find(var_node);
      if (itr == primitive_functions_.end()) {
        // Not bound to a primitive function.
        return {};
      } else {
        return itr->second;
      }
    } else if (const auto* function_node = expr.as<FunctionNode>()) {
      if (!function_node->HasNonzeroAttr(attr::kPrimitive)) {
        // Not marked as primitive by FuseOps.
        return {};
      }
      if (const auto* call_node = function_node->body.as<CallNode>()) {
        if (call_node->op == debug_op_) {
          // Debug 'primitives' are not lowered.
          return {};
        }
      }
      return GetRef<Function>(function_node);
    } else {
      return {};
    }
  }

  /*!
   * \brief Lowers the primitive function \p func to TIR for ultimate execution
   * on a device with configuration \p target. Returns the global var bound
   * to the TIR implementation, and attributes to attach to the call to identify it as
   * a TIR call.
   */
  Expr MakeLoweredCall(Function func, Array<Expr> visited_args, Array<Type> type_args, Span span,
                       Target target) {
    CCacheKey key = CCacheKey(func, target);
    CachedFunc cfunc = compiler_->Lower(key, module_name_);
    ICHECK(cfunc.defined());

    auto opt_compiler = func->GetAttr<String>(attr::kCompiler);

    // Add some metadata on top of the *original function* and invoke the callback so it can
    // be captured.
    // TODO(@areusch, @jroesch): this metadata is for AOT, this should be our interface for AOT
    Map<GlobalVar, tir::PrimFunc> prim_fns;
    Array<GlobalVar> all_prim_fn_vars;
    for (const auto& kv : cfunc->funcs->functions) {
      if (opt_compiler) {
        // We expect just the original func but with just the ExternalSymbol attribute signaling
        // the function (will be) compiled externally.
        ICHECK(kv.second.as<FunctionNode>())
            << PrettyPrint(kv.first) << " must be bound to an (external) Function";
      } else {
        // We expect one or more PrimFuncs, one of which corresponds to 'the' lowered primitive
        // (and the rest in support of that via tir::Calls).
        ICHECK(kv.second.as<tir::PrimFuncNode>())
            << PrettyPrint(kv.first) << " must be bound to a PrimFunc";
        prim_fns.Set(kv.first, Downcast<tir::PrimFunc>(kv.second));
        all_prim_fn_vars.push_back(kv.first);
      }
    }
    Function func_with_metadata = func;
    func_with_metadata = WithAttr(func_with_metadata, "prim_fn_var", cfunc->prim_fn_var);
    func_with_metadata = WithAttr(func_with_metadata, "prim_funcs", prim_fns);
    func_with_metadata = WithAttr(func_with_metadata, tvm::attr::kTarget, cfunc->target);
    this->process_fn_(func_with_metadata);

    auto call_lowered_attrs = make_object<CallLoweredAttrs>();

    // Non-External Relay Function
    // TODO(mbs): "reshape" cleanup.
    if (!opt_compiler && func->HasNonzeroAttr(attr::kReshapeOnly)) {
      call_lowered_attrs->metadata.Set(attr::kReshapeOnly, tvm::Integer(1));
    }

    call_lowered_attrs->metadata.Set("relay_attrs", func->attrs);
    call_lowered_attrs->metadata.Set("all_prim_fn_vars", all_prim_fn_vars);

    if (IsDynamic(func->ret_type)) {
      // Also lower the companion dynamic shape function.
      // Shape function keys use the underlying primitive function as their 'function',
      // but the generic 'cpu' target as the target since all shape functions run
      // on the host cpu irrespective of where the primitive runs.
      CCacheKey shape_key(func, host_se_scope_->target);
      CachedFunc lowered_shape_func = compiler_->LowerShapeFunc(shape_key);

      // Capture the shape function's global var and parameters 'states' in call
      // annotations so calling convention can be recovered.
      // TODO(mbs): Shape cleanup.
      call_lowered_attrs->metadata.Set("prim_shape_fn_var", lowered_shape_func->prim_fn_var);
      call_lowered_attrs->metadata.Set("prim_shape_fn_states",
                                       lowered_shape_func->shape_func_param_states);
      call_lowered_attrs->metadata.Set(
          "prim_shape_fn_num_inputs", Integer(static_cast<int>(lowered_shape_func->inputs.size())));
      call_lowered_attrs->metadata.Set(
          "prim_shape_fn_num_outputs",
          Integer(static_cast<int>(lowered_shape_func->outputs.size())));
      Array<GlobalVar> all_prim_shape_fn_vars;
      for (const auto& kv : lowered_shape_func->funcs->functions) {
        CHECK(kv.second.as<tir::PrimFuncNode>()) << "must be a prim fn";
        all_prim_shape_fn_vars.push_back(kv.first);
      }
      call_lowered_attrs->metadata.Set("all_prim_shape_fn_vars", all_prim_shape_fn_vars);
    }

    return CallLowered(cfunc->prim_fn_var, std::move(visited_args), Attrs(call_lowered_attrs),
                       type_args, std::move(span));
  }

  std::pair<Var, Expr> PreVisitLetBinding_(const Var& var, const Expr& value) final {
    Var new_var = Downcast<Var>(Mutate(var));
    Expr new_value = Mutate(value);
    BaseFunc prim_func = ResolveToPrimitive(new_value);

    if (prim_func.defined()) {
      // Remember let var is bound (possibly indirectly) to a primitive function.
      primitive_functions_.emplace(var.get(), prim_func);
    }
    return {new_var, new_value};
  }

  Expr PostVisitLet_(const LetNode* pre_let_node, const LetNode* post_let_node) final {
    BaseFunc prim_func = ResolveToPrimitive(post_let_node->value);
    if (prim_func.defined()) {
      // Leaving let var scope
      primitive_functions_.erase(pre_let_node->var.get());
    }
    return DeviceAwareExprMutator::PostVisitLet_(pre_let_node, post_let_node);
  }

  Expr DeviceAwareVisitExpr_(const FunctionNode* function_node) override {
    if (function_node->HasNonzeroAttr(attr::kPrimitive) ||
        function_node->GetAttr<String>(attr::kExternalSymbol)) {
      // Nothing to lower inside primitive/external functions.
      return GetRef<Function>(function_node);
    } else {
      return DeviceAwareExprMutator::DeviceAwareVisitExpr_(function_node);
    }
  }

  Expr DeviceAwareVisitExpr_(const CallNode* call_node) override {
    // We can see five forms of calls:
    //  1. A 'normal' Relay call to a Function with the "primitive" attribute. We will need
    //     to lower that to a global PrimFunc and rewrite the call to:
    //       call_lowered(@new_global, (arg1, ..., argn), <attributes>)
    //     However there are a few special forms which are excluded from this treatment, see
    //     below.
    //  2. A 'normal' Relay call to a Function with the "compiler" attribute. We will need
    //     to invoke the appropriate BYOC toolchain function to yield a runtime module and
    //     rewrite the call to the same form as above.
    //  3. A 'normal' Relay call to a PrimFunc which has already been supplied via a global
    //     definition. We rewrite to use the call_lowered form, but otherwise nothing else
    //     needs to be done.
    //  4. A 'normal' Relay call to a Relay Function without any special attribute. These
    //     calls are not changed.
    //  5. A call_lowered call from an earlier invocation of this pass.
    // Note that ResolveToPrimitive will yield non-null only for cases 1-3.

    // Look for (possibly indirect) calls to primitives.
    BaseFunc primitive_func = ResolveToPrimitive(call_node->op);
    if (!primitive_func.defined()) {
      // Not a call to a primitive function we need to rewrite.
      if (const auto* function_node = call_node->op.as<FunctionNode>()) {
        process_fn_(GetRef<Function>(function_node));
      }
      return DeviceAwareExprMutator::DeviceAwareVisitExpr_(call_node);
    }

    // Prepare the arguments.
    Array<Expr> new_args;
    for (const auto& arg : call_node->args) {
      new_args.push_back(VisitExpr(arg));
    }

    // Special case: device_copies are left as calls to primitive operators
    // (thus undoing FuseOps) so that each backend can handle them directly.
    // TODO(mbs): device_copy cleanup. Would be better for FuseOps to just leave device_copy alone.
    if (const auto* function_node = primitive_func.as<FunctionNode>()) {
      DeviceCopyProps device_copy_props = GetDeviceCopyProps(function_node->body);
      if (device_copy_props.body.defined()) {
        ICHECK_EQ(new_args.size(), 1);
        return DeviceCopy(new_args[0], device_copy_props.src_se_scope,
                          device_copy_props.dst_se_scope);
      }
    }

    // Special case: If already lowered by other means then so we don't need to mutate
    // the call but we do need to mutate the arguments
    if (const auto* prim_func_node = primitive_func.as<tir::PrimFuncNode>()) {
      // Function should already be Target annotated by this point
      // but the TE Compiler metadata is still needed for the callback
      // TODO(Mousius) - Robustify this to not assume we're in the GlobalVar for Target Hooks
      GlobalVar prim_func_var = Downcast<GlobalVar>(call_node->op);
      tir::PrimFunc prim_func = GetRef<tir::PrimFunc>(prim_func_node);

      Map<GlobalVar, tir::PrimFunc> prim_fns = {{prim_func_var, prim_func}};
      tir::PrimFunc func_with_metadata = WithAttrs(prim_func, {
                                                                  {"prim_fn_var", prim_func_var},
                                                                  {"prim_funcs", prim_fns},
                                                              });

      ICHECK(!IsDynamic(call_node->checked_type()));
      auto call_lowered_attrs = make_object<CallLoweredAttrs>();
      call_lowered_attrs->metadata.Set("relay_attrs", primitive_func->attrs);

      process_fn_(func_with_metadata);
      return CallLowered(call_node->op, std::move(new_args), Attrs(std::move(call_lowered_attrs)),
                         call_node->type_args, call_node->span);
    }

    // Typical case: call to fused primitive Relay Function.
    // Find the desired target device.
    Target target;
    if (primitive_func->GetAttr<String>(attr::kCompiler).defined()) {
      // The generic 'external device' target.
      // TODO(mbs): Retire once replaced unified BYOC compiler and target machinery
      target = Target("ext_dev");
    } else {
      // The target corresponding to the call_node expression's annotation.
      SEScope se_scope = GetSEScope(GetRef<Call>(call_node));
      ICHECK(!se_scope->IsFullyUnconstrained());
      target = se_scope->target;
      ICHECK(target.defined());
    }

    // Lower the primitive function for that target.
    Function function = Downcast<Function>(primitive_func);
    return MakeLoweredCall(function, std::move(new_args), call_node->type_args, call_node->span,
                           target);
  }

  IRModule module_;
  ProcessFn process_fn_;
  // Map from in-scope let-bound variables to Functions known to be primitive, or PrimFuncs which
  // have already been lowered. We'll rewrite these to the fresh global vars bound to the lowered
  // primitive function as we go. Those vars will be bound in the target device-type specific
  // module we'll ultimately emit for each required device-type. Note that a primitive may be
  // lowered for multiple device types, each which will be assigned a fresh var.
  std::unordered_map<const VarNode*, BaseFunc> primitive_functions_;
  String module_name_;
  TECompiler compiler_;
  /*!
   * \brief The \p SEScope for the host, which is where all shape-related data and computation
   * must live.
   */
  SEScope host_se_scope_;
  // Cache ops that need to be frequently used later to reduce lookup overhead.
  const Op& debug_op_;
};

Target GetTargetFromInteger(DLDeviceType dev_type, tec::TargetMap targets) {
  if (targets.size() == 1) {
    // The homogeneous execution case, return the only target.
    const auto& it = targets.begin();
    return (*it).second;
  } else {
    // The heterogeneous execution case, return the target associated with the
    // given device type.
    // If "dev_type" equals to 0, the device name only can be got from
    // "targets", and it may not be "llvm", so here just set it to "unknown".
    std::string dev_name = "unknown";
    if (dev_type != 0) {
      dev_name = runtime::DeviceName(dev_type);
    }

    if (targets.count(dev_type) == 0) {
      std::stringstream msg;
      msg << "No target is specified for provided device name: `" << dev_name << "`\n\n"
          << dev_name << " mapped to device type (" << dev_type
          << ") which was not found in the target map.\n"
          << "Availible targets: \n";
      for (auto target : targets) {
        msg << "  " << target.first << "-> " << target.second << "\n";
      }
      LOG(FATAL) << msg.str();
    }
    return targets[dev_type];
  }
}

Pass LowerTensorExpr(const String& module_name, TECompiler compiler, ProcessFn process_fn,
                     SEScope host_se_scope) {
  runtime::TypedPackedFunc<Function(Function, IRModule, PassContext)> pass_func =
      [=](Function func, IRModule module, PassContext ctx) {
        LowerTensorExprMutator lower_te(module, process_fn, module_name, compiler, host_se_scope);
        return Downcast<Function>(lower_te.Mutate(func));
      };
  return CreateFunctionPass(pass_func, 0, "LowerTensorExpr", {});
}

backend::FunctionInfo UpdateMainWorkspaceSize(const IRModule& mod, tec::TargetMap targets,
                                              Map<Expr, backend::StorageInfo> storage_info_map) {
  Function func = Downcast<Function>(mod->Lookup("main"));

  VLOG_CONTEXT << "UpdateMainWorkspaceSize";
  VLOG(1) << "calculating FunctionInfo for main:" << std::endl << PrettyPrint(func);
  for (const auto& kv : targets) {
    VLOG(1) << "  target " << kv.first << " = " << kv.second->str();
  }

  // This is a Map<device,Map<storage_id, size>>
  // TODO(mbs): Collapsing SEScopes to just device type.
  std::unordered_map<DLDeviceType, std::unordered_map<int, int>, backend::EnumClassHash>
      sid_workspace;
  // This is a Map<device, size_of_inputs_and_outputs>
  std::unordered_map<DLDeviceType, int, backend::EnumClassHash> device_io;
  // This is a Map<device, size_of_constants>
  std::unordered_map<DLDeviceType, int, backend::EnumClassHash> device_consts;

  // Initialize the mapping from all storage identifiers to workspace sizes,
  // the amount of device io, and the device constants.
  for (const auto& kv : storage_info_map) {
    const backend::StorageInfo& storage_info = kv.second;
    const std::vector<int64_t>& storage_ids = storage_info->storage_ids;
    const std::vector<SEScope>& se_scopes = storage_info->se_scopes;
    CHECK_EQ(storage_ids.size(), se_scopes.size());
    for (uint32_t i = 0; i < se_scopes.size(); i++) {
      DLDeviceType device_type = se_scopes[i]->device_type();
      sid_workspace[device_type][storage_ids[i]] = 0;
      device_io[device_type] = 0;
      device_consts[device_type] = 0;
    }
  }

  // Iterate the storage map to compute all the tensor sizes in the program.
  // There are 3 cases in this code:
  //
  // First we need to compute the sizes of all
  // inline constants.
  //
  // Second we compute the size of any bound variable as these are input and output
  // sizes of the program.
  //
  // Finally for all other expressions we check which storage identifier they have
  // been assigned and we compute the maximal size of the storage, as tensors can
  // share storage with other tensors which are the same size or larger.
  //
  // In this final case there is only one allocation for all tensors which share storage
  // which will be the maximal size of all tensors which were assigned to it.
  for (const auto& kv : storage_info_map) {
    const Expr& expr = kv.first;
    const backend::StorageInfo& storage_info = kv.second;
    int64_t size_bytes = backend::CalculateRelayExprSizeBytes(expr->checked_type());
    VLOG(1) << "expression:" << std::endl
            << PrettyPrint(expr) << std::endl
            << "of type:" << std::endl
            << PrettyPrint(expr->checked_type()) << std::endl
            << "has size " << size_bytes << " and storage info:" << std::endl
            << storage_info;
    const std::vector<int64_t>& storage_ids = storage_info->storage_ids;
    const std::vector<SEScope>& se_scopes = storage_info->se_scopes;

    if (expr->IsInstance<ConstantNode>()) {
      for (const auto& se_scope : se_scopes) {
        DLDeviceType device_type = se_scope->device_type();
        ICHECK_EQ(device_consts.count(device_type), 1);
        device_consts[device_type] += size_bytes;
      }
    } else if (expr->IsInstance<VarNode>() || expr.same_as(func->body)) {
      CHECK_GE(se_scopes.size(), 1) << "must be at least one device";
      for (const auto& se_scope : se_scopes) {
        DLDeviceType device_type = se_scope->device_type();
        device_io[device_type] += size_bytes;
      }
    } else {
      // TODO(@electriclilies): This code is never being called which means sid_workspace is not
      // updated.. This means that storage info is probably not being created correctly. Or is not
      // equivalent to what was here previously
      for (uint32_t i = 0; i < storage_ids.size(); i++) {
        // Here we record the largest size of the tensor
        // that share the same storage id, because storage_id will
        // be shared between multiple tensors that are not live simultaneously.
        DLDeviceType device_type = se_scopes[i]->device_type();
        if (size_bytes > sid_workspace[device_type][storage_ids[i]]) {
          sid_workspace[device_type][storage_ids[i]] = size_bytes;
        }
      }
    }
  }

  // This is a Map<device, workspace_size>
  std::unordered_map<DLDeviceType, int, backend::EnumClassHash> device_workspace;
  // Once we know the sizes of sids, we need to accumulate per device
  for (const auto& dev_sid_size : sid_workspace) {
    auto dev = dev_sid_size.first;
    device_workspace[dev] = 0;
    for (const auto& sid_size : dev_sid_size.second) {
      device_workspace[dev] += sid_size.second;
    }
  }

  Map<Target, Integer> workspace_sizes;
  Map<Target, Integer> io_sizes;
  Map<Target, Integer> constant_sizes;
  Map<Target, tir::PrimFunc> tir_primfuncs;
  Map<Target, Function> relay_primfuncs;

  // Initialize all target workspaces to zero
  for (const auto& kv : targets) {
    auto tgt = kv.second;
    workspace_sizes.Set(tgt, 0);
  }

  for (const auto& dev_and_size : device_workspace) {
    auto tgt = tec::GetTargetFromInteger(dev_and_size.first, targets);
    workspace_sizes.Set(tgt, dev_and_size.second);
    relay_primfuncs.Set(tgt, func);
  }
  for (const auto& dev_and_size : device_io) {
    auto tgt = tec::GetTargetFromInteger(dev_and_size.first, targets);
    io_sizes.Set(tgt, dev_and_size.second);
  }

  for (const auto& dev_and_size : device_consts) {
    auto tgt = tec::GetTargetFromInteger(dev_and_size.first, targets);
    ICHECK_EQ(constant_sizes.count(tgt), 0);
    constant_sizes.Set(tgt, dev_and_size.second);
  }

  backend::FunctionInfo func_info(std::move(workspace_sizes), std::move(io_sizes),
                                  std::move(constant_sizes), std::move(tir_primfuncs),
                                  std::move(relay_primfuncs));
  VLOG(1) << "func_info: " << func_info;
  return std::move(func_info);
}

/*!
 * \brief A function to create the function metadata for an input function (ie calculate buffer
 * input/output sizes)
 * \param func The function to calculate function metadata for
 * \param function_metadata The map that stores all the function metadatas
 */
void UpdateFunctionMetadata(BaseFunc func,
                            Map<String, backend::FunctionInfo>& function_metadata,  // NOLINT(*)
                            Integer workspace_byte_alignment) {
  VLOG_CONTEXT << "UpdateFunctionMetadata";
  VLOG(1) << "updating function metadata for:" << std::endl << PrettyPrint(func);
  // Originally UpdateFunctionMetadata took in CCachedFunc and looped through all the funcs stored
  // there Now the goal is to take only one func because process_fn should be controlling the
  // iteration However, to do the workspace calculations we need the primfuncs. So process_fn
  // needs to either access the cached funcs or be directly passed primfuncs This is bad and
  // ideally we don't want process_fn to look at primfuncs There's also the question now of what
  // the function metadatas are and how they are used if we can do something else to replicate the
  // behavior of the function metadatas that might be good (ie annotating functions or something).
  Map<Target, Integer> workspace_sizes;
  Map<Target, Integer> io_sizes;
  Map<Target, Integer> constant_sizes;
  Map<Target, tir::PrimFunc> tir_primfuncs;
  Map<Target, Function> relay_primfuncs;

  Optional<Map<GlobalVar, tir::PrimFunc>> prim_fns =
      func->GetAttr<Map<GlobalVar, tir::PrimFunc>>("prim_funcs");
  CHECK(prim_fns) << "primitive functions not set on Relay function by TECompiler.";

  Optional<GlobalVar> prim_fn_var = func->GetAttr<GlobalVar>("prim_fn_var");
  CHECK(prim_fn_var) << "prim_fn_var must be set on Relay functions by TECompiler.";

  Optional<Target> relay_target = func->GetAttr<Target>(tvm::attr::kTarget);
  CHECK(relay_target) << "target must be set on Relay functions by the TECompiler.";

  for (const auto& kv : prim_fns.value()) {
    auto prim_fn = Downcast<tir::PrimFunc>(kv.second);
    CHECK(prim_fn.defined()) << "the primitive function must be defined";

    Integer workspace_size = CalculateWorkspaceBytes(prim_fn, workspace_byte_alignment);

    // Workspace sizes
    Target prim_fn_target;
    if (prim_fn->attrs->dict.count(tvm::attr::kTarget)) {
      prim_fn_target = Downcast<Target>(prim_fn->attrs->dict[tvm::attr::kTarget]);
    } else {
      prim_fn_target = relay_target.value();
    }

    workspace_sizes.Set(prim_fn_target, workspace_size);

    // Calculating size for I/O
    // TODO(mbs): See also the other three utils for calculating tensor bytesize.
    for (auto const& param : prim_fn->params) {
      bool not_a_buffer = prim_fn->buffer_map.count(param) == 0;
      if (not_a_buffer) {
        io_sizes.Set(prim_fn_target, 0);
        continue;
      }

      auto p_shape = prim_fn->buffer_map[param]->shape;
      int num_of_elements = 1;
      for (const auto& dim_index_expr : p_shape) {
        if (dim_index_expr->IsInstance<IntImmNode>()) {
          num_of_elements *= dim_index_expr.as<IntImmNode>()->value;
        } else {
          // If shape is dynamic, we cannot calculate workspace in compile time.
          num_of_elements = 0;
        }
      }
      int element_size = prim_fn->buffer_map[param]->dtype.bytes();
      io_sizes.Set(prim_fn_target, element_size * num_of_elements);
    }

    constant_sizes.Set(prim_fn_target, 0);
    tir_primfuncs.Set(prim_fn_target, prim_fn);
    if (func->IsInstance<FunctionNode>()) {
      relay_primfuncs.Set(prim_fn_target, Downcast<Function>(func));
    }
  }

  backend::FunctionInfo fi = backend::FunctionInfo(
      std::move(workspace_sizes), std::move(io_sizes), std::move(constant_sizes),
      std::move(tir_primfuncs), std::move(relay_primfuncs));

  VLOG(1) << "FunctionInfo: " << PrettyPrint(prim_fn_var.value()) << " = " << PrettyPrint(fi);

  // The primitive function name here corresponds to the string we will use to generate
  // this Relay function at the low level.
  function_metadata.Set(prim_fn_var.value()->name_hint, fi);
}

IRModule LowerTE(const IRModule& module, const String& module_name, ProcessFn process_fn,
                 SEScope host_se_scope) {
  TECompiler compiler(module);

  // TODO(mbs): This is all unnecessarily convoluted. Better would be to accumulate the rewritten
  // module as we go (including rewritten Functions, lowered primitives, and runtime modules
  // generated by external toolchains), and use a pair of maps over vars and global vars
  // to global vars to remember which functions have already been lowered.

  // Lower all the callees in module:
  //  - Functions tagged with "Compiler" are unchanged (checked by CreateFunctionPass)
  //  - Functions tagged with "Primitive" are unchanged (checked by LowerTensorExprMutator)
  //  - Called functions tagged with "Compiler" are copied into the compiler cache with a fresh
  //    GlobalVar, and calls updated (sticking with regular Relay Call).
  //  - Calls to functions tagged with "Primitive" are compiled to PrimFuncs, and calls updated
  //    (using call_lowered convention).
  IRModule updated_module = LowerTensorExpr(module_name, compiler, std::move(process_fn),
                                            std::move(host_se_scope))(module);

  // The Functions tagged with "Compiler" are now residing in the cache ready to be
  // compiled by LowerExternalFunctions. However we still need a record of them in the
  // IRModule so that the various executors can see which function names need to be
  // retrieved. They may, however, have been renamed.
  compiler->AddExterns(updated_module);

  // Add the lowered functions.
  IRModule lowered_module = compiler->GetLoweredFunctions();
  VLOG(1) << "capturing " << lowered_module->functions.size() << " new lowered functions";
  for (const auto& kv : lowered_module->functions) {
    if (updated_module->ContainGlobalVar(kv.first->name_hint)) {
      LOG(FATAL) << "duplicate bindings for '" << kv.first->name_hint
                 << "'. Existing is:" << std::endl
                 << PrettyPrint(updated_module->Lookup(kv.first->name_hint)) << std::endl
                 << "while new is:" << std::endl
                 << PrettyPrint(kv.second);
    }
    updated_module->Add(kv.first, kv.second);
  }

  // Invoke external codegen for all Functions in the cache tagged with "Compiler", and
  // annotate the module with the resulting runtime modules.
  // TODO(mbs): runtime modules should be first class rather than attributes.
  Array<runtime::Module> external_mods =
      module->GetAttr<Array<runtime::Module>>("external_mods", Array<runtime::Module>()).value();
  Array<runtime::Module> new_external_mods = compiler->LowerExternalFunctions();
  VLOG(1) << "capturing " << external_mods.size() << " existing and " << new_external_mods.size()
          << " new external modules";
  for (const auto& mod : new_external_mods) {
    external_mods.push_back(mod);  // copy-on-write.
  }

  // Annotate the module with C Device API context mapping (this is until we have Targets
  // annotated for the C Device API)
  // TODO(Mousius) - Remove "device_contexts" as soon as we have the graph annotated properly with
  // Targets
  Map<GlobalVar, String> device_contexts =
      module->GetAttr<Map<GlobalVar, String>>("device_contexts", Map<GlobalVar, String>()).value();
  Map<GlobalVar, String> new_device_contexts = compiler->GetDeviceContexts();
  VLOG(1) << "capturing " << device_contexts.size() << " existing and "
          << new_device_contexts.size() << " new device contexts for external functions";
  for (const auto& kv : new_device_contexts) {
    ICHECK_EQ(device_contexts.count(kv.first), 0);
    device_contexts.Set(kv.first, kv.second);  // copy-on-write.
  }

  updated_module = WithAttrs(updated_module, {{"external_mods", std::move(external_mods)},
                                              {"device_contexts", std::move(device_contexts)}});

  if (backend::IsAutoSchedulerEnabled()) {
    // Capture all the 'operator weights', ie usage counts for each PrimFunc.
    Map<String, Integer> op_weights =
        module->GetAttr<Map<String, Integer>>("op_weights", Map<String, Integer>()).value();
    Map<String, Integer> new_op_weights = compiler->GetOpWeights();
    VLOG(1) << "capturing " << op_weights.size() << " existing and " << new_op_weights.size()
            << " new operator weights for PrimFuncs";
    for (const auto& kv : new_op_weights) {
      ICHECK_EQ(op_weights.count(kv.first), 0);
      op_weights.Set(kv.first, kv.second);  // copy-on-write.
    }
    updated_module = WithAttr(updated_module, "op_weights", std::move(op_weights));
  }

  return updated_module;
}

Map<Target, IRModule> GetPerTargetModules(IRModule mod) {
  std::unordered_map<Target, IRModule, backend::TargetStrHash, backend::TargetStrEqual>
      per_target_modules;
  for (const auto& kv : mod->functions) {
    const GlobalVar& var = kv.first;
    const BaseFunc& func = kv.second;
    if (func->IsInstance<tir::PrimFuncNode>()) {
      // Extract target
      Optional<Target> target = func->GetAttr<Target>(tvm::attr::kTarget);
      ICHECK(target) << "Target should be set at this point";

      // Put the function in per_target_modules
      if (!per_target_modules.count(target.value())) {
        // Initialize the IRModule for this target with the attributes from the input IRModule
        IRModule target_module = IRModule({}, {}, {}, {}, mod->attrs);
        // Add the function to the IRModule
        target_module->Add(var, func);
        per_target_modules[target.value()] = target_module;
      } else {
        // The IRModule for this target is initialized, so just add the function.
        IRModule target_module = per_target_modules.at(target.value());
        target_module->Add(var, func);
      }
    } else if (!func->IsInstance<relay::FunctionNode>()) {
      LOG(FATAL)
          << "The function types in the IRModule should be RelayFunction or PrimFunc, but got "
          << func->GetTypeKey();
    }
  }
  return per_target_modules;
}

Pass LowerTEPass(const String& module_name, ProcessFn process_fn, SEScope host_se_scope) {
  runtime::TypedPackedFunc<IRModule(IRModule, PassContext)> pass_func = [=](IRModule module,
                                                                            PassContext ctx) {
    return LowerTE(module, module_name, process_fn, host_se_scope);
  };

  return tvm::transform::Sequential(
      {tvm::relay::transform::RelayToTIRTargetHook(),
       tvm::transform::CreateModulePass(pass_func, 0, "LowerTE", {"InferType"}), InferType()});
}
}  // namespace tec
}  // namespace relay
}  // namespace tvm
