// RUN: tpp-opt %s -linalg-ext-to-loops -convert-linalg-to-loops -convert-tpp-to-xsmm -convert-xsmm-to-func -convert-vector-to-scf -convert-scf-to-cf -lower-affine -convert-vector-to-llvm -convert-memref-to-llvm -arith-expand -convert-math-to-llvm -convert-func-to-llvm -reconcile-unrealized-casts |\
// RUN: mlir-cpu-runner \
// RUN:  -e entry -entry-point-result=void  \
// RUN: -shared-libs=%llvmlibdir/libmlir_c_runner_utils%shlibext,%tpplibdir/libtpp_c_runner_utils%shlibext
//

func.func @entry(){
  %c0 = arith.constant 1.0:bf16
  %arg0 = memref.alloc():memref<128x256xbf16>
  linalg.fill ins(%c0:bf16) outs(%arg0:memref<128x256xbf16>)
  %arg1 = memref.alloc():memref<256x512xbf16>
  linalg.fill ins(%c0:bf16) outs(%arg1:memref<256x512xbf16>)
  %arg2 = memref.alloc():memref<512xbf16>
  linalg.fill ins(%c0:bf16) outs(%arg2:memref<512xbf16>)
  %arg3 = memref.alloc():memref<128x512xbf16>
  linalg.fill ins(%c0:bf16) outs(%arg3:memref<128x512xbf16>)
  tpp.identity ins(%arg2 : memref<512xbf16>) out(%arg3 : memref<128x512xbf16>)
  %wt = memref.alloc():memref<64x256x2xbf16>
  linalgx.pack %arg0 inner_dims_pos=[0] inner_tiles=[2] into %wt: (memref<128x256xbf16> memref<64x256x2xbf16>)
  tpp.vnni_matmul ins(%wt : memref<64x256x2xbf16>, %arg1 : memref<256x512xbf16>) out(%arg3 : memref<128x512xbf16>)
  tpp.relu out(%arg3 : memref<128x512xbf16>)
  return
}
