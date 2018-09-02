#ifndef SHARED_H
#define SHARED_H

typedef unsigned long long Word;
#define Wsize ((uint)(CHAR_BIT * sizeof(Word)))

#include "type_info.cuh"
//#include "zfp_structs.h"
#include "zfp.h"
#include <stdio.h>

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define bitsize(x) (CHAR_BIT * (uint)sizeof(x))

#define LDEXP(x, e) ldexp(x, e)
#define FREXP(x, e) frexp(x, e)
#define FABS(x) fabs(x)

#define NBMASK 0xaaaaaaaaaaaaaaaaull

__constant__ unsigned char c_perm_1[4];
__constant__ unsigned char c_perm_2[16];
__constant__ unsigned char c_perm[64];

namespace cuZFP
{

dim3 get_max_grid_dims()
{
  cudaDeviceProp prop; 
  int device = 0;
  cudaGetDeviceProperties(&prop, device);
  dim3 grid_dims;
  grid_dims.x = prop.maxGridSize[0];
  grid_dims.y = prop.maxGridSize[1];
  grid_dims.z = prop.maxGridSize[2];
  return grid_dims;
}

// size is assumed to have a pad to the nearest cuda block size
dim3 calculate_grid_size(size_t size, size_t cuda_block_size)
{
  size_t grids = size / cuda_block_size; // because of pad this will be exact
  dim3 max_grid_dims = get_max_grid_dims();
  int dims  = 1;
  // check to see if we need to add more grids
  if( grids > max_grid_dims.x)
  {
    dims = 2; 
  }
  if(grids > max_grid_dims.x * max_grid_dims.y)
  {
    dims = 3;
  }
  dim3 grid_size;
  grid_size.x = 1;
  grid_size.y = 1;
  grid_size.z = 1;
 
  if(dims == 1)
  {
    grid_size.x = grids; 
  }

  if(dims == 2)
  {
    float sq_r = sqrt((float)grids);
    float intpart = 0.;
    modf(sq_r,&intpart); 
    uint base = intpart;
    grid_size.x = base; 
    grid_size.y = base; 
    // figure out how many y to add
    uint rem = (size - base * base);
    uint y_rows = rem / base;
    if(rem % base != 0) y_rows ++;
    grid_size.y += y_rows; 
  }

  if(dims == 3)
  {
    float cub_r = pow((float)grids, 1.f/3.f);;
    float intpart = 0.;
    modf(cub_r,&intpart); 
    int base = intpart;
    grid_size.x = base; 
    grid_size.y = base; 
    grid_size.z = base; 
    // figure out how many z to add
    uint rem = (size - base * base * base);
    uint z_rows = rem / (base * base);
    if(rem % (base * base) != 0) z_rows ++;
    grid_size.z += z_rows; 
  }

  
  return grid_size;
}

template<int block_size>
struct BlockWriter2
{

  uint m_word_index;
  uint m_start_bit;
  uint m_current_bit;
  const int m_maxbits; 
  Word *m_stream;
   uint di;

  __device__ BlockWriter2(Word *stream, const int &maxbits, const uint &block_idx)
   :  m_current_bit(0),
      m_maxbits(maxbits),
      m_stream(stream)
  {
    m_word_index = (block_idx * maxbits)  / (sizeof(Word) * 8); 
    di = m_word_index;
    m_start_bit = uint((block_idx * maxbits) % (sizeof(Word) * 8)); 
  }

  template<typename T>
  __device__
  void print_bits(T bits)
  {
    const int bit_size = sizeof(T) * 8;
    for(int i = bit_size - 1; i >=0; --i)
    {
      T one = 1;
      T mask = one << i;
      int val = (bits & mask) >> i;
      printf("%d", val);
    }
    printf("\n");
  }
  __device__
  void print()
  {
    //vtkm::Int64 v = Portal.Add(debug_index,0);
    //std::cout<<"current bit "<<m_current_bit<<" debug_index "<<debug_index<<" ";
    //print_bits(*reinterpret_cast<vtkm::UInt64*>(&v));
    print_bits(m_stream[di]);
  }
  __device__
  void print(int index)
  {
    //vtkm::Int64 v = Portal.Add(index,0);
    print_bits(m_stream[index]);
  }


