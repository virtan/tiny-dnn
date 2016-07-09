/*
    Copyright (c) 2016, Taiga Nomi, Edgar Riba
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once

#include "tiny_cnn/core/params/conv_params.h"
#include "tiny_cnn/core/kernels/tiny_quantization_kernel.h"

namespace tiny_cnn {
namespace core {
namespace kernels {

void tiny_quantized_conv2d_kernel(const conv_params& params,
                        const vec_t&       in,
                        const vec_t&       W,
                        const vec_t&       bias,
                        vec_t&             a,
                        const bool layer_parallelize) {
    // image quantization
    float min_input(in[0]);
    float max_input(in[0]);
    for (cnn_size_t inc = 0; inc < params.in.depth_; inc++) {
        for (cnn_size_t ins = 0; ins < params.in_padded.height_*params.in_padded.height_; ins++) {
            cnn_size_t idx = params.in_padded.get_index(0, 0, inc);
            min_input = std::min(min_input, (&in[idx])[ins]);
            max_input = std::max(max_input, (&in[idx])[ins]);
        }
    }
    std::vector<uint8_t> in_quantized =
        float_tensor_to_quantized<uint8_t>(in, min_input, max_input);
    // filter quantization
    float min_filter(W[0]);
    float max_filter(W[0]);
    for (cnn_size_t inc = 0; inc < params.in_padded.depth_; inc++) {
        for (cnn_size_t ins = 0; ins < params.weight.height_*params.weight.height_; ins++) {
            cnn_size_t idx = params.in_padded.get_index(0, 0, inc);
            min_filter = std::min(min_filter, (&W[idx])[ins]);
            max_filter = std::max(max_filter, (&W[idx])[ins]);
        }
    }
    if (min_filter == max_filter) {
      max_filter = W[0] + 1e-3f;
      min_filter = W[0] - 1e-3f;
    }
    std::vector<uint8_t> W_quantized =
        float_tensor_to_quantized<uint8_t>(W, min_filter, max_filter);
    // bias quantization
    float min_bias(0);
    float max_bias(0);
    std::vector<uint8_t> bias_quantized;
    if (params.has_bias) {
        for (cnn_size_t inc = 0; inc < params.out.depth_; inc++) {
            min_bias = std::min(min_bias, bias[inc]);
            max_bias = std::max(max_bias, bias[inc]);
        }
        if (min_bias == max_bias) {
          max_bias = bias[0] + 1e-3f;
          min_bias = bias[0] - 1e-3f;
        }
        bias_quantized =
            float_tensor_to_quantized<uint8_t>(bias, min_bias, max_bias);
    }
    // output range
    float min_output_value;
    float max_output_value;
    quantization_range_for_multiplication<uint8_t, uint8_t, int32_t>(
        min_input, max_input, min_filter, max_filter, &min_output_value,
        &max_output_value);

    std::vector<int32_t> a_quantized(a.size(), static_cast<int32_t>(0));

    // calculating offset
    const int32_t offset_input =
        float_to_quantized_unclamped<uint8_t>(0.0f, min_input, max_input);
    const int32_t offset_filter =
        float_to_quantized_unclamped<uint8_t>(0.0f, min_filter, max_filter);
    const int32_t zero_in_total_space =
        float_to_quantized<int32_t>(0.0f, min_output_value, max_output_value);

    const int32_t offset_output = 0;
    const int32_t mult_output = 1;
    const int32_t shift_output = 0;

    const int32_t rounding = (shift_output < 1) ? 0 : (1 << (shift_output - 1));
    const int32_t highest_ = static_cast<int32_t>(highest<uint8_t>());
    const int32_t lowest_ = static_cast<int32_t>(lowest<uint8_t>());

    for_i(layer_parallelize, params.out.depth_, [&](int o) {
        for (cnn_size_t inc = 0; inc < params.in.depth_; inc++) {
            if (!params.tbl.is_connected(o, inc)) continue;

            cnn_size_t idx = 0;
            idx = params.in.depth_ * o + inc;
            idx = params.weight.get_index(0, 0, idx);
            const uint8_t *pw = &W_quantized[idx];

            idx = params.in_padded.get_index(0, 0, inc);
            const uint8_t *pi = &in_quantized[idx];

            idx = params.out.get_index(0, 0, o);
            int32_t *pa_quantized = &a_quantized[idx];

            for (cnn_size_t y = 0; y < params.out.height_; y++) {
                for (cnn_size_t x = 0; x < params.out.width_; x++) {
                    const uint8_t * ppw = pw;
                    const uint8_t * ppi = pi + params.in_padded.width_ *
                                        (y * params.h_stride) +
                                         x * params.w_stride;
                    int32_t sum = 0;

                    // should be optimized for small kernel(3x3,5x5)
                    for (cnn_size_t wy = 0; wy < params.weight.height_; wy++) {
                        for (cnn_size_t wx = 0; wx < params.weight.width_; wx++) {
                            idx = wy * params.in_padded.width_ + wx;
                            sum += (static_cast<int32_t>(*ppw++) - offset_filter)
                                    * (static_cast<int32_t>(ppi[idx]) - offset_input);
                        }
                    }
                    // here we can consider whether to choose the clamped_output or not;
                    const int32_t output =
                        ((((sum + offset_output) * mult_output) + rounding) >>
                         shift_output);
                    const int32_t top_clamped_output = std::min<int32_t>(output, highest_);
                    const int32_t clamped_output = std::max<int32_t>(top_clamped_output, lowest_);
                    pa_quantized[y * params.out.width_ + x] += output;
                }
            }
        }
        if (params.has_bias) {
            int32_t * pa_quantized  = &a_quantized[params.out.get_index(0, 0, o)];
            int32_t * paa_quantized = pa_quantized + params.out.width_ * params.out.height_;
            std::for_each(pa_quantized, paa_quantized, [&](int32_t& f) {
                f += (bias_quantized[o] - zero_in_total_space);
            });
        }
    });

    float min_output_requantized;
    float max_output_requantized;
    std::vector<uint8_t> a_requantized(a_quantized.size(), static_cast<uint8_t>(0));

    // Requantize from 32bits to 8 bits for next layer
    quantize_down_and_shrink_range<int32_t, uint8_t>(a_quantized, min_output_value, max_output_value,
    &min_output_requantized, &max_output_requantized, &a_requantized);

    // dequantize to flaot, this could be removed within concatenated quantized network
    a = quantized_tensor_to_float<uint8_t>(a_requantized, min_output_requantized, max_output_requantized);
}

void tiny_quantized_conv2d_kernel(const conv_params& params,
                        const std::vector<uint8_t>&       in,
                        const std::vector<uint8_t>&       W,
                        const std::vector<uint8_t>&       bias,
                        const vec_t&                      in_r,
                        const vec_t&                      W_r,
                        const vec_t&                      b_r,
                        std::vector<uint8_t>&             a,
                        vec_t&                            a_r,
                        const bool layer_parallelize) {
    // filter
    float min_filter(W_r[0]);
    float max_filter(W_r[1]);
    if (W_r[0] == W_r[1]) {
      max_filter = W[0] + 1e-3f;
      min_filter = W[0] - 1e-3f;
    }
    // bias
    float min_bias(b_r[0]);
    float max_bias(b_r[1]);
    if (params.has_bias) {
        if (min_bias == max_bias) {
          max_bias = bias[0] + 1e-3f;
          min_bias = bias[0] - 1e-3f;
        }
    }
    // output range
    float min_output_value;
    float max_output_value;
    quantization_range_for_multiplication<uint8_t, uint8_t, int32_t>(
        in_r[0], in_r[1], min_filter, max_filter, &min_output_value,
        &max_output_value);

    std::vector<int32_t> a_quantized(a.size(), static_cast<int32_t>(0));

    // calculating offset
    const int32_t offset_input =
        float_to_quantized_unclamped<uint8_t>(0.0f, in_r[0], in_r[1]);
    const int32_t offset_filter =
        float_to_quantized_unclamped<uint8_t>(0.0f, min_filter, max_filter);
    const int32_t zero_in_total_space =
        float_to_quantized<int32_t>(0.0f, min_output_value, max_output_value);

    const int32_t offset_output = 0;
    const int32_t mult_output = 1;
    const int32_t shift_output = 0;

    const int32_t rounding = (shift_output < 1) ? 0 : (1 << (shift_output - 1));
    const int32_t highest_ = static_cast<int32_t>(highest<uint8_t>());
    const int32_t lowest_ = static_cast<int32_t>(lowest<uint8_t>());

    for_i(layer_parallelize, params.out.depth_, [&](int o) {
        for (cnn_size_t inc = 0; inc < params.in.depth_; inc++) {
            if (!params.tbl.is_connected(o, inc)) continue;

            cnn_size_t idx = 0;
            idx = params.in.depth_ * o + inc;
            idx = params.weight.get_index(0, 0, idx);
            const uint8_t *pw = &W[idx];

            idx = params.in_padded.get_index(0, 0, inc);
            const uint8_t *pi = &in[idx];

            idx = params.out.get_index(0, 0, o);
            int32_t *pa_quantized = &a_quantized[idx];

            for (cnn_size_t y = 0; y < params.out.height_; y++) {
                for (cnn_size_t x = 0; x < params.out.width_; x++) {
                    const uint8_t * ppw = pw;
                    const uint8_t * ppi = pi + params.in_padded.width_ *
                                        (y * params.h_stride) +
                                         x * params.w_stride;
                    int32_t sum = 0;

                    // should be optimized for small kernel(3x3,5x5)
                    for (cnn_size_t wy = 0; wy < params.weight.height_; wy++) {
                        for (cnn_size_t wx = 0; wx < params.weight.width_; wx++) {
                            idx = wy * params.in_padded.width_ + wx;
                            sum += (static_cast<int32_t>(*ppw++) - offset_filter)
                                    * (static_cast<int32_t>(ppi[idx]) - offset_input);
                        }
                    }
                    // here we can consider whether to choose the clamped_output or not;
                    const int32_t output =
                        ((((sum + offset_output) * mult_output) + rounding) >>
                         shift_output);
                    const int32_t top_clamped_output = std::min<int32_t>(output, highest_);
                    const int32_t clamped_output = std::max<int32_t>(top_clamped_output, lowest_);
                    pa_quantized[y * params.out.width_ + x] += output;
                }
            }
        }
        if (params.has_bias) {
            int32_t * pa_quantized  = &a_quantized[params.out.get_index(0, 0, o)];
            int32_t * paa_quantized = pa_quantized + params.out.width_ * params.out.height_;
            std::for_each(pa_quantized, paa_quantized, [&](int32_t& f) {
                f += (bias[o] - zero_in_total_space);
            });
        }
    });

    float min_output_requantized;
    float max_output_requantized;
    std::vector<uint8_t> a_requantized(a_quantized.size(), static_cast<uint8_t>(0));

    // Requantize from 32bits to 8 bits for next layer
    quantize_down_and_shrink_range<int32_t, uint8_t>(a_quantized, min_output_value, max_output_value,
        &min_output_requantized, &max_output_requantized, &a_requantized);

    a = a_requantized;
    a_r[0] = min_output_requantized;
    a_r[1] = max_output_requantized;
}

}  // namespace kernels
}  // namespace core
}  // namespace tiny_cnn