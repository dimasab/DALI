// Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dali/core/static_switch.h"
#include "dali/kernels/imgproc/convolution/separable_convolution_cpu.h"
#include "dali/kernels/kernel_manager.h"
#include "dali/operators/image/convolution/gaussian_blur.h"
#include "dali/operators/image/convolution/gaussian_blur_params.h"
#include "dali/pipeline/data/views.h"
#include "dali/pipeline/operator/common.h"

namespace dali {

using namespace gaussian_blur;  // NOLINT

constexpr static const char* kSigmaArgName = "sigma";
constexpr static const char* kWindowSizeArgName = "window_size";

DALI_SCHEMA(GaussianBlur)
    .DocStr(R"code(Apply Gaussian Blur to the input.

User can specify sigma, kernel window size or both.
If only the sigma is provided, the radius of kernel is calculated as ``ceil(3 * sigma)``,
thus the kernel window size is ``2 * ceil(3 * sigma) + 1``.

If only the kernel window size is provided, the sigma is calculated using the following formula::

  radius = (window_size - 1) / 2
  sigma = (radius - 1) * 0.3 + 0.8

Both sigma and kernel window size can be specified as single value for all data axes
or per data axis.

When specifying the sigma or window size per axis, they are provided same as layouts: from outermost
to innermost.
The channel ``C`` and frame ``F`` dimensions are not considered data axes.
If channels are present only channel-first or channel-last inputs are supported.

For example, with ``HWC`` input, user can provide ``sigma=1.0`` or ``sigma=(1.0, 2.0)`` as there
are two data axes H and W.

The same input can be provided as per-sample tensors.
)code")
    .NumInput(1)
    .NumOutput(1)
    .AllowSequences()
    .SupportVolumetric()
    .AddOptionalArg<int>(kWindowSizeArgName, "The diameter of kernel.", std::vector<int>{0}, true)
    .AddOptionalArg<float>(kSigmaArgName, "Sigma value for Gaussian Kernel.",
                           std::vector<float>{0.f}, true)
    .AddOptionalArg(
        "dtype", "Output data type; if not set, the input type is used. Supported type: `FLOAT`.",
        DALI_NO_TYPE);

/**
 * @brief Fill the result span with the argument which can be provided as:
 * * ArgumentInput - {result.size()}-shaped Tensor
 * * ArgumentInput - {1}-shaped Tensor, the value will be replicated `result.size()` times
 * * Vector input - single "repeated argument" of length {result.size()} or {1}
 * * scalar argument - it will be replicated `result.size()` times
 *
 * TODO(klecki): we may want to make this a generic utility and propagate the span-approach to
 * the rest of the related argument gettters
 */
template <typename T>
void GetGeneralizedArg(span<T> result, const std::string name, int sample_idx, const OpSpec& spec,
                       const ArgumentWorkspace& ws) {
  int argument_length = result.size();
  if (spec.HasTensorArgument(name)) {
    const auto& tv = ws.ArgumentInput(name);
    const auto& tensor = tv[sample_idx];
    DALI_ENFORCE(tensor.shape().sample_dim() == 1,
                 make_string("Argument ", name, " for sample ", sample_idx,
                             " is expected to be 1D, got: ", tensor.shape().sample_dim(), "."));
    DALI_ENFORCE(tensor.shape()[0] == 1 || tensor.shape()[0] == argument_length,
                 make_string("Argument ", name, " for sample ", sample_idx,
                             " is expected to have shape equal to {1} or {", argument_length,
                             "}, got: ", tensor.shape(), "."));
    if (tensor.shape()[0] == 1) {
      for (int i = 0; i < argument_length; i++) {
        result[i] = tensor.data<T>()[0];
      }
    } else {
      memcpy(result.data(), tensor.data<T>(), sizeof(T) * argument_length);
    }
    return;
  }
  std::vector<T> tmp;
  // we already handled the argument input, this handles spec-related arguments only
  GetSingleOrRepeatedArg(spec, tmp, name, argument_length);
  memcpy(result.data(), tmp.data(), sizeof(T) * argument_length);
}

template <int axes>
GaussianBlurParams<axes> GetSampleParams(int sample, const OpSpec& spec,
                                         const ArgumentWorkspace& ws) {
  GaussianBlurParams<axes> params;
  GetGeneralizedArg<float>(make_span(params.sigmas), kSigmaArgName, sample, spec, ws);
  GetGeneralizedArg<int>(make_span(params.window_sizes), kWindowSizeArgName, sample, spec, ws);
  for (int i = 0; i < axes; i++) {
    DALI_ENFORCE(
        !(params.sigmas[i] == 0 && params.window_sizes[i] == 0),
        make_string("`sigma` and `window_size` shouldn't be 0 at the same time for sample: ",
                    sample, ", axis: ", i, "."));
    DALI_ENFORCE(params.sigmas[i] >= 0,
                 make_string("`sigma` must have non-negative values, got ", params.sigmas[i],
                             " for sample: ", sample, ", axis: ", i, "."));
    DALI_ENFORCE(params.window_sizes[i] >= 0,
                 make_string("`window_size` must have non-negative values, got ",
                             params.window_sizes[i], " for sample: ", sample, ", axis : ", i, "."));
    if (params.window_sizes[i] == 0) {
      params.window_sizes[i] = SigmaToDiameter(params.sigmas[i]);
    } else if (params.sigmas[i] == 0.f) {
      params.sigmas[i] = DiameterToSigma(params.window_sizes[i]);
    }
  }
  return params;
}

DimDesc ParseAndValidateDim(int ndim, TensorLayout layout) {
  static constexpr int kMaxDim = 3;
  if (layout.empty()) {
    // assuming plain data with no channels
    DALI_ENFORCE(ndim <= kMaxDim,
                 make_string("Input data with empty layout cannot have more than ", kMaxDim,
                             " dimensions, got input with ", ndim, " dimensions."));
    return {0, ndim, false, false};
  }
  // not-empty layout
  int axes_start = 0;
  int axes_count = ndim;
  bool has_channels = ImageLayoutInfo::IsChannelLast(layout);
  if (has_channels) {
    axes_count--;
  }
  // Skip possible occurrences of 'C' or 'F' at the beggining
  TensorLayout layout_tmp = layout;
  while (ImageLayoutInfo::IsChannelFirst(layout_tmp) || VideoLayoutInfo::IsSequence(layout_tmp)) {
    axes_start++;
    axes_count--;
    layout_tmp = layout_tmp.sub(1);
  }
  if (!has_channels) {
    DALI_ENFORCE(!ImageLayoutInfo::HasChannel(layout_tmp),
                 make_string("Only channel-first or channel-last layouts are supported, got: ",
                             layout, "."));
  }
  DALI_ENFORCE(
      !VideoLayoutInfo::HasSequence(layout_tmp),
      make_string("For sequences, layout should begin with 'F' or 'CF', got: ", layout, "."));
  DALI_ENFORCE(
      axes_start <= 2,
      make_string("Found more the one occurrence of 'F' or 'C' axes in layout: ", layout, "."));
  DALI_ENFORCE(axes_count <= kMaxDim,
               make_string("Too many dimensions, found: ", axes_count,
                           " data axes, maximum supported is: ", kMaxDim, "."));
  return {axes_start, axes_count, has_channels, axes_start != 0};
}

// axes here is dimension of element processed by kernel - in case of sequence it's 1 less than the
// actual dim
template <typename Out, typename In, int axes, bool has_channels>
class GaussianBlurOpCpu : public OpImplBase<CPUBackend> {
 public:
  using Kernel = kernels::SeparableConvolutionCpu<Out, In, float, axes, has_channels>;
  static constexpr int ndim = Kernel::ndim;