  __device__
  long long unsigned int
  write_bits(const long long unsigned int &bits, const uint &n_bits)
  {
    const uint wbits = sizeof(Word) * 8;
    //if(bits == 0) { printf("no\n"); return;}
    //uint seg_start = (m_start_bit + bit_offset) % wbits;
    //int write_index = m_word_index + (m_start_bit + bit_offset) / wbits;
    uint seg_start = (m_start_bit + m_current_bit) % wbits;
    uint write_index = m_word_index + uint((m_start_bit + m_current_bit) / wbits);
    di = write_index;
    uint seg_end = seg_start + n_bits - 1;
    //int write_index = m_word_index;
    uint shift = seg_start; 
    // we may be asked to write less bits than exist in 'bits'
    // so we have to make sure that anything after n is zero.
    // If this does not happen, then we may write into a zfp
    // block not at the specified index
    // uint zero_shift = sizeof(Word) * 8 - n_bits;
    Word left = (bits >> n_bits) << n_bits;
    
    Word b = bits - left;
    Word add = b << shift;
    atomicAdd(&m_stream[write_index], add); 
    // n_bits straddles the word boundary
    bool straddle = seg_start < sizeof(Word) * 8 && seg_end >= sizeof(Word) * 8;
    if(straddle)
    {
      Word rem = b >> (sizeof(Word) * 8 - shift);
      atomicAdd(&m_stream[write_index + 1], rem); 
      di = write_index+1;
    }
    m_current_bit += n_bits;
    return bits >> (Word)n_bits;
  }

