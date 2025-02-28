# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
# pylint: disable=invalid-name, unused-argument, import-outside-toplevel, no-value-for-parameter
"""A set of passes to legalize some of operations for the NPU"""
from typing import List, Type, Callable
import math

import numpy as np  # type: ignore

import tvm  # type: ignore
from tvm import relay
from tvm import ir
from tvm.relay.dataflow_pattern import DFPatternCallback  # type: ignore
from tvm.relay.dataflow_pattern import wildcard
from tvm.relay.dataflow_pattern import is_op
from tvm.relay.dataflow_pattern import rewrite
from tvm.relay.dataflow_pattern import CallPattern
from tvm.relay.backend.contrib.ethosu import op as ethosu_ops  # type: ignore
from tvm.relay.backend.contrib.ethosu.errors import UnsupportedLayout  # type: ignore
from tvm.relay.backend.contrib.ethosu import vela_api
from tvm.relay.backend.contrib.ethosu import util
from tvm.relay.op.contrib import ethosu as ethosu_patterns  # type: ignore


class SplitRewriter(DFPatternCallback):
    """This rewriting converts split operations into a sequence of
    strided_slice operations, because codegen is going to be based
    on strided_slices that will define the slice of the tensor that
    will be fed to the consumer.
    """

    def __init__(self):
        super().__init__(require_type=True)
        self.split_in = wildcard()
        self.pattern = is_op("split")(self.split_in)

    @staticmethod
    def get_section_begin_coords(split: tvm.relay.Expr) -> List[int]:
        """Currently, the split operator takes an array of indices or an integer
        indicating the number of splits. However, its an array of indices could
        represent both cases, therefore this function just make it an array of
        indices where each index represent the co-ordinate of beginning of each
        section -- defines as section begins.

        Parameters
        ----------
        split : tvm.relay.Expr
            The Relay Call expression for a split operator

        Returns
        -------
        section_begins : List[int]
            A list containing integers corresponding to section
            begins
        """
        indices_or_sections = split.attrs.indices_or_sections
        input_shape = split.args[0].checked_type.shape
        split_axis = split.attrs.axis

        if isinstance(indices_or_sections, tvm.ir.container.Array):
            # 0 is the beginning of the first section.
            return [0] + list(indices_or_sections)
        split_axis_len = input_shape[split_axis].value
        section_length = split_axis_len // indices_or_sections.value
        return list(range(0, split_axis_len, section_length))

    def callback(
        self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map
    ) -> tvm.relay.Expr:
        split_input = post.args[0]
        split_begins = list()
        split_ends = list()
        section_begins_in_split_axis = self.get_section_begin_coords(post)
        for split_cord in section_begins_in_split_axis:
            # first begin is [0, 0, ... , 0]
            begin_shape = [0 for i in range(len(split_input.checked_type.shape))]
            begin_shape[post.attrs.axis] = split_cord
            split_begins.append(begin_shape)

            end_shape = list(split_input.checked_type.shape)
            # Only the split axis coordinate changes
            end_shape[post.attrs.axis] = split_cord
            split_ends.append(end_shape)

        # Coordinates needs to be shifted left because beginning
        # of the next section is the end of the previous
        split_ends = split_ends[1:]
        # Last section end is the shape of the tensor itself.
        split_ends.append(list(split_input.checked_type.shape))

        strided_slices = list()
        for sb, se in zip(split_begins, split_ends):
            strided_slices.append(relay.strided_slice(split_input, sb, se))

        return relay.Tuple(strided_slices)


