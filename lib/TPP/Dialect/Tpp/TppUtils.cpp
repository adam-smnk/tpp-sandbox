//===- TppUtils.cpp ----------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TPP/Dialect/Tpp/TppUtils.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Utils/Utils.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Value.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/TypeSwitch.h"

namespace mlir {
namespace tpp {
namespace utils {

// Prototypes
static bool isZeroTensor(Value op);
static bool isZeroTensor(Operation *defOp);

// taken from LinalgInterfaces.cpp
// Returns true if the use-def chain from `v` to `from` consists of 0 or more
// unary single-operand operations.
// TODO: relax to multi-operands with constants, which are technically unary ops
// as needed (e.g. add5).
static bool isChainOfUnaryOpsFrom(Value v, Value from) {
  while (true) {
    if (v == from)
      return true;
    Operation *op = v.getDefiningOp();
    if (!op || op->getNumOperands() != 1)
      return false;
    v = op->getOperand(0);
  };
}

// taken from LinalgInterfaces.cpp
// Returns the unique instance of OpType in `block` if it is indeed unique.
// Returns null if none or more than 1 instances exist.
template <typename OpType> static OpType getSingleOpOfType(Block &block) {
  OpType res = nullptr;
  block.walk([&](OpType op) {
    if (res) {
      res = nullptr;
      return WalkResult::interrupt();
    }
    res = op;
    return WalkResult::advance();
  });
  return res;
}

// taken from: LinalgInterfaces.cpp
// Detect whether res is any permutation of `u5(u1(c) + u2(u3(a) * u4(b)))`
// on the field (AddOpType, MulOpType), where u1, u2, u3, u4 and u5 represent
// unary operations that may change the type.
// TODO: this low-tech stuff is too manual (see:
// https://discourse.llvm.org/t/linalg-to-llvm-lowering/4867/7)
// Use OpDSL to generate all this.
template <typename AddOpType, typename MulOpType>
static bool isAddMul(Block &block) {
  if (block.getNumArguments() != 3)
    return false;
  Operation *yieldOp = block.getTerminator();
  if (yieldOp->getNumOperands() != 1)
    return false;

  AddOpType addOp = getSingleOpOfType<AddOpType>(block);
  MulOpType mulOp = getSingleOpOfType<MulOpType>(block);
  if (!addOp || !mulOp)
    return false;

  Value argA = block.getArgument(0), argB = block.getArgument(1);
  Value a = mulOp->getOperand(0), b = mulOp->getOperand(1);
  Value mul = mulOp->getResult(0);
  Value argC = block.getArgument(2);
  Value c1 = addOp->getOperand(0), c2 = addOp->getOperand(1);
  Value add = addOp->getResult(0);
  Value res = yieldOp->getOperand(0);
  // Result traces back to add.
  auto un = isChainOfUnaryOpsFrom;
  bool success = un(res, add);
  // One of the operands of add traces back to argC, the other to the mul.
  success |= (un(c1, argC) && un(c2, mul)) || ((un(c1, mul)) && un(c2, argC));
  // One of the operands of mul traces back to argA, the other to argB.
  success |= (un(a, argA) && un(b, argB)) || ((un(a, argB)) && un(b, argA));
  return success;
}

bool hasMatmulBody(linalg::LinalgOp linalgOp) {
  if (linalgOp->getNumRegions() != 1)
    return false;
  Region &region = linalgOp->getRegion(0);
  if (!region.hasOneBlock())
    return false;
  bool isFloat = isAddMul<arith::AddFOp, arith::MulFOp>(region.front());
  bool isInt = isAddMul<arith::AddIOp, arith::MulIOp>(region.front());
  return (isFloat || isInt);
}

bool hasStaticShape(linalg::LinalgOp linalgOp) {
  return !linalgOp.hasDynamicShape();
}

bool hasTppMark(linalg::LinalgOp linalgOp) {
  // Here we are abusing a bit the linalg library name machinery.
  // Main asserts if we query the name at tensor level. Inspect
  // only generic operation annotated by us.
  if (!isa<linalg::GenericOp>(linalgOp))
    return false;
  std::string libraryCall = linalgOp.getLibraryCallName();
  if (libraryCall.empty())
    return false;
  std::string delimiter = ".";
  std::string prefix = libraryCall.substr(0, libraryCall.find(delimiter));
  return prefix.compare("tpp") == 0;
}

bool isMarkedWithTpp(linalg::LinalgOp linalgOp, const std::string &target) {
  if (!hasTppMark(linalgOp))
    return false;
  std::string libraryCall = linalgOp.getLibraryCallName();
  return libraryCall.compare(target) == 0;
}

// Returns true if the region of the linalgOp has only a single operation
// (linalg.yieldOp).
static bool hasOnlyYieldOp(Region &region) {
  if (!region.hasOneBlock())
    return false;
  return std::distance(region.front().begin(), region.front().end()) == 1;
}

bool hasCopySemantics(linalg::LinalgOp linalgOp) {
  if (linalgOp.getNumParallelLoops() != linalgOp.getNumLoops())
    return false;
  if ((linalgOp->getNumOperands() != 2) || (linalgOp.getNumDpsInputs() != 1))
    return false;
  return hasOnlyYieldOp(linalgOp->getRegion(0));
}

// Returns the closest earlier user of a given operation op relative
// to an another op user currentUser.
// If there are no earlier users or the specified currentUser is invalid
// e.g., it does not belong the def-use chain of op, return nullptr instead.
static Operation *getPrevUser(Operation *op, Operation *currentUser) {
  if (!op || !currentUser)
    return nullptr;

  // Iterate over the op users to find the given current user.
  auto userIt = op->user_begin();
  while (userIt != op->user_end()) {
    if (*userIt == currentUser) {
      // Given that the user_iterator visits the op users from the last to
      // the first user, simply return the next user after the current user
      // i.e. an earlier/previous operation.
      const auto nextUser = ++userIt;
      return nextUser != op->user_end() ? *nextUser : nullptr;
    }

    ++userIt;
  }

  return nullptr;
}

// Returns true if the value is a constant float or integer.
static bool isValConstZero(Value val) {
  return matchPattern(val, m_AnyZeroFloat()) || matchPattern(val, m_Zero());
}

// Returns true if the value represents a zero filled tensor.
static bool isZeroTensor(Value op) { return isZeroTensor(op.getDefiningOp()); }

// Returns true if the operation represents a zero filled tensor
static bool isZeroTensor(Operation *defOp) {
  if (!defOp)
    return false;

  // TODO: add more possible sources of zero filled tensors
  // TODO: propagate operands of other operations that do not modify underlying
  //       data values
  return TypeSwitch<Operation *, bool>(defOp)
      .Case<linalg::FillOp>([&](auto op) {
        auto inputs = op.getInputs();
        return inputs.size() == 1 ? isValConstZero(inputs[0]) : false;
      })
      .Case<linalg::CopyOp>([&](auto op) {
        auto inputs = op.getInputs();
        return inputs.size() == 1 &&
               (isZeroTensor(inputs[0]) ||
                isZeroTensor(
                    getPrevUser(inputs[0].getDefiningOp(), op.getOperation())));
      })
      .Case<memref::CopyOp, memref::SubViewOp, tensor::CastOp,
            tensor::ExtractSliceOp>([&](auto op) {
        return isZeroTensor(op.getSource()) ||
               isZeroTensor(getPrevUser(op.getSource().getDefiningOp(),
                                        op.getOperation()));
      })
      .Default([&](Operation *op) { return false; });
}

bool hasMaxfZeroOp(linalg::LinalgOp linalgOp) {
  if (!isa<linalg::GenericOp>(linalgOp))
    return false;

  auto genOp = cast<linalg::GenericOp>(linalgOp);
  if (!genOp.getRegion().hasOneBlock())
    return false;

  for (Operation &op : genOp.getRegion().front()) {
    if (auto maxfOp = dyn_cast_or_null<arith::MaxFOp>(op)) {
      // Only check rhs for const value as this should be sufficient
      // for the op's canonical form.
      // Otherwise, check both operands if either one is a zero filled tensor.
      if (isValConstZero(maxfOp.getRhs()) || isZeroTensor(maxfOp.getLhs()) ||
          isZeroTensor(maxfOp.getRhs())) {
        return true;
      }

      // Check if maxf directly uses one of the linalg.generic operands.
      for (auto arg : genOp.getRegion().getArguments()) {
        if (arg != maxfOp.getLhs() && arg != maxfOp.getRhs())
          continue;

        if (auto argOp = genOp.getMatchingOpOperand(arg)) {
          // Check the operand itself and the operand's def-use chain to
          // detect more indirect dependencies such as a copy of a zero
          // tensor into this operand.
          if (isZeroTensor(argOp->get()) ||
              isZeroTensor(getPrevUser(argOp->get().getDefiningOp(),
                                       genOp.getOperation()))) {
            return true;
          }
        }
      }
    }
  }

  return false;
}

// Returns true if: 1) the region has a single block. 2) The block has two
// operations only (linalg.YieldOp and OP). 3) The operation result types are
// int or float.
// TODO: For now we assume the region to have only two operations: The YieldOp
// and the 'OP', meaning that the entire linalg.generic will map to a single
// tpp operation. If we do element-wise fusion at the linalg level this
// assumption does not hold anymore as now a linalg.generic can map to n tpp
// operations. If we support 1:n matching what should we do if the entire
// linalg.op cannot be replace by tpp operations?
template <typename OP> static bool hasOnlyScalarElementwiseOp(Region &region) {
  if (!region.hasOneBlock())
    return false;
  if (std::distance(region.front().begin(), region.front().end()) != 2)
    return false;
  for (Operation &op : region.front()) {
    if (!isa<OP, linalg::YieldOp>(op) ||
        llvm::any_of(op.getResultTypes(),
                     [](Type type) { return !type.isIntOrFloat(); }))
      return false;
  }
  return true;
}

// Returns true if the operation as 1 input and 1 output.
static bool hasOneInputOneOutput(linalg::GenericOp linalgOp) {
  return ((linalgOp.getNumDpsInputs() == 1) &&
          (linalgOp.getNumDpsInits() == 1));
}

// Returns true if the linalg.generic maps to a tpp.gemm.
bool isTppMatmul(linalg::GenericOp linalgOp) {
  // structural and access pattern.
  SmallVector<mlir::utils::IteratorType> iteratorTypes =
      linalgOp.getIteratorTypesArray();
  if (iteratorTypes.size() != 3)
    return false;
  if (!(linalg::isParallelIterator(iteratorTypes[0]) &&
        linalg::isParallelIterator(iteratorTypes[1]) &&
        linalg::isReductionIterator(iteratorTypes[2])))
    return false;
  using MapList = ArrayRef<ArrayRef<AffineExpr>>;
  auto infer = [](MapList m) { return AffineMap::inferFromExprList(m); };
  AffineExpr i, j, k;
  bindDims(linalgOp.getContext(), i, j, k);
  if (linalgOp.getIndexingMapsArray() != infer({{i, k}, {k, j}, {i, j}}))
    return false;
  // operations and operands.
  return hasMatmulBody(linalgOp);
}

bool canMapToTppAdd(linalg::GenericOp linalgOp) {
  if (!linalg::isElementwise(linalgOp))
    return false;
  if (!hasStaticShape(linalgOp) || !hasOneInputOneOutput(linalgOp))
    return false;
  return hasOnlyScalarElementwiseOp<arith::AddFOp>(linalgOp.getRegion());
}

// TODO: check access pattern using affine map.
bool isTppAdd(linalg::GenericOp linalgOp) {
  if (linalgOp.getNumLoops() != linalgOp.getNumParallelLoops())
    return false;
  if ((linalgOp.getNumDpsInputs() != 1) || (linalgOp.getNumDpsInits() != 1))
    return false;
  if (linalgOp.hasTensorSemantics())
    return false;
  return hasOnlyScalarElementwiseOp<arith::AddFOp>(linalgOp.getRegion());
}

bool canMapToTppRelu(linalg::GenericOp linalgOp) {
  if (!linalg::isElementwise(linalgOp))
    return false;
  if (!hasStaticShape(linalgOp) || !hasMaxfZeroOp(linalgOp))
    return false;
  return hasOnlyScalarElementwiseOp<arith::MaxFOp>(linalgOp.getRegion());
}

// TODO: check access pattern using affine map.
bool isTppRelu(linalg::GenericOp linalgOp) {
  if (linalgOp.getNumLoops() != linalgOp.getNumParallelLoops())
    return false;
  if ((linalgOp.getNumDpsInputs() != 0) || (linalgOp.getNumDpsInits() != 1))
    return false;
  if (linalgOp.hasTensorSemantics())
    return false;
  return hasOnlyScalarElementwiseOp<arith::MaxFOp>(linalgOp.getRegion());
}

bool canMapToTppIdentity(linalg::GenericOp linalgOp) {
  if (!linalg::isElementwise(linalgOp))
    return false;
  return (hasStaticShape(linalgOp) && hasCopySemantics(linalgOp));
}

// TODO: check access pattern using affine map. We need to take into account
// broadcasting too.
bool isTppIdentity(linalg::GenericOp linalgOp) {
  if (linalgOp.getNumLoops() != linalgOp.getNumParallelLoops())
    return false;
  if ((linalgOp.getNumDpsInputs() != 1) || (linalgOp.getNumDpsInits() != 1))
    return false;
  if (linalgOp.hasTensorSemantics())
    return false;
  return hasCopySemantics(linalgOp);
}

} // namespace utils
} // namespace tpp
} // namespace mlir