  // TODO: optimize
  __device__
  uint write_bit(const unsigned int &bit)
  {
    //bool print = m_word_index == 0  && m_start_bit == 0;
    const uint wbits = sizeof(Word) * 8;
    //if(bits == 0) { printf("no\n"); return;}
    //uint seg_start = (m_start_bit + bit_offset) % wbits;
    //int write_index = m_word_index + (m_start_bit + bit_offset) / wbits;
    uint seg_start = (m_start_bit + m_current_bit) % wbits;
    uint write_index = m_word_index + uint((m_start_bit + m_current_bit) / wbits);
    di = write_index;
    //uint seg_end = seg_start;
    //int write_index = m_word_index;
    uint shift = seg_start; 
    // we may be asked to write less bits than exist in 'bits'
    // so we have to make sure that anything after n is zero.
    // If this does not happen, then we may write into a zfp
    // block not at the specified index
    // uint zero_shift = sizeof(Word) * 8 - n_bits;
    
    Word add = (Word)bit << shift;
    atomicAdd(&m_stream[write_index], add); 
    m_current_bit += 1;

    return bit;
  }

};

template<typename Scalar>
inline __device__
void pad_block(Scalar *p, uint n, uint s)
{
  switch (n) 
  {
    case 0:
      p[0 * s] = 0;
      /* FALLTHROUGH */
    case 1:
      p[1 * s] = p[0 * s];
      /* FALLTHROUGH */
    case 2:
      p[2 * s] = p[1 * s];
      /* FALLTHROUGH */
    case 3:
      p[3 * s] = p[0 * s];
      /* FALLTHROUGH */
    default:
      break;
  }
}

// maximum number of bit planes to encode
__device__ __host__
static int
precision(int maxexp, int maxprec, int minexp)
{
  return MIN(maxprec, MAX(0, maxexp - minexp + 8));
}

// map two's complement signed integer to negabinary unsigned integer
inline __device__ __host__
unsigned long long int int2uint(const long long int x)
{
    return (x + (unsigned long long int)0xaaaaaaaaaaaaaaaaull) ^ 
                (unsigned long long int)0xaaaaaaaaaaaaaaaaull;
}

inline __device__ __host__
unsigned int int2uint(const int x)
{
    return (x + (unsigned int)0xaaaaaaaau) ^ 
                (unsigned int)0xaaaaaaaau;
}

template<class Scalar>
__device__
static int
exponent(Scalar x)
{
  if (x > 0) {
    int e;
    frexp(x, &e);
    // clamp exponent in case x is denormalized
    return max(e, 1 - get_ebias<Scalar>());
  }
  return -get_ebias<Scalar>();
}

template<class Scalar, int BlockSize>
__device__
static int
max_exponent(const Scalar* p)
{
  Scalar max_val = 0;
  for(int i = 0; i < BlockSize; ++i)
  {
    Scalar f = fabs(p[i]);
    max_val = max(max_val,f);
  }
  return exponent<Scalar>(max_val);
}

// lifting transform of 4-vector
template <class Int, uint s>
__device__ __host__
static void
fwd_lift(Int* p)
{
  Int x = *p; p += s;
  Int y = *p; p += s;
  Int z = *p; p += s;
  Int w = *p; p += s;

  // default, non-orthogonal transform (preferred due to speed and quality)
  //        ( 4  4  4  4) (x)
  // 1/16 * ( 5  1 -1 -5) (y)
  //        (-4  4  4 -4) (z)
  //        (-2  6 -6  2) (w)
  x += w; x >>= 1; w -= x;
  z += y; z >>= 1; y -= z;
  x += z; x >>= 1; z -= x;
  w += y; w >>= 1; y -= w;
  w += y >> 1; y -= w >> 1;

  p -= s; *p = w;
  p -= s; *p = z;
  p -= s; *p = y;
  p -= s; *p = x;
}

template<typename Scalar>
Scalar
inline __device__
quantize_factor(const int &exponent, Scalar);

template<>
float
inline __device__
quantize_factor<float>(const int &exponent, float)
{
	return  LDEXP(1.0, get_precision<float>() - 2 - exponent);
}

template<>
double
inline __device__
quantize_factor<double>(const int &exponent, double)
{
	return  LDEXP(1.0, get_precision<double>() - 2 - exponent);
}

template<typename Int, typename Scalar>
__device__
Scalar
dequantize(const Int &x, const int &e);

template<>
__device__
double
dequantize<long long int, double>(const long long int &x, const int &e)
{
	return LDEXP((double)x, e - (CHAR_BIT * scalar_sizeof<double>() - 2));
}

template<>
__device__
float
dequantize<int, float>(const int &x, const int &e)
{
	return LDEXP((float)x, e - (CHAR_BIT * scalar_sizeof<float>() - 2));
}

template<>
__device__
int
dequantize<int, int>(const int &x, const int &e)
{
	return 1;
}

template<>
__device__
long long int
dequantize<long long int, long long int>(const long long int &x, const int &e)
{
	return 1;
}

/* inverse lifting transform of 4-vector */
template<class Int, uint s>
__host__ __device__
static void
inv_lift(Int* p)
{
	Int x, y, z, w;
	x = *p; p += s;
	y = *p; p += s;
	z = *p; p += s;
	w = *p; p += s;

	/*
	** non-orthogonal transform
	**       ( 4  6 -4 -1) (x)
	** 1/4 * ( 4  2  4  5) (y)
	**       ( 4 -2  4 -5) (z)
	**       ( 4 -6 -4  1) (w)
	*/
	y += w >> 1; w -= y >> 1;
	y += w; w <<= 1; w -= y;
	z += x; x <<= 1; x -= z;
	y += z; z <<= 1; z -= y;
	w += x; x <<= 1; x -= w;

	p -= s; *p = w;
	p -= s; *p = z;
	p -= s; *p = y;
	p -= s; *p = x;
}

template<typename Scalar, typename Int, int BlockSize>
void __device__ fwd_cast(Int *iblock, const Scalar *fblock, int emax)
{
	Scalar s = quantize_factor(emax, Scalar());
  for(int i = 0; i < BlockSize; ++i)
  {
    iblock[i] = (Int) (s * fblock[i]);
  }
}

template<int BlockSize>
struct transform;

template<>
struct transform<64>
{
  template<typename Int>
  __device__ void fwd_xform(Int *p)
  {

    uint x, y, z;
    /* transform along x */
    for (z = 0; z < 4; z++)
      for (y = 0; y < 4; y++)
        fwd_lift<Int,1>(p + 4 * y + 16 * z);
    /* transform along y */
    for (x = 0; x < 4; x++)
      for (z = 0; z < 4; z++)
        fwd_lift<Int,4>(p + 16 * z + 1 * x);
    /* transform along z */
    for (y = 0; y < 4; y++)
      for (x = 0; x < 4; x++)
        fwd_lift<Int,16>(p + 1 * x + 4 * y);

   }

};

template<int BlockSize>
__device__
unsigned char* get_perm();

template<>
__device__
unsigned char* get_perm<64>()
{
  return c_perm;
}

template<>
__device__
unsigned char* get_perm<16>()
{
  return c_perm_2;
}

template<>
__device__
unsigned char* get_perm<4>()
{
  return c_perm_1;
}


template<typename Int, typename UInt, int BlockSize>
__device__ void fwd_order(UInt *ublock, const Int *iblock)
{
  unsigned char *perm = get_perm<BlockSize>();
  for(int i = 0; i < BlockSize; ++i)
  {
    ublock[i] = int2uint(iblock[perm[i]]);
  }
}

template<typename Int, int DIMS>
__device__ void fwd_xform(Int* p);

template<typename Int, int BlockSize> 
void inline __device__ encode_block(BlockWriter2<BlockSize> &stream,
                                    int maxbits,
                                    int maxprec,
                                    Int *iblock)
{
  transform<BlockSize> tform;
  tform.fwd_xform(iblock);

  typedef typename zfp_traits<Int>::UInt UInt;
  UInt ublock[BlockSize]; 
  fwd_order<Int, UInt, BlockSize>(ublock, iblock);

  uint intprec = CHAR_BIT * (uint)sizeof(UInt);
  uint kmin = intprec > maxprec ? intprec - maxprec : 0;
  uint bits = maxbits;
  uint i, k, m, n;
  uint64 x;
  //for(int p = 0; p < BlockSize; ++p) printf(" %llu \n", ublock[p]);

  for (k = intprec, n = 0; bits && k-- > kmin;) {
    /* step 1: extract bit plane #k to x */
    x = 0;
    for (i = 0; i < BlockSize; i++)
    {
      x += (uint64)((ublock[i] >> k) & 1u) << i;
    }
    //printf("plane %llu\n", x);
    //print_bits(x);
    /* step 2: encode first n bits of bit plane */
    m = min(n, bits);
    uint temp  = bits;
    bits -= m;
    x = stream.write_bits(x, m);
    
    //printf("rem plane %llu\n", x);
    /* step 3: unary run-length encode remainder of bit plane */
    for (; n < BlockSize && bits && (bits--, stream.write_bit(!!x)); x >>= 1, n++)
    {
      for (; n < BlockSize - 1 && bits && (bits--, !stream.write_bit(x & 1u)); x >>= 1, n++)
      {  
      }
    }
    //stream.print();
    //temp = temp - bits;
    //printf(" rem buts %d intprec %d k %d encoded_bits %d\n", (int)bits, (int)intprec, (int)k,(int)temp); 
  }
  
}
                                   
template<typename Scalar, int BlockSize>
void inline __device__ zfp_encode_block(Scalar *fblock,
                                        const int maxbits,
                                        const uint block_idx,
                                        Word *stream)
{
  BlockWriter2<BlockSize> block_writer(stream, maxbits, block_idx);
  int emax = max_exponent<Scalar, BlockSize>(fblock);
  int maxprec = precision(emax, get_precision<Scalar>(), get_min_exp<Scalar>());
  uint e = maxprec ? emax + get_ebias<Scalar>() : 0;
  if(e)
  {
    const uint ebits = get_ebits<Scalar>()+1;
    block_writer.write_bits(2 * e + 1, ebits);
    typedef typename zfp_traits<Scalar>::Int Int;
    Int iblock[BlockSize];
    fwd_cast<Scalar, Int, BlockSize>(iblock, fblock, emax);


    encode_block<Int, BlockSize>(block_writer, maxbits - ebits, maxprec, iblock);
  }
}

template<>
void inline __device__ zfp_encode_block<int, 64>(int *fblock,
                                             const int maxbits,
                                             const uint block_idx,
                                             Word *stream)
{
  BlockWriter2<64> block_writer(stream, maxbits, block_idx);
  const int intprec = get_precision<int>();
  encode_block<int, 64>(block_writer, maxbits, intprec, fblock);
}

template<>
void inline __device__ zfp_encode_block<long long int, 64>(long long int *fblock,
                                                       const int maxbits,
                                                       const uint block_idx,
                                                       Word *stream)
{
  BlockWriter2<64> block_writer(stream, maxbits, block_idx);
  const int intprec = get_precision<long long int>();
  encode_block<long long int, 64>(block_writer, maxbits, intprec, fblock);
}

/* transform along z */
template<class Int>
 __device__
static void
inv_xform_yx(Int* p)
{
	inv_lift<Int, 16>(p + 1 * threadIdx.x + 4 * threadIdx.z);
}

/* transform along y */
template<class Int>
 __device__
static void
inv_xform_xz(Int* p)
{
	inv_lift<Int, 4>(p + 16 * threadIdx.z + 1 * threadIdx.x);
}

/* transform along x */
template<class Int>
 __device__
static void
inv_xform_zy(Int* p)
{
	inv_lift<Int, 1>(p + 4 * threadIdx.x + 16 * threadIdx.z);
}

/* inverse decorrelating 3D transform */
template<class Int>
 __device__
static void
inv_xform(Int* p)
{

	inv_xform_yx(p);
	__syncthreads();
	inv_xform_xz(p);
	__syncthreads();
	inv_xform_zy(p);
	__syncthreads();
}

/* map two's complement signed integer to negabinary unsigned integer */
inline __host__ __device__
long long int uint2int(unsigned long long int x)
{
	return (x ^0xaaaaaaaaaaaaaaaaull) - 0xaaaaaaaaaaaaaaaaull;
}

inline __host__ __device__
int uint2int(unsigned int x)
{
	return (x ^0xaaaaaaaau) - 0xaaaaaaaau;
}
template<typename T>
__device__ void print_bits(const T &bits)
{
  const int bit_size = sizeof(T) * 8;

  for(int i = bit_size - 1; i >= 0; --i)
  {
    T one = 1;
    T mask = one << i;
    T val = (bits & mask) >> i ;
    printf("%d", (int) val);
  }
  printf("\n");
}

struct BitStream32
{
  int current_bits; 
  unsigned int bits;
  __device__ BitStream32()
    : current_bits(0), bits(0)
  {
  }

