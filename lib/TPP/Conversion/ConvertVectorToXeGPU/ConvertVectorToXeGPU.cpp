//===- ConvertVectorToXeGPU.cpp ----------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TPP/Passes.h"
#include "TPP/Transforms/Utils/ValueUtils.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/Dialect/XeGPU/IR/XeGPU.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include "llvm/ADT/TypeSwitch.h"

#include <algorithm>
#include <optional>

using namespace mlir;

namespace mlir {
namespace tpp {
#define GEN_PASS_DEF_CONVERTVECTORTOXEGPU
#include "TPP/Passes.h.inc"
} // namespace tpp
} // namespace mlir

namespace {

static bool isZeroConstant(Value val) {
  auto constant = val.getDefiningOp<arith::ConstantOp>();
  if (!constant)
    return false;

  return TypeSwitch<Attribute, bool>(constant.getValue())
      .Case<FloatAttr>([](auto attr) { return attr.getValue().isZero(); })
      .Case<IntegerAttr>([](auto attr) { return attr.getValue().isZero(); })
      .Default([](auto attr) { return false; });
}

static LogicalResult transferPreconditions(PatternRewriter &rewriter,
                                           VectorTransferOpInterface xferOp) {
  if (xferOp.getMask())
    return rewriter.notifyMatchFailure(xferOp,
                                       "Masked transfer is not supported");

  auto srcTy = dyn_cast<MemRefType>(xferOp.getShapedType());
  if (!srcTy)
    return rewriter.notifyMatchFailure(xferOp, "Expects memref source");
  VectorType vecTy = xferOp.getVectorType();
  unsigned vecRank = vecTy.getRank();
  if (!(vecRank == 1 || vecRank == 2))
    return rewriter.notifyMatchFailure(xferOp,
                                       "Expects 1D or 2D vector result");

  SmallVector<int64_t> strides;
  int64_t offset;
  if (failed(getStridesAndOffset(srcTy, strides, offset)) ||
      strides.back() != 1)
    return rewriter.notifyMatchFailure(
        xferOp, "Buffer must be contiguous in the innermost dimension");

  AffineMap map = xferOp.getPermutationMap();
  if (!map.isProjectedPermutation(/*allowZeroInResults=*/false))
    return rewriter.notifyMatchFailure(xferOp, "Unsupported permutation map");
  unsigned numInputDims = map.getNumInputs();
  for (auto expr : map.getResults().take_back(vecRank)) {
    auto dim = dyn_cast<AffineDimExpr>(expr);
    if (dim.getPosition() < (numInputDims - vecRank))
      return rewriter.notifyMatchFailure(
          xferOp, "Only innermost dimensions can be accessed");
  }

  return success();
}

static xegpu::CreateNdDescOp
createNdDescriptor(PatternRewriter &rewriter, Location loc,
                   xegpu::TensorDescType descType, TypedValue<MemRefType> src,
                   Operation::operand_range offsets) {
  MemRefType srcTy = src.getType();
  auto [strides, offset] = getStridesAndOffset(srcTy);

  xegpu::CreateNdDescOp ndDesc;
  if (srcTy.hasStaticShape()) {
    ndDesc = rewriter.create<xegpu::CreateNdDescOp>(loc, descType, src,
                                                    getAsOpFoldResult(offsets));
  } else {
    SmallVector<Value> sourceDims;
    unsigned srcRank = srcTy.getRank();
    for (unsigned i = 0; i < srcRank; ++i)
      sourceDims.push_back(rewriter.create<memref::DimOp>(loc, src, i));

    SmallVector<int64_t> constOffsets;
    SmallVector<Value> dynOffsets;
    for (auto offset : offsets) {
      std::optional<int64_t> staticVal = getConstantIntValue(offset);
      if (!staticVal)
        dynOffsets.push_back(offset);
      constOffsets.push_back(staticVal ? *staticVal : ShapedType::kDynamic);
    }

    SmallVector<Value> dynShapes;
    for (auto [idx, shape] : llvm::enumerate(srcTy.getShape())) {
      if (shape == ShapedType::kDynamic)
        dynShapes.push_back(sourceDims[idx]);
    }

    // Compute strides in reverse order.
    SmallVector<Value> dynStrides;
    Value accStride = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    // Last stride is guaranteed to be static and unit.
    for (int i = static_cast<int>(strides.size()) - 2; i >= 0; --i) {
      accStride =
          rewriter.create<arith::MulIOp>(loc, accStride, sourceDims[i + 1]);
      if (strides[i] == ShapedType::kDynamic)
        dynStrides.push_back(accStride);
    }
    std::reverse(dynStrides.begin(), dynStrides.end());

    ndDesc = rewriter.create<xegpu::CreateNdDescOp>(
        loc, descType, src, dynOffsets, dynShapes, dynStrides,
        DenseI64ArrayAttr::get(rewriter.getContext(), constOffsets),
        DenseI64ArrayAttr::get(rewriter.getContext(), srcTy.getShape()),
        DenseI64ArrayAttr::get(rewriter.getContext(), strides));
  }

  return ndDesc;
}

struct TransferReadLowering : public OpRewritePattern<vector::TransferReadOp> {
  using OpRewritePattern<vector::TransferReadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(vector::TransferReadOp readOp,
                                PatternRewriter &rewriter) const override {
    Location loc = readOp.getLoc();

    if (failed(transferPreconditions(rewriter, readOp)))
      return failure();

    bool isOutOfBounds = readOp.hasOutOfBoundsDim();
    if (isOutOfBounds && !isZeroConstant(readOp.getPadding()))
      return rewriter.notifyMatchFailure(
          readOp, "Unsupported non-zero padded out-of-bounds access");

    AffineMap readMap = readOp.getPermutationMap();
    bool isTransposeLoad = !readMap.isMinorIdentity();

    VectorType vecTy = readOp.getVectorType();
    Type elementType = vecTy.getElementType();
    unsigned minTransposeBitWidth = 32;
    if (isTransposeLoad &&
        elementType.getIntOrFloatBitWidth() < minTransposeBitWidth)
      return rewriter.notifyMatchFailure(
          readOp, "Unsupported data type for tranposition");

    // If load is transposed, get the base shape for the tensor descriptor.
    SmallVector<int64_t> descShape{vecTy.getShape()};
    if (isTransposeLoad)
      std::reverse(descShape.begin(), descShape.end());
    auto descType = xegpu::TensorDescType::get(
        descShape, elementType, /*scattered=*/false, /*array_length=*/1,
        xegpu::MemoryScope::Global,
        /*boundary_check=*/isOutOfBounds);

    xegpu::CreateNdDescOp ndDesc =
        createNdDescriptor(rewriter, loc, descType,
                           dyn_cast<TypedValue<MemRefType>>(readOp.getSource()),
                           readOp.getIndices());

    auto transposeAttr = !isTransposeLoad
                             ? nullptr
                             : DenseI64ArrayAttr::get(rewriter.getContext(),
                                                      ArrayRef<int64_t>{1, 0});
    // By default, no specific caching policy is assigned.
    xegpu::CachePolicyAttr hint = nullptr;
    auto loadOp = rewriter.create<xegpu::LoadNdOp>(
        loc, vecTy, ndDesc, /*packed=*/nullptr, transposeAttr,
        /*l1_hint=*/hint,
        /*l2_hint=*/hint, /*l3_hint=*/hint);
    rewriter.replaceOp(readOp, loadOp);

    return success();
  }
};

struct TransferWriteLowering
    : public OpRewritePattern<vector::TransferWriteOp> {
  using OpRewritePattern<vector::TransferWriteOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(vector::TransferWriteOp writeOp,
                                PatternRewriter &rewriter) const override {
    return success();
  }
};

struct ContractionLowering : public OpRewritePattern<vector::ContractionOp> {
  using OpRewritePattern<vector::ContractionOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(vector::ContractionOp contract,
                                PatternRewriter &rewriter) const override {
    return success();
  }
};

struct ConvertVectorToXeGPU
    : public tpp::impl::ConvertVectorToXeGPUBase<ConvertVectorToXeGPU> {
  void runOnOperation() override {
    auto *ctx = &getContext();
    RewritePatternSet patterns(ctx);
    patterns
        .add<TransferReadLowering, TransferWriteLowering, ContractionLowering>(
            ctx);
    (void)applyPatternsAndFoldGreedily(getOperation(), std::move(patterns));
  }
};

} // end namespace