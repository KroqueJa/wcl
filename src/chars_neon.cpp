/*
 * Copyright Ji Krochmal 2026
 */
#include <arm_neon.h>

#include "chars.h"

// NEON character counter -- the vectorized counterpart of chars_scalar.cpp.
//
// A UTF-8 code point is one leading byte followed by zero or more continuation
// bytes (10xxxxxx), so the character count is the number of bytes that are NOT
// continuation bytes. Equivalently -- and more SIMD-friendly -- it is the total
// byte count minus the number of continuation bytes. Counting continuation
// bytes is the same shape as countlines (count bytes matching a predicate), so
// this mirrors count_neon: detect 10xxxxxx with one vand+vceq per vector and
// accumulate. Each byte is classified on its own, so the result is independent
// of where the buffer is split -- no cross-chunk stitching needed.
usize chars( const char* buffer, const usize length )
{
  const uint8x16_t contBits = vdupq_n_u8( 0xC0 );  // mask off the top two bits
  const uint8x16_t contTag = vdupq_n_u8( 0x80 );  // ...continuation == 10xxxxxx

  usize cont = 0;  // number of UTF-8 continuation bytes
  const u8* tmp = reinterpret_cast<const u8*>( buffer );
  usize processedBytes = 0;

  // Align to a 16-byte boundary with a scalar prologue.
  while ( processedBytes < length && reinterpret_cast<usize>( tmp ) % 16 != 0
  ) {
    if ( ( *tmp & 0xC0 ) == 0x80 ) ++cont;
    ++tmp;
    ++processedBytes;
  }

  // Main loop: 64 bytes per iteration across 4 byte-accumulators. vceqq_u8
  // yields 0xFF (== -1) on a continuation byte, so vsubq_u8 adds 1 per hit
  // straight into the accumulator -- no per-iteration horizontal reduction.
  // Each lane gains at most 1 per iteration and saturates at 255, so we drain
  // the accumulators into `cont` only once every 255 iterations.
  while ( processedBytes + 64 <= length ) {
    const usize remIters = ( length - processedBytes ) / 64;
    const usize block = remIters < 255 ? remIters : 255;

    uint8x16_t acc0 = vdupq_n_u8( 0 );
    uint8x16_t acc1 = vdupq_n_u8( 0 );
    uint8x16_t acc2 = vdupq_n_u8( 0 );
    uint8x16_t acc3 = vdupq_n_u8( 0 );

    for ( usize b = 0; b < block; ++b ) {
      acc0 = vsubq_u8(
          acc0, vceqq_u8( vandq_u8( vld1q_u8( tmp ), contBits ), contTag )
      );
      acc1 = vsubq_u8(
          acc1, vceqq_u8( vandq_u8( vld1q_u8( tmp + 16 ), contBits ), contTag )
      );
      acc2 = vsubq_u8(
          acc2, vceqq_u8( vandq_u8( vld1q_u8( tmp + 32 ), contBits ), contTag )
      );
      acc3 = vsubq_u8(
          acc3, vceqq_u8( vandq_u8( vld1q_u8( tmp + 48 ), contBits ), contTag )
      );
      tmp += 64;
      processedBytes += 64;
    }

    // vaddlvq_u8 widens to u16 while summing all 16 lanes (max 16*255 = 4080).
    cont += vaddlvq_u8( acc0 );
    cont += vaddlvq_u8( acc1 );
    cont += vaddlvq_u8( acc2 );
    cont += vaddlvq_u8( acc3 );
  }

  // Scalar epilogue for the remaining < 64 bytes.
  while ( processedBytes < length ) {
    if ( ( *tmp & 0xC0 ) == 0x80 ) ++cont;
    ++tmp;
    ++processedBytes;
  }

  // Characters = total bytes that are not continuation bytes.
  return length - cont;
}