  inline __device__ 
  unsigned short write_bits(const unsigned int &src, 
                            const uint &n_bits)
  {
    // set the first n bits to 0
    unsigned int left = (src >> n_bits) << n_bits;
    unsigned int b = src - left;
    b = b << current_bits;  
    current_bits += n_bits;
    bits += b;
    unsigned int res = left >> n_bits;
    return res;
  }

  inline __device__ 
  unsigned short write_bit(const unsigned short bit)
  {
    bits += bit << current_bits;   
    current_bits += 1;
    return bit;
  }

};

template<int block_size>
struct BlockWriter
{
  int m_word_index;
  int m_start_bit;
  const int m_maxbits; 
  Word *m_words;
  bool m_valid_block;
  __device__ BlockWriter(Word *b, const int &maxbits, const int &block_idx, const int &num_blocks)
    : m_words(b), m_maxbits(maxbits), m_valid_block(true)
  {
    if(block_idx >= num_blocks) m_valid_block = false;
    m_word_index = (block_idx * maxbits)  / (sizeof(Word) * 8); 
    m_start_bit = (block_idx * maxbits) % (sizeof(Word) * 8); 
  }

  inline __device__ 
  void write_bits(const unsigned int &bits, const uint &n_bits, const uint &bit_offset)
  {
    //bool print = m_word_index == 0  && m_start_bit == 0;
    const uint wbits = sizeof(Word) * 8;
    //if(bits == 0) { printf("no\n"); return;}
    uint seg_start = (m_start_bit + bit_offset) % wbits;
    int write_index = m_word_index + (m_start_bit + bit_offset) / wbits;
    uint seg_end = seg_start + n_bits - 1;
    //int write_index = m_word_index;
    uint shift = seg_start; 
    // we may be asked to write less bits than exist in 'bits'
    // so we have to make sure that anything after n is zero.
    // If this does not happen, then we may write into a zfp
    // block not at the specified index
    // uint zero_shift = sizeof(Word) * 8 - n_bits;
    Word left = (bits >> n_bits) << n_bits;
    
    Word b = bits - left;
    Word add = b << shift;
    if(m_valid_block) atomicAdd(&m_words[write_index], add); 
    //if(m_valid_block) print_bits(m_words[write_index]);
    // n_bits straddles the word boundary
    bool straddle = seg_start < sizeof(Word) * 8 && seg_end >= sizeof(Word) * 8;
    if(straddle)
    {
      Word rem = b >> (sizeof(Word) * 8 - shift);
      if(m_valid_block) atomicAdd(&m_words[write_index + 1], rem); 
    //  printf("Straddle "); print_bits(rem);
    }
  }

