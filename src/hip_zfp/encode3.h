#include "hip/hip_runtime.h"
#ifndef HIPZFP_ENCODE3_H
#define HIPZFP_ENCODE3_H

#include "hipZFP.h"
#include "shared.h"
#include "encode.h"
#include "type_info.h"

#define ZFP_3D_BLOCK_SIZE 64

namespace hipZFP {

template <typename Scalar> 
__device__ __host__ inline 
void gather_partial3(Scalar* q, const Scalar* p, int nx, int ny, int nz, int sx, int sy, int sz)
{
  for (uint z = 0; z < 4; z++)
    if (z < nz) {
      for (uint y = 0; y < 4; y++)
        if (y < ny) {
          for (uint x = 0; x < 4; x++)
            if (x < nx) {
              q[16 * z + 4 * y + x] = *p;
              p += sx;
            }
          p += sy - nx * sx;
          pad_block(q + 16 * z + 4 * y, nx, 1);
        }
      for (uint x = 0; x < 4; x++)
        pad_block(q + 16 * z + x, ny, 4);
      p += sz - ny * sy;
    }
  for (uint y = 0; y < 4; y++)
    for (uint x = 0; x < 4; x++)
      pad_block(q + 4 * y + x, nz, 16);
}

template <typename Scalar> 
__device__ __host__ inline 
void gather3(Scalar* q, const Scalar* p, int sx, int sy, int sz)
{
  for (uint z = 0; z < 4; z++, p += sz - 4 * sy)
    for (uint y = 0; y < 4; y++, p += sy - 4 * sx)
      for (uint x = 0; x < 4; x++, p += sx)
        *q++ = *p;
}

template <class Scalar>
__global__
void 
hipEncode3(
  const uint maxbits,
  const Scalar* scalars,
  Word* stream,
  const uint3 dims,
  const int3 stride,
  const uint3 padded_dims,
  const uint tot_blocks
)
{
  typedef unsigned long long int ull;
  typedef long long int ll;
  const ull blockId = blockIdx.x + gridDim.x * (blockIdx.y + gridDim.y * blockIdx.z);

  // each thread gets a block so the block index is 
  // the global thread index
  const uint block_idx = blockId * blockDim.x + threadIdx.x;

  if (block_idx >= tot_blocks) {
    // we can't launch the exact number of blocks
    // so just exit if this isn't real
    return;
  }

  uint3 block_dims;
  block_dims.x = padded_dims.x >> 2; 
  block_dims.y = padded_dims.y >> 2; 
  block_dims.z = padded_dims.z >> 2; 

  // logical pos in 3d array
  uint3 block;
  block.x = (block_idx % block_dims.x) * 4; 
  block.y = ((block_idx / block_dims.x) % block_dims.y) * 4; 
  block.z = (block_idx / (block_dims.x * block_dims.y)) * 4; 

  const ll offset = (ll)block.x * stride.x + (ll)block.y * stride.y + (ll)block.z * stride.z; 
  Scalar fblock[ZFP_3D_BLOCK_SIZE]; 

  bool partial = false;
  if (block.x + 4 > dims.x) partial = true;
  if (block.y + 4 > dims.y) partial = true;
  if (block.z + 4 > dims.z) partial = true;
 
  if (partial) {
    const uint nx = block.x + 4 > dims.x ? dims.x - block.x : 4;
    const uint ny = block.y + 4 > dims.y ? dims.y - block.y : 4;
    const uint nz = block.z + 4 > dims.z ? dims.z - block.z : 4;
    gather_partial3(fblock, scalars + offset, nx, ny, nz, stride.x, stride.y, stride.z);
  }
  else
    gather3(fblock, scalars + offset, stride.x, stride.y, stride.z);

  encode_block<Scalar, ZFP_3D_BLOCK_SIZE>(fblock, maxbits, block_idx, stream);  
}

//
// Launch the encode kernel
//
template <class Scalar>
size_t encode3launch(
  uint3 dims, 
  int3 stride,
  const Scalar* d_data,
  Word* stream,
  const int maxbits
)
{
  const int hip_block_size = 128;
  dim3 block_size = dim3(hip_block_size, 1, 1);

  uint3 zfp_pad(dims); 
  if (zfp_pad.x % 4 != 0) zfp_pad.x += 4 - dims.x % 4;
  if (zfp_pad.y % 4 != 0) zfp_pad.y += 4 - dims.y % 4;
  if (zfp_pad.z % 4 != 0) zfp_pad.z += 4 - dims.z % 4;

  const uint zfp_blocks = (zfp_pad.x * zfp_pad.y * zfp_pad.z) / 64; 

  // ensure that we launch a multiple of the hip block size
  int block_pad = 0; 
  if (zfp_blocks % hip_block_size != 0)
    block_pad = hip_block_size - zfp_blocks % hip_block_size; 

  size_t total_blocks = block_pad + zfp_blocks;
  dim3 grid_size = calculate_grid_size(total_blocks, hip_block_size);
  size_t stream_bytes = calc_device_mem3d(zfp_pad, maxbits);

  // ensure we have zeros (for atomics)
  hipMemset(stream, 0, stream_bytes);

#ifdef HIP_ZFP_RATE_PRINT
  Timer timer;
  timer.start();
#endif
  
  hipLaunchKernelGGL(HIP_KERNEL_NAME(hipEncode3<Scalar>), grid_size, block_size, 0, 0, maxbits,
     d_data,
     stream,
     dims,
     stride,
     zfp_pad,
     zfp_blocks);

#ifdef HIP_ZFP_RATE_PRINT
  timer.stop();
  timer.print_throughput<Scalar>("Encode", "encode3", dim3(dims.x, dims.y, dims.z));
#endif

  size_t bits_written = zfp_blocks * maxbits;

  return bits_written;
}

template <class Scalar>
size_t encode3(
  uint3 dims, 
  int3 stride,
  Scalar* d_data,
  Word* stream,
  const int maxbits
)
{
  return encode3launch<Scalar>(dims, stride, d_data, stream, maxbits);
}

} // namespace hipZFP

#endif