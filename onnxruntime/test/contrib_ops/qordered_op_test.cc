// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "gtest/gtest.h"
#include "test/common/tensor_op_test_utils.h"
#include "test/common/cuda_op_test_utils.h"
#include "test/providers/provider_test_utils.h"
#include "test/util/include/scoped_env_vars.h"
#include "contrib_ops/cpu/bert/longformer_attention_base.h"

#include <cublasLt.h>

namespace onnxruntime {
namespace test {

template <typename T>
static std::vector<T> GenData(std::vector<int64_t> const & shape, float scale) {
  int64_t n = std::accumulate(shape.begin(), shape.end(), 1LL, std::multiply<int64_t>());
  std::vector<T> fp(n);
  for (int64_t i = 0; i < n; i++) {
    r[i] = static_cast<T>(((i % 256) - 128) * scale);
  }
  return r;
}

int64_t calcOrderIndex(cublasLtOrder_t order, int64_t rows, int64_t cols, int64_t r, int64_t c) {
  swithc (order) {
    case CUBLASLT_ORDER_ROW:
      return r * cols + c;
    case CUBLASLT_ORDER_COL:
      return c * rows + r;
    case CUBLASLT_ORDER_COL32:
      {
        int64_t tile_id = c / 32;
        int64_t tile_stride = 32 * rows;
        int64_t in_tile_c = c % 32;
        return tile_id * tile_stride + r * 32 + (c % 32);
      }
    case CUBLASLT_ORDER_COL4_4R2_8C:
      {
        int64_t tiles_c = c / 32;
        int64_t tiles_r = r / 8;
        int64_t tile_idx = tiles_c * (rows / 8) + tiles_r;
        int64_t tile_stride = 32 * 8;
        int64_t odd = r & 0x1;
        int64_t odd_stride = 32 * 4;
        int64_t in_4x4x8_tile_c = c % 32;
        int64_t in_4x4x8_tile_r = (r % 8) / 2;
        int64_t in_4x4x8_idx = (in_4x4x8_tile_c / 4) * (4*8) + in_4x4x8_tile_r * 4 + (in_4x4x8_tile_c % 4);
        return tile_idx * tile_stride + odd * odd_stride + in_4x4x8_idx;
      }
    case CUBLASLT_ORDER_COL32_2R_4R4:
    {
      // TODO:
    }
    default:
      return 0;
  }
}

template <typename TSrc>
static std::vector<int8_t> QuantizeTransform(std::vector<int64_t> const& shape, float scale, const std::vector<TSrc>& src, cublasLtOrder_t order) {
  int64_t cols = shape.back();
  int64_t rows = (shape.size() > 1 ? shape[shape.size() - 2] : 1LL);
  int64_t batch = (shape.size() <= 2 ? 1LL : std::accumulate(shape.data(), shape.data() + (shape.size() - 2), 1LL, std::multiply<int64_t>()));
  
  std::vector<int8_t> dst(batch * cols * rows);
  const TSrc* bsrc = src.data(;
  int8_t* bdst = dst.data();
  for (int64_t b = 0, batch_stride = rows * cols; b < batch; b++) {
    for (int64_t r = 0; r < rows; r++) {
      for (int64_t c = 0; c < cols; c++) {
        int64_t index_src = calcOrderIndex(CUBLASLT_ORDER_ROW, rows, cols, r, c);
        int64_t index_dst = calcOrderIndex(order, rows, cols, r, c);
        float v = (float)bsrc[index_src] * scale;
        v = std::max(TSrc(-128.0f), v);
        v = std::min(TSrc(127.0f), v);
        bdst[index_dst] = static_cast<int8_t>(std::round(v));
      }
    }
    bsrc += batch_stride;
    bdst += batch_stride;
  }
  return dst;
}

template <typename T>
static void RunQOrdered_Quantize_Test(
    std::vector<T> const& fvec,
    std::vector<int64_t> const& shape,
    cublasLtOrder_t order_q,
    T scale) {
  auto qvec = QuantizeTransform(shape, scale,fvec, order_q);

  std::vector<std::unique_ptr<IExecutionProvider>> execution_providers;
  execution_providers.push_back(DefaultCudaExecutionProvider());
  OpTester test_q("QuantizeWithOrder", 1, onnxruntime::kMSDomain);
  test_q.AddAttribute("order_input", (int64_t)CUBLASLT_ORDER_ROW);
  test_q.AddAttribute("order_output", (int64_t)order_q);
  test_q.template AddInput<T>("input", shape, fvec);
  test_q.AddInput<T>("scale_input", {}, {scale});
  test_q.AddOutput("output", shape, qvec);
  test_q.Run(OpTester::ExpectResult::kExpectSuccess, "", {}, nullptr, &execution_providers);
}

// template <typename T>
// static void RunQOrdered_Dequantize_Test(
//     std::vector<int8_t> const& qvec,
//     cublasLtOrder_t order_q,
//     std::vector<int64_t> const& shape,
//     std::vector<T> const& fvec,
//     cublasLtOrder_t order_f,
//     T scale) {
//   std::vector<std::unique_ptr<IExecutionProvider>> execution_providers;
//   execution_providers.push_back(DefaultCudaExecutionProvider());

//   OpTester test_dq("DequantizeWithOrder", 1, onnxruntime::kMSDomain);
//   test_dq.AddAttribute("order_input", (int64_t)order_q);
//   test_dq.AddAttribute("order_output", (int64_t)order_f);
//   test_dq.template AddInput("input", shape, qvec);
//   test_dq.AddInput<T>("scale_input", {}, {scale});
//   test_dq.AddOutput("output", shape, fvec);
//   test_dq.Run(OpTester::ExpectResult::kExpectSuccess, "", {}, nullptr, &execution_providers);
// }


TEST(QOrderedTest, FP32_Quantize_COL32) {
  std::vector<int64_t> shape = {2, 32 * 3, 32 * 2};
  float scale = 0.25f;
  std::vector<float> fvec = GenData(shape, scale);
  RunQOrdered_Quantize_Test(fvec, shape, CUBLASLT_ORDER_COL32, scale);
}

TEST(QOrderedTest, FP16_Quantize_COL32) {
  std::vector<int64_t> shape = {2, 32 * 3, 32 * 2};
  MLFloat16 scale(0.25f);
  std::vector<MLFloat16> fvec = GenData(shape, scale);
  RunQOrdered_Quantize_Test(fvec, shape, CUBLASLT_ORDER_COL32, scale);
}


}  // namespace test
}  // namespace onnxruntime