  private:
  __device__ BlockWriter()
  {
  }

};

template<int block_size>
struct BlockReader
{
  int m_current_bit;
  const int m_maxbits; 
  Word *m_words;
  Word m_buffer;
  bool m_valid_block;
  int m_block_idx;

  __device__ BlockReader(Word *b, const int &maxbits, const int &block_idx, const int &num_blocks)
    :  m_maxbits(maxbits), m_valid_block(true)
  {
    if(block_idx >= num_blocks) m_valid_block = false;
    int word_index = (block_idx * maxbits)  / (sizeof(Word) * 8); 
    m_words = b + word_index;
    m_buffer = *m_words;
    m_current_bit = (block_idx * maxbits) % (sizeof(Word) * 8); 

    m_buffer >>= m_current_bit;
    m_block_idx = block_idx;
  }

  inline __device__ 
  uint read_bit()
  {
    uint bit = m_buffer & 1;
    ++m_current_bit;
    m_buffer >>= 1;
    // handle moving into next word
    if(m_current_bit >= sizeof(Word) * 8) 
    {
      m_current_bit = 0;
      ++m_words;
      m_buffer = *m_words;
    }
    return bit; 
  }


  // note this assumes that n_bits is <= 64
  inline __device__ 
  uint read_bits(const int &n_bits)
  {
    uint bits; 
    // rem bits will always be positive
    int rem_bits = sizeof(Word) * 8 - m_current_bit;
     
    int first_read = min(rem_bits, n_bits);
    // first mask 
    Word mask = ((Word)1<<((first_read)))-1;
    bits = m_buffer & mask;
    m_buffer >>= n_bits;
    m_current_bit += first_read;
    int next_read = 0;
    if(n_bits >= rem_bits) 
    {
      ++m_words;
      m_buffer = *m_words;
      m_current_bit = 0;
      next_read = n_bits - first_read; 
    }
   
    // this is basically a no-op when first read constained 
    // all the bits. TODO: if we have aligned reads, this could 
    // be a conditional without divergence
    mask = ((Word)1<<((next_read)))-1;
    bits += (m_buffer & mask) << first_read;
    m_buffer >>= next_read;
    m_current_bit += next_read; 
    return bits;
  }

  private:
  __device__ BlockReader()
  {
  }

}; // block reader

template<typename Scalar, int Size, typename UInt>
inline __device__
void decode_ints(BlockReader<Size> &reader, uint &max_bits, UInt *data)
{
  const int intprec = get_precision<Scalar>();
  memset(data, 0, sizeof(UInt) * Size);
  unsigned int x; 
  // maxprec = 64;
  const uint kmin = 0; //= intprec > maxprec ? intprec - maxprec : 0;
  int bits = max_bits;
  for (uint k = intprec, n = 0; bits && k-- > kmin;)
  {
    // read bit plane
    uint m = MIN(n, bits);
    bits -= m;
    x = reader.read_bits(m);
    for (; n < Size && bits && (bits--, reader.read_bit()); x += (Word) 1 << n++)
      for (; n < (Size - 1) && bits && (bits--, !reader.read_bit()); n++);
    
    // deposit bit plane
    #pragma unroll
    for (int i = 0; x; i++, x >>= 1)
    {
      data[i] += (UInt)(x & 1u) << k;
    }
  } 
}


} // namespace cuZFP
#endif