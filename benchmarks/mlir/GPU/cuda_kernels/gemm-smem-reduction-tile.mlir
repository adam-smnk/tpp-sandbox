// RUN: tpp-run %s -n 10 \
// RUN:  -e entry -entry-point-result=void

// BENCH_TOTAL_FLOPS: 2147483648

// A and B tiles cached in SMEM (shared memory).
#map = affine_map<(d0) -> (d0 * 32)>
module attributes {gpu.container_module} {
  func.func @entry(%arg0: memref<1024x1024xf32>, %arg1: memref<1024x1024xf32>, %arg2: memref<1024x1024xf32>) {
    %c32 = arith.constant 32 : index
    %c1 = arith.constant 1 : index
    %c0 = arith.constant 0 : index
    %c1024 = arith.constant 1024 : index
    gpu.launch_func  @entry_kernel::@entry_kernel blocks in (%c32, %c32, %c1) threads in (%c32, %c32, %c1)  args(%arg0 : memref<1024x1024xf32>, %arg1 : memref<1024x1024xf32>, %arg2 : memref<1024x1024xf32>, %c0 : index, %c1024 : index, %c1 : index)
    return
  }
  gpu.module @entry_kernel {
    // SMEM sub-tile buffers.
    memref.global "private" @smemTileA : memref<32x32xf32, #gpu.address_space<workgroup>>
    memref.global "private" @smemTileB : memref<32x32xf32, #gpu.address_space<workgroup>>
    gpu.func @entry_kernel(%arg0: memref<1024x1024xf32>, %arg1: memref<1024x1024xf32>, %arg2: memref<1024x1024xf32>, %arg3: index, %arg4: index, %arg5: index) kernel attributes {gpu.known_block_size = array<i32: 32, 32, 1>, gpu.known_grid_size = array<i32: 32, 32, 1>} {
      %c0 = arith.constant 0 : index
      %c1 = arith.constant 1 : index
      %c32 = arith.constant 32 : index

      %0 = gpu.block_id  x
      %1 = gpu.block_id  y
      %2 = gpu.thread_id  y
      %3 = gpu.thread_id  x
      %4 = affine.apply #map(%0)
      %5 = affine.apply #map(%1)

      %subview = memref.subview %arg0[%4, 0] [32, 1024] [1, 1] : memref<1024x1024xf32> to memref<32x1024xf32, strided<[1024, 1], offset: ?>>
      %subview_0 = memref.subview %arg1[0, %5] [1024, 32] [1, 1] : memref<1024x1024xf32> to memref<1024x32xf32, strided<[1024, 1], offset: ?>>
      %subview_1 = memref.subview %arg2[%4, %5] [32, 32] [1, 1] : memref<1024x1024xf32> to memref<32x32xf32, strided<[1024, 1], offset: ?>>

      %smemA = memref.get_global @smemTileA : memref<32x32xf32, #gpu.address_space<workgroup>>
      %smemB = memref.get_global @smemTileB : memref<32x32xf32, #gpu.address_space<workgroup>>

      %dimK = memref.dim %subview, %c1 : memref<32x1024xf32, strided<[1024, 1], offset: ?>>
      %bDimX = gpu.block_dim x
      %bDimY = gpu.block_dim y
      %numSubTilesK = arith.ceildivsi %dimK, %bDimX : index

      %elemC = memref.load %subview_1[%2, %3] : memref<32x32xf32, strided<[1024, 1], offset: ?>>

      %res = scf.for %subtileIv = %c0 to %numSubTilesK step %c1 iter_args(%acc = %elemC) -> (f32) {
        // First, load A and B sub-tiles to SMEM.
        %subTileStepRow = arith.muli %subtileIv, %bDimX : index
        %subTileStepCol = arith.muli %subtileIv, %bDimY : index
        %offsetA = arith.addi %3, %subTileStepRow : index
        %offsetB = arith.addi %2, %subTileStepCol : index

        %elemA = memref.load %subview[%2, %offsetA] : memref<32x1024xf32, strided<[1024, 1], offset: ?>>
        %elemB = memref.load %subview_0[%offsetB, %3] : memref<1024x32xf32, strided<[1024, 1], offset: ?>>

        memref.store %elemA, %smemA[%2, %3] : memref<32x32xf32, #gpu.address_space<workgroup>>
        memref.store %elemB, %smemB[%2, %3] : memref<32x32xf32, #gpu.address_space<workgroup>>

        // Sync all threads in a threadblock.
        // Wait for A and B tile loads to complete.
        gpu.barrier

        %7 = scf.for %arg6 = %c0 to %c32 step %c1 iter_args(%arg7 = %acc) -> (f32) {
          // A tile same element -> broadcast [fast]
          // B tile consecutive elements -> coalesced [fast]
          %8 = memref.load %smemA[%2, %arg6] : memref<32x32xf32, #gpu.address_space<workgroup>>
          %9 = memref.load %smemB[%arg6, %3] : memref<32x32xf32, #gpu.address_space<workgroup>>
          %10 = arith.mulf %8, %9 : f32
          %11 = arith.addf %arg7, %10 : f32
          scf.yield %11 : f32
        }

        // Sync all threads in a threadblock.
        // Wait for GEMM computation to finish before loading next A and B sub-tiles.
        gpu.barrier

        scf.yield %7 : f32
      }

      // Store the final C tile element value.
      // C tile consecutive elements -> coalesced [fast]
      memref.store %res, %subview_1[%2, %3] : memref<32x32xf32, strided<[1024, 1], offset: ?>>
      gpu.return
    }
  }
}