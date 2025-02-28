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

/*!
 * \file tir/usmp/utils.cc
 * \brief Utilities for Unified Static Memory Planner
 */

#include <tvm/runtime/device_api.h>
#include <tvm/runtime/registry.h>
#include <tvm/tir/stmt.h>
#include <tvm/tir/usmp/utils.h>

namespace tvm {
namespace tir {
namespace usmp {

BufferInfo::BufferInfo(String name_hint, Integer size_bytes, Array<PoolInfo> pool_candidates,
                       Integer alignment) {
  auto bufinfo_node = make_object<BufferInfoNode>();
  bufinfo_node->name_hint = name_hint;
  bufinfo_node->size_bytes = size_bytes;
  bufinfo_node->pool_candidates = pool_candidates;
  bufinfo_node->alignment = alignment;
  data_ = std::move(bufinfo_node);
}

void BufferInfoNode::SetConflicts(Array<ObjectRef> conflicting_buffer_info_objs) {
  this->conflicts = conflicting_buffer_info_objs;
}

TVM_REGISTER_NODE_TYPE(BufferInfoNode);
TVM_REGISTER_GLOBAL("tir.usmp.BufferInfo")
    .set_body_typed([](String name_hint, Integer size_bytes, Array<PoolInfo> pool_candidates,
                       Integer alignment) {
      if (!alignment.defined()) {
        return BufferInfo(name_hint, size_bytes, pool_candidates);
      }
      return BufferInfo(name_hint, size_bytes, pool_candidates, alignment);
    });
TVM_REGISTER_GLOBAL("tir.usmp.BufferInfoSetConflicts")
    .set_body_method<BufferInfo>(&BufferInfoNode::SetConflicts);

TVM_STATIC_IR_FUNCTOR(ReprPrinter, vtable)
    .set_dispatch<BufferInfoNode>([](const ObjectRef& ref, ReprPrinter* p) {
      auto* node = static_cast<const BufferInfoNode*>(ref.get());
      p->stream << "BufferInfoNode(\n"
                << "name_hint=" << node->name_hint << ",\n  size_bytes=" << node->size_bytes
                << ",\n  pool_candidates=" << node->pool_candidates
                << ",\n  alignment=" << node->alignment << ")";
    });

BufferInfoAnalysis::BufferInfoAnalysis(Map<BufferInfo, tir::Stmt> buffer_info_stmts,
                                       Integer memory_pressure) {
  auto bufinfo_analysis_node = make_object<BufferInfoAnalysisNode>();
  bufinfo_analysis_node->buffer_info_stmts = buffer_info_stmts;
  bufinfo_analysis_node->memory_pressure = memory_pressure;
  data_ = std::move(bufinfo_analysis_node);
}

TVM_REGISTER_NODE_TYPE(BufferInfoAnalysisNode);
TVM_REGISTER_GLOBAL("tir.usmp.BufferInfoAnalysis")
    .set_body_typed([](Map<BufferInfo, tir::Stmt> buffer_info_stmts, Integer memory_pressure) {
      return BufferInfoAnalysis(buffer_info_stmts, memory_pressure);
    });

TVM_STATIC_IR_FUNCTOR(ReprPrinter, vtable)
    .set_dispatch<BufferInfoAnalysisNode>([](const ObjectRef& ref, ReprPrinter* p) {
      auto* node = static_cast<const BufferInfoAnalysisNode*>(ref.get());
      p->stream << "BufferInfoAnalysisNode(\n"
                << "buffer_info_stmts=" << node->buffer_info_stmts
                << ",\n  memory_pressure=" << node->memory_pressure << ")";
    });

PoolInfo::PoolInfo(String pool_name, Map<Target, String> target_access, Integer size_hint_bytes) {
  auto poolinfo_node = make_object<PoolInfoNode>();
  poolinfo_node->pool_name = pool_name;
  poolinfo_node->size_hint_bytes = size_hint_bytes;
  poolinfo_node->target_access = target_access;
  data_ = std::move(poolinfo_node);
}

TVM_REGISTER_NODE_TYPE(PoolInfoNode);
TVM_REGISTER_GLOBAL("tir.usmp.PoolInfo")
    .set_body_typed([](String pool_name, Map<Target, String> target_access,
                       Integer size_hint_bytes) {
      if (size_hint_bytes.defined()) {
        return PoolInfo(pool_name, target_access, size_hint_bytes);
      }
      return PoolInfo(pool_name, target_access);
    });

TVM_STATIC_IR_FUNCTOR(ReprPrinter, vtable)
    .set_dispatch<PoolInfoNode>([](const ObjectRef& ref, ReprPrinter* p) {
      auto* node = static_cast<const PoolInfoNode*>(ref.get());
      p->stream << "PoolInfoNode(\n"
                << "pool_name=" << node->pool_name << ",\n  target_access=" << node->target_access
                << ",\n  size_hint_bytes=" << node->size_hint_bytes << ")";
    });

PoolAllocation::PoolAllocation(PoolInfo pool_info, Integer byte_offset) {
  auto pool_allocation_node = make_object<PoolAllocationNode>();
  pool_allocation_node->pool_info = pool_info;
  pool_allocation_node->byte_offset = byte_offset;
  data_ = std::move(pool_allocation_node);
}

TVM_REGISTER_NODE_TYPE(PoolAllocationNode);
TVM_REGISTER_GLOBAL("tir.usmp.PoolAllocation")
    .set_body_typed([](PoolInfo pool_info, Integer byte_offset) {
      return PoolAllocation(pool_info, byte_offset);
    });

TVM_STATIC_IR_FUNCTOR(ReprPrinter, vtable)
    .set_dispatch<PoolAllocationNode>([](const ObjectRef& ref, ReprPrinter* p) {
      auto* node = static_cast<const PoolAllocationNode*>(ref.get());
      p->stream << "PoolAllocationNode(\n"
                << "pool_info=" << node->pool_info << ",\n  byte_offset=" << node->byte_offset
                << ")";
    });

Array<BufferInfo> CreateArrayBufferInfo(const Map<BufferInfo, Stmt>& buffer_info_map) {
  Array<BufferInfo> ret;
  for (const auto& kv : buffer_info_map) {
    auto buffer_info = kv.first;
    ret.push_back(buffer_info);
  }
  return ret;
}

Integer CalculateExtentsSize(const AllocateNode* op) {
  size_t element_size_bytes = op->dtype.bytes();
  size_t num_elements = 1;
  for (const auto& ext : op->extents) {
    if (ext->IsInstance<IntImmNode>()) {
      num_elements *= Downcast<IntImm>(ext)->value;
    } else {
      // We can't statically calculate workspace for dynamic shapes
      return Integer();
    }
  }
  return Integer(num_elements * element_size_bytes);
}

TVM_REGISTER_GLOBAL("tir.usmp.CreateArrayBufferInfo")
    .set_body_typed([](Map<BufferInfo, Stmt> buffer_info_map) {
      return (CreateArrayBufferInfo(buffer_info_map));
    });

}  // namespace usmp
}  // namespace tir
}  // namespace tvm