  explicit GaussianBlurOpCpu(const OpSpec& spec, const DimDesc& dim_desc)
      : spec_(spec), batch_size_(spec.GetArgument<int>("batch_size")), dim_desc_(dim_desc) {}

  bool SetupImpl(std::vector<OutputDesc>& output_desc, const workspace_t<CPUBackend>& ws) override {
    const auto& input = ws.template InputRef<CPUBackend>(0);
    int nsamples = input.size();
    auto nthreads = ws.GetThreadPool().size();

    output_desc.resize(1);
    output_desc[0].type = TypeInfo::Create<Out>();
    output_desc[0].shape.resize(nsamples, input.shape().sample_dim());

    params_.resize(nsamples);
    windows_.resize(nsamples);

    kmgr_.template Resize<Kernel>(nthreads, nsamples);

    for (int i = 0; i < nsamples; i++) {
      params_[i] = GetSampleParams<axes>(i, spec_, ws);
      windows_[i].PrepareWindows(params_[i]);
      // We take only last `ndim` siginificant dimensions to handle sequences as well
      auto elem_shape = input[i].shape().template last<ndim>();
      auto& req = kmgr_.Setup<Kernel>(i, ctx_, elem_shape, params_[i].window_sizes);
      // The shape of data stays untouched
      output_desc[0].shape.set_tensor_shape(i, input[i].shape());
    }
    return true;
  }

