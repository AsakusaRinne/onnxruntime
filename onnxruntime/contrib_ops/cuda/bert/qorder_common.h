// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/common/common.h"
#include "core/providers/cuda/cuda_kernel.h"
#include <cublasLt.h>

namespace onnxruntime {
namespace contrib {
namespace cuda {

using namespace onnxruntime::cuda;

cublasLtOrder_t GetCublasLtOrderAttr(const OpKernelInfo& info, const char* order_attr);

int64_t CalcLeadingDimensionLt(int64_t rows, int64_t cols, cublasLtOrder_t order);}

class QuantizeWithOrder final : public CudaKernel {
 public:
  QuantizeWithOrder(const OpKernelInfo& info);
  Status ComputeInternal(OpKernelContext* context) const override;

 private:
  cublasLtOrder_t order_input_;
  cublasLtOrder_t order_output_;
};

class DequantizeWithOrder final : public CudaKernel {
 public:
  DequantizeWithOrder(const OpKernelInfo& info);
  Status ComputeInternal(OpKernelContext* context) const override;

 private:
  cublasLtOrder_t order_input_;
  cublasLtOrder_t order_output_;
  ONNX_NAMESPACE::TensorProto_DataType to_;
};

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