@ir.transform.module_pass(opt_level=1)
class LegalizeSplit:
    """This is the pass that wraps SplitRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(SplitRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


def get_lut_from_func(
    ifm_scale: float, ifm_zp: int, ofm_scale: float, ofm_zp: int, func: Callable[[float], float]
) -> List[int]:
    """Method to calculate the values of the lookup table based on the calculation function"""
    lut_values = list()
    # Only int8 is currently supported
    dtype = np.int8
    qmin, qmax = np.iinfo(dtype).min, np.iinfo(dtype).max
    for x in range(qmin, qmax + 1):
        x_real = ifm_scale * (x - ifm_zp)
        out_real = func(x_real)
        lut_result = int(util.round_away_zero(ofm_zp + out_real / ofm_scale))
        lut_result = min(qmax, max(qmin, lut_result))
        lut_values.append(lut_result)

    return lut_values


class LutActivationRewriter(DFPatternCallback):
    """A class to create an identity operator with the LUT"""

    def __init__(
        self, params_class: Type, activation_type: str, calc_func: Callable[[float], float]
    ):
        super().__init__(require_type=True, rewrite_once=True)
        self.pattern = (wildcard().has_attr({"Composite": params_class.composite_name}))(wildcard())
        self.activation_type = activation_type
        self.calc_func = calc_func

    def callback(self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map):
        id_input = post.args[0]

        quantize_args = post.op.body.args
        output_scale = float(quantize_args[1].data.asnumpy())
        output_zp = int(quantize_args[2].data.asnumpy())

        dequantize_args = quantize_args[0].args[0].args
        input_scale = float(dequantize_args[1].data.asnumpy())
        input_zp = int(dequantize_args[2].data.asnumpy())

        lut_values = get_lut_from_func(
            input_scale, input_zp, output_scale, output_zp, self.calc_func
        )
        lut = relay.const(lut_values, dtype="uint8")

        # We baked the requantization into the LUT, so we don't requantize the identity operator
        identity = ethosu_ops.ethosu_identity(
            ifm=id_input,
            lut=lut,
            ifm_scale=input_scale,
            ifm_zero_point=input_zp,
            ofm_scale=input_scale,
            ofm_zero_point=input_zp,
            activation=self.activation_type,
        )

        return identity


class TanhRewriter(LutActivationRewriter):
    """This pass adds tanh as a LUT to the identity operator"""

    def __init__(self):
        super().__init__(
            params_class=ethosu_patterns.TanhParams, activation_type="TANH", calc_func=math.tanh
        )


@ir.transform.module_pass(opt_level=1)
class LegalizeTanh:
    """This is the pass that wraps TanhRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(TanhRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


def sigmoid_calc_func(x: float) -> float:
    """Function to calculate the values for sigmoid"""
    # Thse limits are inherited from TFLite
    upper_limit = 8.0
    lower_limit = -8.0

    if x <= lower_limit:
        y = 0.0
    elif x >= upper_limit:
        y = 1.0
    else:
        y = 1 / (1 + math.exp(-x))
    return y


class SigmoidRewriter(LutActivationRewriter):
    """This pass adds sigmoid as a LUT for identity op"""

    def __init__(self):
        super().__init__(
            params_class=ethosu_patterns.SigmoidParams,
            activation_type="SIGMOID",
            calc_func=sigmoid_calc_func,
        )


@ir.transform.module_pass(opt_level=1)
class LegalizeSigmoid:
    """This is the pass that wraps SigmoidRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(SigmoidRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class Conv2DRewriter(DFPatternCallback):
    """Convert conv2d related composite functions into ethosu_conv2d operators"""

    def __init__(self):
        super().__init__(require_type=True)
        self.pattern = (wildcard().has_attr({"Composite": "ethos-u.qnn_conv2d"}))(wildcard())

    def callback(
        self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map
    ) -> tvm.relay.Expr:
        params = ethosu_patterns.QnnConv2DParams(post.op.body)
        params.ifm.tensor = post.args[0]
        channels_map = {
            "NHWC": 3,
        }
        if str(params.ofm.layout) not in channels_map.keys():
            raise UnsupportedLayout(str(params.ofm.layout))
        kernel_size_map = {
            "HWIO": params.weights.shape[0:2],
            "OHWI": params.weights.shape[1:3],
            "HWOI": params.weights.shape[0:2],
        }
        if str(params.weights.layout) not in kernel_size_map.keys():
            raise UnsupportedLayout(str(params.weights.layout))
        activation_map = {"clip": "CLIP"}
        weight_to_ohwi_transform_map = {"HWIO": [3, 0, 1, 2]}
        weights_values = params.weights.values
        weights_values_ohwi = np.transpose(
            weights_values, weight_to_ohwi_transform_map[str(params.weights.layout)]
        )
        if params.activation:
            activation = activation_map[params.activation.op.name]
            clip_min = int(params.activation.attrs.a_min)
            clip_max = int(params.activation.attrs.a_max)
        else:
            activation = "NONE"
            clip_min = 0
            clip_max = 0
        scale_bias = vela_api.pack_biases(
            biases=params.biases.tensor.data.asnumpy(),
            ifm_scale=params.ifm.q_params.scale_f32,
            ifm_dtype=np.dtype(params.ifm.dtype),
            weight_scales=params.weights.q_params.scale_f32,
            ofm_scale=params.ofm.q_params.scale_f32,
            is_activation_tanh_or_sigmoid=activation in ["TANH", "SIGMOID"],
        )
        ethosu_conv2d = ethosu_ops.ethosu_conv2d(
            ifm=post.args[0],
            weight=relay.const(weights_values_ohwi, params.weights.values.dtype),
            scale_bias=relay.const(scale_bias, "uint8"),
            lut=relay.const([], dtype="int8"),
            ifm_scale=float(params.ifm.q_params.scale_f32),
            ifm_zero_point=int(params.ifm.q_params.zero_point),
            weight_zero_point=int(params.weights.q_params.zero_point),
            ofm_scale=float(params.ofm.q_params.scale_f32),
            ofm_zero_point=int(params.ofm.q_params.zero_point),
            kernel_shape=kernel_size_map[str(params.weights.layout)],
            ofm_channels=params.ofm.shape[channels_map[str(params.ofm.layout)]],
            strides=params.strides,
            padding=params.padding,
            dilation=params.dilation,
            activation=activation,
            clip_min=clip_min,
            clip_max=clip_max,
            upscale="NONE",
            ifm_layout=str(params.ifm.layout),
            ofm_layout=str(params.ofm.layout),
        )
        return ethosu_conv2d


@ir.transform.module_pass(opt_level=1)
class LegalizeConv2D:
    """This is the pass that wraps the Conv2DRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(Conv2DRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class DepthwiseConv2DRewriter(DFPatternCallback):
    """Convert ethosu.qnn_depthwise_conv2d composite functions to ethosu_depthwise_conv2d
    operators"""

    def __init__(self):
        super().__init__(require_type=True)
        self.pattern = (
            wildcard().has_attr(
                {"Composite": ethosu_patterns.QnnDepthwiseConv2DParams.composite_name}
            )
        )(wildcard())

    def callback(
        self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map
    ) -> tvm.relay.Expr:
        params = ethosu_patterns.QnnDepthwiseConv2DParams(post.op.body)
        params.ifm.tensor = post.args[0]
        channels_map = {
            "NHWC": 3,
        }
        if str(params.ofm.layout) not in channels_map.keys():
            raise UnsupportedLayout(str(params.ofm.layout))
        kernel_shape_map = {
            "HWOI": params.weights.shape[0:2],
        }
        if str(params.weights.layout) not in kernel_shape_map.keys():
            raise UnsupportedLayout(str(params.weights.layout))

        weights_values = params.weights.values
        weights_values_ohwi = np.moveaxis(weights_values, [0, 1, 2, 3], [1, 2, 0, 3])

        activation = "NONE"
        # Activations requiring LUT is currently not supported, so setting it to an empty list
        lut = relay.const([], "int8")
        clip_min = 0
        clip_max = 0
        if params.activation:
            activation = ethosu_patterns.QnnDepthwiseConv2DParams.activation_map[
                params.activation.op.name
            ]
            if activation == "CLIP":
                clip_min = int(params.activation.attrs.a_min)
                clip_max = int(params.activation.attrs.a_max)
        scale_bias = vela_api.pack_biases(
            biases=params.biases.tensor.data.asnumpy(),
            ifm_scale=params.ifm.q_params.scale_f32,
            ifm_dtype=np.dtype(params.ifm.dtype),
            weight_scales=params.weights.q_params.scale_f32,
            ofm_scale=params.ofm.q_params.scale_f32,
            is_activation_tanh_or_sigmoid=activation in ["TANH", "SIGMOID"],
        )

        ethosu_depthwise_conv2d = ethosu_ops.ethosu_depthwise_conv2d(
            post.args[0],  # IFM
            relay.const(weights_values_ohwi, params.weights.values.dtype),
            relay.const(scale_bias, "uint8"),
            lut,
            float(params.ifm.q_params.scale_f32),
            int(params.ifm.q_params.zero_point),
            int(params.weights.q_params.zero_point),
            float(params.ofm.q_params.scale_f32),
            int(params.ofm.q_params.zero_point),
            kernel_shape_map[str(params.weights.layout)],
            params.ofm.shape[channels_map[str(params.ofm.layout)]],
            strides=params.strides,
            padding=params.padding,
            dilation=params.dilation,
            activation=activation,
            clip_min=clip_min,
            clip_max=clip_max,
            upscale="NONE",
            ifm_layout=str(params.ifm.layout),
            ofm_layout=str(params.ofm.layout),
            ofm_dtype=str(params.ofm.dtype),
        )
        return ethosu_depthwise_conv2d


@ir.transform.module_pass(opt_level=1)
class LegalizeDepthwiseConv2D:
    """This is the pass that wraps the DepthwiseConv2DRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(DepthwiseConv2DRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class PoolingRewriter(DFPatternCallback):
    """Convert ethosu.avgpool2d and ethosu.maxpool2d composite functions to
    ethosu_pooling operators"""

    def __init__(
        self,
        params_class: Type,
        pattern: CallPattern,
    ):
        super().__init__(require_type=True)
        self.params_class = params_class
        self.pattern = pattern

    def callback(
        self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map
    ) -> tvm.relay.Expr:
        params = self.params_class(post.op.body)
        params.ifm.tensor = post.args[0]
        channels_map = {
            "NHWC": 3,
        }
        if str(params.ofm.layout) not in channels_map.keys():
            raise UnsupportedLayout(str(params.ofm.layout))

        activation_map = {"clip": "CLIP"}
        if params.activation:
            activation = activation_map[params.activation.op.name]
            clip_min = int(params.activation.attrs.a_min)
            clip_max = int(params.activation.attrs.a_max)
        else:
            activation = "NONE"
            clip_min = 0
            clip_max = 0

        # Activations requiring LUT is currently not supported, so setting it to an empty list
        lut = relay.const([], dtype="int8")

        return ethosu_ops.ethosu_pooling(
            ifm=post.args[0],
            lut=lut,
            pooling_type=params.pooling_type,
            ifm_scale=params.ifm.q_params.scale_f32,
            ifm_zero_point=params.ifm.q_params.zero_point,
            ofm_scale=params.ofm.q_params.scale_f32,
            ofm_zero_point=params.ofm.q_params.zero_point,
            pool_shape=params.pool_shape,
            ofm_channels=params.ofm.shape[channels_map[str(params.ofm.layout)]],
            strides=params.strides,
            padding=params.padding,
            activation=activation,
            clip_min=clip_min,
            clip_max=clip_max,
            upscale="NONE",
            ifm_layout=str(params.ifm.layout),
            ofm_layout=str(params.ofm.layout),
        )


class MaxPoolingRewriter(PoolingRewriter):
    def __init__(self):
        super().__init__(
            params_class=ethosu_patterns.MaxPool2DParams,
            pattern=(
                wildcard().has_attr({"Composite": ethosu_patterns.MaxPool2DParams.composite_name})
            )(wildcard()),
        )


@ir.transform.module_pass(opt_level=1)
class LegalizeMaxPooling:
    """This is the pass that wraps the MaxPoolingRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(MaxPoolingRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class AvgPoolingRewriter(PoolingRewriter):
    def __init__(self):
        super().__init__(
            params_class=ethosu_patterns.AvgPool2DParams,
            pattern=(
                wildcard().has_attr({"Composite": ethosu_patterns.AvgPool2DParams.composite_name})
            )(wildcard()),
        )


@ir.transform.module_pass(opt_level=1)
class LegalizeAvgPooling:
    """This is the pass that wraps the AvgPoolingRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(AvgPoolingRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class BinaryElementwiseRewriter(DFPatternCallback):
    """Convert ethosu binary elementwise composite functions to
    ethosu_binary_elementwise operators"""

    def __init__(
        self,
        params_class: Type,
        pattern: CallPattern,
    ):
        super().__init__(require_type=True)
        self.params_class = params_class
        self.pattern = pattern

    @staticmethod
    def reshape_input(
        inputs: List["TensorParams"],
    ) -> List[tvm.relay.Expr]:
        """Reshape the inputs so that the following binary elementwise
        operator receives 4-dimensional inputs.

        Parameters
        ----------
        inputs: List[TensorParams]
            The inputs to reshape.

        Returns
        -------
        reshaped_inputs: List[tvm.relay.Expr]
            The new reshaped inputs.
        """
        reshaped_inputs = []
        for i in inputs:
            in_shape = i.shape
            if len(in_shape) < 4:
                pad_size = 4 - len(in_shape)
                new_shape = ([1] * pad_size) + in_shape
                new_call = relay.reshape(i.tensor, new_shape)
                reshaped_inputs.append(new_call)
            else:
                reshaped_inputs.append(i.tensor)
        return reshaped_inputs

    @staticmethod
    def reshape_output(output: tvm.relay.Expr, ifm_input_shape: List[int]) -> tvm.relay.Expr:
        """Reshape the output back to the original dimensionality.
        Since the NPU must have the brodcastable tensor as the
        second operand, the original shape of the first ifm must
        be the output shape.

        Parameters
        ----------
        output: tvm.relay.Expr
            The output to reshape.

        ifm_input_shape: List[int]
            The shape of the non-reshaped ifm tensor.

        Returns
        -------
        reshaped_output: tvm.relay.Expr
            The reshaped output expression.
        """
        if len(ifm_input_shape) == 4:
            return output
        reshaped_output = relay.reshape(output, ifm_input_shape)
        return reshaped_output

    def callback(
        self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map
    ) -> tvm.relay.Expr:
        params = self.params_class(post.op.body)
        params.ifm.tensor = post.args[1] if params.reversed_operands else post.args[0]
        params.ifm2.tensor = post.args[0] if params.reversed_operands else post.args[1]
        channels_map = {
            "NHWC": 3,
        }
        if str(params.ofm.layout) not in channels_map.keys():
            raise UnsupportedLayout(str(params.ofm.layout))

        activation_map = {"clip": "CLIP"}
        if params.activation:
            activation = activation_map[params.activation.op.name]
            clip_min = int(params.activation.attrs.a_min)
            clip_max = int(params.activation.attrs.a_max)
        else:
            activation = "NONE"
            clip_min = 0
            clip_max = 0

        # We don't yet support activation functions that need to get legalized to LUTs.
        lut = relay.const([], dtype="int8")

        inputs = [params.ifm, params.ifm2]
        inputs = self.reshape_input(inputs)

        ethosu_binary_elementwise = ethosu_ops.ethosu_binary_elementwise(
            ifm=inputs[0],
            ifm2=inputs[1],
            lut=lut,
            operator_type=params.operator_type,
            ifm_scale=float(params.ifm.q_params.scale_f32),
            ifm_zero_point=int(params.ifm.q_params.zero_point),
            ifm2_scale=float(params.ifm2.q_params.scale_f32),
            ifm2_zero_point=int(params.ifm2.q_params.zero_point),
            ofm_scale=float(params.ofm.q_params.scale_f32),
            ofm_zero_point=int(params.ofm.q_params.zero_point),
            ifm_channels=params.ifm.shape[-1],
            ifm2_channels=params.ifm2.shape[-1],
            reversed_operands=params.reversed_operands,
            ofm_dtype=params.ofm.dtype,
            activation=activation,
            clip_min=clip_min,
            clip_max=clip_max,
            ifm_layout=str(params.ifm.layout),
            ifm2_layout=str(params.ifm2.layout),
            ofm_layout=str(params.ofm.layout),
        )
        output = self.reshape_output(ethosu_binary_elementwise, params.ifm.shape)
        return output


class AddRewriter(BinaryElementwiseRewriter):
    def __init__(self):
        super().__init__(
            params_class=ethosu_patterns.AddParams,
            pattern=(wildcard().has_attr({"Composite": ethosu_patterns.AddParams.composite_name}))(
                wildcard(), wildcard()
            ),
        )


@ir.transform.module_pass(opt_level=1)
class LegalizeAdd:
    """This is the pass that wraps the AddRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(AddRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class SubRewriter(BinaryElementwiseRewriter):
    def __init__(self):
        super().__init__(
            params_class=ethosu_patterns.SubParams,
            pattern=(wildcard().has_attr({"Composite": ethosu_patterns.SubParams.composite_name}))(
                wildcard(), wildcard()
            ),
        )


@ir.transform.module_pass(opt_level=1)
class LegalizeSub:
    """This is the pass that wraps the SubRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(SubRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class MulRewriter(BinaryElementwiseRewriter):
    def __init__(self):
        super().__init__(
            params_class=ethosu_patterns.MulParams,
            pattern=(wildcard().has_attr({"Composite": ethosu_patterns.MulParams.composite_name}))(
                wildcard(), wildcard()
            ),
        )


@ir.transform.module_pass(opt_level=1)
class LegalizeMul:
    """This is the pass that wraps the MulRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(MulRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class MinRewriter(BinaryElementwiseRewriter):
    def __init__(self):
        super().__init__(
            params_class=ethosu_patterns.MinParams,
            pattern=(wildcard().has_attr({"Composite": ethosu_patterns.MinParams.composite_name}))(
                wildcard(), wildcard()
            ),
        )


@ir.transform.module_pass(opt_level=1)
class LegalizeMin:
    """This is the pass that wraps the MinRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(MinRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class MaxRewriter(BinaryElementwiseRewriter):
    def __init__(self):
        super().__init__(
            params_class=ethosu_patterns.MaxParams,
            pattern=(wildcard().has_attr({"Composite": ethosu_patterns.MaxParams.composite_name}))(
                wildcard(), wildcard()
            ),
        )


@ir.transform.module_pass(opt_level=1)
class LegalizeMax:
    """This is the pass that wraps the MaxRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(MaxRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class ShlRewriter(BinaryElementwiseRewriter):
    def __init__(self):
        super().__init__(
            params_class=ethosu_patterns.ShlParams,
            pattern=(wildcard().has_attr({"Composite": ethosu_patterns.ShlParams.composite_name}))(
                wildcard(), wildcard()
            ),
        )


@ir.transform.module_pass(opt_level=1)
class LegalizeShl:
    """This is the pass that wraps the ShlRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(ShlRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class StridedSliceRewriter(DFPatternCallback):
    """This pass brings the strided slice out of the partitioned function"""

    def __init__(self):
        super().__init__(require_type=True, rewrite_once=True)
        self.pattern = (
            wildcard().has_attr({"Composite": ethosu_patterns.StridedSliceParams.composite_name})
        )(wildcard())

    def callback(
        self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map
    ) -> tvm.relay.Expr:

        slice_input = post.args[0]
        params = ethosu_patterns.StridedSliceParams(post.op.body)
        strided_slice = relay.op.strided_slice(
            slice_input,
            params.begin,
            params.end,
            strides=params.strides,
            axes=params.axes,
            slice_mode=params.slice_mode,
        )
        return strided_slice


@ir.transform.module_pass(opt_level=1)
class LegalizeStridedSlice:
    """This is the pass that wraps StridedSliceRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(StridedSliceRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class ReshapeRewriter(DFPatternCallback):
    """This pass brings the reshape out of the partitioned function"""

    def __init__(self):
        super().__init__(require_type=True, rewrite_once=True)
        self.pattern = (
            wildcard().has_attr({"Composite": ethosu_patterns.ReshapeParams.composite_name})
        )(wildcard())

    def callback(
        self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map
    ) -> tvm.relay.Expr:
        reshape_input = post.args[0]
        reshape_params = ethosu_patterns.ReshapeParams(post.op.body)
        new_shape = reshape_params.new_shape
        return relay.op.reshape(reshape_input, newshape=new_shape)


@ir.transform.module_pass(opt_level=1)
class LegalizeReshape:
    """This is the pass that wraps ReshapeRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(ReshapeRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class NoOpRewriter(DFPatternCallback):
    """This pass adds an idenity operator to reshape and strided slice to avoid a no op
    without a consumer"""

    def __init__(self):
        super().__init__(require_type=True, rewrite_once=True)
        self.reshape = is_op("reshape")(wildcard())
        self.strided_slice = is_op("strided_slice")(wildcard())
        self.pattern = self.reshape | self.strided_slice

    def callback(
        self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map
    ) -> tvm.relay.Expr:
        if pre.checked_type.dtype == "int32":
            return post
        return ethosu_ops.ethosu_identity(ifm=post, lut=relay.const([], dtype="int8"))


@ir.transform.module_pass(opt_level=1)
class LegalizeNoOps:
    """This is the pass that wraps RewriteNoOps"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(NoOpRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class UnaryElementwiseRewriter(DFPatternCallback):
    """
    Convert ethosu unary elementwise composite function to
    ethosu_unary_elementwise operators
    """

    def __init__(self, params_class: Type, pattern: CallPattern):
        super().__init__(require_type=True)
        self.params_class = params_class
        self.pattern = pattern

    def callback(
        self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map
    ) -> tvm.relay.Expr:
        params = self.params_class(post.op.body)
        params.ifm.tensor = post.args[0]

        if str(params.ofm.layout) != "NHWC":
            raise UnsupportedLayout(str(params.ofm.layout))

        activation_map = {"clip": "CLIP"}
        if params.activation:
            activation = activation_map[params.activation.op.name]
            clip_min = int(params.activation.attrs.a_min)
            clip_max = int(params.activation.attrs.a_max)
        else:
            activation = "NONE"
            clip_min = 0
            clip_max = 0

        # We don't yet support activation functions that use LUT.
        lut = relay.const([], dtype="int8")

        unary_input_shape = params.ifm.shape
        # If the input tensor is not 4D, enter reshapes before and after the unary operator
        if len(params.ifm.shape) == 4:
            unary_input = params.ifm.tensor
        else:
            pad_size = 4 - len(unary_input_shape)
            unary_input_shape = ([1] * pad_size) + unary_input_shape
            unary_input = relay.op.reshape(params.ifm.tensor, newshape=unary_input_shape)

        ethosu_unary_elementwise = ethosu_ops.ethosu_unary_elementwise(
            ifm=unary_input,
            lut=lut,
            operator_type=params.operator_type,
            ifm_scale=float(params.ifm.q_params.scale_f32),
            ifm_zero_point=int(params.ifm.q_params.zero_point),
            ofm_scale=float(params.ofm.q_params.scale_f32),
            ofm_zero_point=int(params.ofm.q_params.zero_point),
            ofm_channels=unary_input_shape[3],
            activation=activation,
            clip_min=clip_min,
            clip_max=clip_max,
            ifm_layout=str(params.ifm.layout),
            ofm_layout=str(params.ofm.layout),
        )
        if len(params.ifm.shape) == 4:
            op = ethosu_unary_elementwise
        else:
            op = relay.op.reshape(ethosu_unary_elementwise, newshape=params.ifm.shape)
        return op


class AbsRewriter(UnaryElementwiseRewriter):
    def __init__(self):
        super().__init__(
            params_class=ethosu_patterns.AbsParams,
            pattern=(wildcard().has_attr({"Composite": ethosu_patterns.AbsParams.composite_name}))(
                wildcard()
            ),
        )


@ir.transform.module_pass(opt_level=1)
class LegalizeAbs:
    """This is the pass that wraps the AbsRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(AbsRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class MeanRewriter(DFPatternCallback):
    """Convert ethosu.mean composite functions to to an equivalent legalization:
    - Case 1 (axis == [1, 2] and keepsdims == True):
        ethosu_depthwise_conv2d + ethosu_binary_elementwise
    - Case 2 (ifm qparams == ofm qparams): ethosu_pooling
    - Case 3 (else): ethosu_depthwise_conv2d
    """

    def __init__(self):
        super().__init__(require_type=True)
        self.pattern = (
            wildcard().has_attr({"Composite": ethosu_patterns.MeanParams.composite_name})
        )(wildcard())

    def callback(
        self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map
    ) -> tvm.relay.Expr:
        params = ethosu_patterns.MeanParams(post.op.body)
        params.ifm.tensor = post.args[0]

        ifm_shape = params.ifm.shape
        ofm_shape = params.ofm.shape
        lut = relay.const([], "int8")
        axis = params.axis
        reduced_op = params.ifm.tensor

        # Enforce 4d input
        if len(ifm_shape) < 4:
            axis = [x + 1 for x in axis]
            if len(ifm_shape) == 3:
                ifm_shape = [1, params.height, params.width, ifm_shape[2]]
            else:
                ifm_shape = [1, params.height, params.width, 1]
            reduced_op = relay.reshape(reduced_op, ifm_shape)

        filter_height = ifm_shape[1] if 1 in axis else 1
        filter_width = ifm_shape[2] if 2 in axis else 1
        in_channels = out_channels = ifm_shape[-1]

        # If the height is greater than max kernel height, reshape the input
        # from [filter_height, filter_width] to [1, (filter_height*filter_width)]
        # only in the case the axis is [1, 2].
        if axis == [1, 2] and filter_height > 64:
            ifm_shape = (ifm_shape[0], 1, filter_height * filter_width, in_channels)
            filter_width = filter_height * filter_width
            filter_height = 1
            reduced_op = relay.reshape(reduced_op, ifm_shape)

        if axis == [1, 2] and params.keepdims:
            weight_scale = 1
            weight_values = np.ones([out_channels, filter_height, filter_width, in_channels])
            scale_bias = vela_api.pack_biases(
                biases=np.zeros(ifm_shape[-1]),
                ifm_scale=params.ifm.q_params.scale_f32,
                ifm_dtype=np.dtype(params.ifm.dtype),
                weight_scales=np.array([weight_scale], dtype=np.float),
                ofm_scale=params.ofm.q_params.scale_f32,
                is_activation_tanh_or_sigmoid=False,
            )

            reduced_op = ethosu_ops.ethosu_depthwise_conv2d(
                ifm=reduced_op,
                weight=relay.const(weight_values, params.ifm.dtype),
                scale_bias=relay.const(scale_bias, "uint8"),
                lut=lut,
                ifm_scale=float(params.ifm.q_params.scale_f32),
                ifm_zero_point=int(params.ifm.q_params.zero_point),
                weight_zero_point=0,
                ofm_scale=float(params.ofm.q_params.scale_f32),
                ofm_zero_point=int(params.ofm.q_params.zero_point),
                kernel_shape=(filter_height, filter_width),
                ofm_channels=out_channels,
                ofm_dtype="int16",
            )

            n = int(filter_height * filter_width)
            eps = 1 / (256 * (n + 1)) if n % 2 == 0 else 0

            scalar_tensor = relay.const(np.ones([1, 1, 1, 1], dtype="int16"), dtype="int16")

            reduced_op = ethosu_ops.ethosu_binary_elementwise(
                ifm=reduced_op,
                ifm2=scalar_tensor,
                lut=lut,
                operator_type="MUL",
                ifm_scale=float(params.ofm.q_params.scale_f32),
                ifm_zero_point=int(params.ofm.q_params.zero_point),
                ifm2_scale=1 / (n - eps),
                ifm2_zero_point=0,
                ofm_scale=float(params.ofm.q_params.scale_f32),
                ofm_zero_point=int(params.ofm.q_params.zero_point),
                ifm_channels=out_channels,
                ifm2_channels=out_channels,
                reversed_operands=False,
                ofm_dtype="int8",
                rounding_mode="NATURAL",
            )
        elif (
            params.ifm.q_params.scale_f32 == params.ofm.q_params.scale_f32
            and params.ifm.q_params.zero_point == params.ofm.q_params.zero_point
        ):
            reduced_op = ethosu_ops.ethosu_pooling(
                ifm=reduced_op,
                lut=lut,
                pooling_type="AVG",
                ifm_scale=float(params.ifm.q_params.scale_f32),
                ifm_zero_point=0,
                ofm_scale=float(params.ofm.q_params.scale_f32),
                ofm_zero_point=0,
                pool_shape=(filter_height, filter_width),
                ofm_channels=out_channels,
                rounding_mode="TRUNCATE",
            )
        else:
            weight_scale = 1 / (filter_height * filter_width)
            weight_values = np.ones([out_channels, filter_height, filter_width, in_channels])
            bias = -1 * int(params.ifm.q_params.zero_point) * filter_height * filter_width

            scale_bias = vela_api.pack_biases(
                biases=np.ones([ifm_shape[-1]]) * bias,
                ifm_scale=params.ifm.q_params.scale_f32,
                ifm_dtype=np.dtype(params.ifm.dtype),
                weight_scales=np.array([weight_scale], dtype=np.float),
                ofm_scale=params.ofm.q_params.scale_f32,
                is_activation_tanh_or_sigmoid=False,
            )
            reduced_op = ethosu_ops.ethosu_depthwise_conv2d(
                ifm=reduced_op,
                weight=relay.const(weight_values, params.ifm.dtype),
                scale_bias=relay.const(scale_bias, "uint8"),
                lut=lut,
                ifm_scale=float(params.ifm.q_params.scale_f32),
                ifm_zero_point=0,
                weight_zero_point=0,
                ofm_scale=float(params.ofm.q_params.scale_f32),
                ofm_zero_point=int(params.ofm.q_params.zero_point),
                kernel_shape=(filter_height, filter_width),
                ofm_channels=out_channels,
                rounding_mode="NATURAL",
            )

        # Reshape to original ofm shape
        if len(ofm_shape) < 4:
            reduced_op = relay.reshape(reduced_op, ofm_shape)

        return reduced_op


@ir.transform.module_pass(opt_level=1)
class LegalizeMean:
    """This is the pass that wraps the MeanRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(MeanRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


class ConcatRewriter(DFPatternCallback):
    """The newer versions of TFLite converters return a concatenate operator that concatenates
    tensors with same QNN params (if the QNN params of tensors were initially different,
    the converter adds a requantize node), so this rewriter replaces the QNN concatenate with
    "normal" concatenate"""

    def __init__(self):
        super().__init__(require_type=True, rewrite_once=True)
        self.pattern = (
            wildcard().has_attr({"Composite": ethosu_patterns.ConcatParams.composite_name})
        )(None)

    def callback(
        self, pre: tvm.relay.Expr, post: tvm.relay.Expr, node_map: tvm.ir.container.Map
    ) -> tvm.relay.Expr:
        # Find the tensors that are inputs to the concat and the scales and zero points
        concat_args = list()
        for arg in post.args:
            if isinstance(arg, tvm.relay.expr.Call):
                concat_args.append(arg)

        axis = post.op.body.attrs.axis
        concat = relay.op.concatenate(relay.Tuple(concat_args), axis=axis)
        return concat


@ir.transform.module_pass(opt_level=1)
class LegalizeConcat:
    """This is the pass that wraps ConcatRewriter"""

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        for global_var, func in mod.functions.items():
            func = rewrite(ConcatRewriter(), func)
            mod.update_func(global_var, func)
        return mod

    def __call__(self, *args, **kwargs):
        pass


@ir.transform.module_pass(opt_level=1)
class LegalizeEthosU:
    """This is the pass to call graph-rewrites to perform graph transformation
    in a way such that the operations are replaced with hardware/codegen supported
    operations.
    """

    def transform_module(
        self, mod: tvm.ir.IRModule, ctx: tvm.ir.transform.PassContext
    ) -> tvm.ir.IRModule:
        """This is the method that replaces the operations with hardware/codegen supported
        operations.
        """
        mod = LegalizeSplit()(mod)
        mod = LegalizeConv2D()(mod)
        mod = LegalizeDepthwiseConv2D()(mod)
        mod = LegalizeMaxPooling()(mod)
        mod = LegalizeAvgPooling()(mod)
        mod = LegalizeAdd()(mod)
        mod = LegalizeSub()(mod)
        mod = LegalizeMul()(mod)
        mod = LegalizeMin()(mod)
        mod = LegalizeMax()(mod)
        mod = LegalizeShl()(mod)
        mod = LegalizeAbs()(mod)
        mod = LegalizeTanh()(mod)
        mod = LegalizeMean()(mod)
        mod = LegalizeConcat()(mod)
        mod = LegalizeSigmoid()(mod)
        mod = LegalizeReshape()(mod)
        mod = LegalizeStridedSlice()(mod)
        mod = LegalizeNoOps()(mod)
        return mod

    def __call__(self, *args, **kwargs):
        # pylint is unable figure out the decorated
        # class is callable, thus adding this to
        # suppress the warning.
        pass