  void RunImpl(workspace_t<CPUBackend>& ws) override {
    const auto& input = ws.template InputRef<CPUBackend>(0);
    auto& output = ws.template OutputRef<CPUBackend>(0);
    output.SetLayout(input.GetLayout());
    auto in_shape = input.shape();
    auto& thread_pool = ws.GetThreadPool();

    int nsamples = input.shape().num_samples();
    for (int sample_idx = 0; sample_idx < nsamples; sample_idx++) {
      const auto& shape = input[sample_idx].shape();
      auto elem_volume = volume(shape.begin() + dim_desc_.usable_axes_start, shape.end());

      int seq_elements = 1;
      int64_t stride = 0;
      if (dim_desc_.is_sequence) {
        seq_elements = volume(shape.begin(), shape.begin() + dim_desc_.usable_axes_start);
        stride = elem_volume;
      }
      for (int elem_idx = 0; elem_idx < seq_elements; elem_idx++) {
        thread_pool.AddWork(
            [this, &input, &output, sample_idx, elem_idx, stride](int thread_id) {
              auto gaussian_windows = windows_[sample_idx].GetWindows();
              auto elem_shape = input[sample_idx].shape().template last<ndim>();
              auto in_view = TensorView<StorageCPU, const In, ndim>{
                  input[sample_idx].template data<In>() + stride * elem_idx, elem_shape};
              auto out_view = TensorView<StorageCPU, Out, ndim>{
                  output[sample_idx].template mutable_data<Out>() + stride * elem_idx, elem_shape};
              // I need a context for that particular run (or rather matching the thread &
              // scratchpad)
              auto ctx = ctx_;
              kmgr_.Run<Kernel>(thread_id, sample_idx, ctx, out_view, in_view, gaussian_windows);
            }, elem_volume);
      }
    }
    thread_pool.RunAll();
  }

 private:
  OpSpec spec_;
  int batch_size_ = 0;
  DimDesc dim_desc_;

  kernels::KernelManager kmgr_;
  kernels::KernelContext ctx_;

  std::vector<GaussianBlurParams<axes>> params_;
  std::vector<GaussianWindows<axes>> windows_;
};

template <>
bool GaussianBlur<CPUBackend>::SetupImpl(std::vector<OutputDesc>& output_desc,
                                         const workspace_t<CPUBackend>& ws) {
  const auto& input = ws.template InputRef<CPUBackend>(0);
  auto layout = input.GetLayout();
  auto dim_desc = ParseAndValidateDim(input.shape().sample_dim(), layout);
  dtype_ = dtype_ != DALI_NO_TYPE ? dtype_ : input.type().id();
  DALI_ENFORCE(dtype_ == input.type().id() || dtype_ == DALI_FLOAT,
               "Output data type must be same as input, FLOAT or skipped (defaults to input type)");

  // clang-format off
  TYPE_SWITCH(input.type().id(), type2id, In, GAUSSIAN_BLUR_SUPPORTED_TYPES, (
    VALUE_SWITCH(dim_desc.usable_axes_count, AXES, GAUSSIAN_BLUR_SUPPORTED_AXES, (
      VALUE_SWITCH(static_cast<int>(dim_desc.has_channels), HAS_CHANNELS, (0, 1), (
        constexpr bool has_ch = HAS_CHANNELS;
        if (dtype_ == input.type().id()) {
          impl_ = std::make_unique<GaussianBlurOpCpu<In, In, AXES, has_ch>>(spec_, dim_desc);
        } else {
          impl_ = std::make_unique<GaussianBlurOpCpu<float, In, AXES, has_ch>>(spec_, dim_desc);
        }
      ), (DALI_FAIL("Got value different than {0, 1} when converting bool to int."))); // NOLINT
    ), DALI_FAIL("Axis count out of supported range."));  // NOLINT
  ), DALI_FAIL(make_string("Unsupported data type: ", input.type().id())));  // NOLINT
  // clang-format on

  return impl_->SetupImpl(output_desc, ws);
}

template <>
void GaussianBlur<CPUBackend>::RunImpl(workspace_t<CPUBackend>& ws) {
  impl_->RunImpl(ws);
}

DALI_REGISTER_OPERATOR(GaussianBlur, GaussianBlur<CPUBackend>, CPU);

}  // namespace dali
