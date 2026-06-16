/*
 * Copyright Ji Krochmal 2026
 */
#include <arm_neon.h>

#include <algorithm>

#include "iswprint_table.h"
#include "words.h"
#include "words_kernel.h"

// NEON unified word counter -- the AArch64 counterpart of words_avx2.cpp, and
// a faithful port of it. Per 32-byte block it builds two 32-bit bitmasks -- S
// (separator bytes) and P (printable bytes) -- then advances the shared run
// state machine over the masks with ctz hops (see words_kernel.h). A NEON
// register is only 16 bytes wide, so each block is a pair of uint8x16_t and a
// movemask helper compacts the two halves into the 32-bit mask the kernel
// expects; that lets stepMasks / scalarUtf8 / the carry logic be reused
// verbatim.
//
// C mode: S = ASCII whitespace, P = 0x21..0x7E, every full block vectorizes.
// UTF-8 mode vectorizes exactly the content it can classify bit-for-bit like
// the scalar kernel: ASCII, plus well-formed 2-byte sequences whose lead the
// generated kCandLead table certifies as "clean". Anything else punts that one
// block to the scalar classifier, so the kernels agree on ALL input.

using namespace qwc::words_kernel;

namespace {

// One 32-byte block: two NEON registers, low half then high half.
struct Blk
{
  uint8x16_t lo;
  uint8x16_t hi;
};

inline Blk loadBlk( const u8* p )
{
  return { vld1q_u8( p ), vld1q_u8( p + 16 ) };
}

// Compact two compare results (each lane 0x00 or 0xFF) into a 32-bit mask, bit
// i = byte i. Within each 8-byte group the AND leaves distinct powers of two,
// so vaddv_u8 sums them to the group's bitmask (a 0x00 lane adds nothing).
inline u32 mm( const uint8x16_t lo, const uint8x16_t hi )
{
  static constexpr u8 kBitsArr[16] = { 1, 2, 4, 8, 16, 32, 64, 128,
                                       1, 2, 4, 8, 16, 32, 64, 128 };
  const uint8x16_t bits = vld1q_u8( kBitsArr );
  const uint8x16_t a = vandq_u8( lo, bits );
  const uint8x16_t b = vandq_u8( hi, bits );
  return static_cast<u32>( vaddv_u8( vget_low_u8( a ) ) ) |
         ( static_cast<u32>( vaddv_u8( vget_high_u8( a ) ) ) << 8 ) |
         ( static_cast<u32>( vaddv_u8( vget_low_u8( b ) ) ) << 16 ) |
         ( static_cast<u32>( vaddv_u8( vget_high_u8( b ) ) ) << 24 );
}

// Per-half in-range mask: lo <= byte <= hi, unsigned. NEON's native unsigned
// compares need no signed bias (unlike the AVX2 rangeU). Valid for the same
// lo >= 1, hi <= 0xFE uses as AVX2.
inline uint8x16_t rangeHalf( const uint8x16_t v, const u8 lo, const u8 hi )
{
  return vandq_u8(
      vcgeq_u8( v, vdupq_n_u8( lo ) ), vcleq_u8( v, vdupq_n_u8( hi ) )
  );
}

inline u32 rangeMask( const Blk& v, const u8 lo, const u8 hi )
{
  return mm( rangeHalf( v.lo, lo, hi ), rangeHalf( v.hi, lo, hi ) );
}

inline u32 eqMask( const Blk& v, const u8 c )
{
  return mm(
      vceqq_u8( v.lo, vdupq_n_u8( c ) ), vceqq_u8( v.hi, vdupq_n_u8( c ) )
  );
}

// Bytes >= 0x80 (high bit set) -- the AVX2 mm(v) sign-bit mask.
inline u32 highMask( const Blk& v )
{
  const uint8x16_t top = vdupq_n_u8( 0x80 );
  return mm( vtstq_u8( v.lo, top ), vtstq_u8( v.hi, top ) );
}

// ASCII separator/printable masks. Bytes >= 0x80 fall outside both ranges, so
// the unsigned compares reproduce the AVX2 signed-compare results exactly.
inline u32 asciiSep( const Blk& v )
{
  const uint8x16_t sp = vdupq_n_u8( 0x20 );
  const uint8x16_t a =
      vorrq_u8( rangeHalf( v.lo, 0x09, 0x0D ), vceqq_u8( v.lo, sp ) );
  const uint8x16_t b =
      vorrq_u8( rangeHalf( v.hi, 0x09, 0x0D ), vceqq_u8( v.hi, sp ) );
  return mm( a, b );
}

inline u32 asciiPrint( const Blk& v )
{
  return mm( rangeHalf( v.lo, 0x21, 0x7E ), rangeHalf( v.hi, 0x21, 0x7E ) );
}

}  // namespace

void words(
    const char* buf, const usize len, const usize ownedBegin,
    const usize ownedEnd, WordScan& s, const WordsMode& m
)
{
  const u8* base = reinterpret_cast<const u8*>( buf );
  usize i = ownedBegin;

  if ( !m.utf8 ) {
    // C parameterization: purely bytewise, every full block vectorizes.
    for ( ; i + 32 <= ownedEnd; i += 32 ) {
      const Blk v = loadBlk( base + i );
      stepMasks( asciiSep( v ), asciiPrint( v ), 32, s );
    }
    for ( ; i < ownedEnd; ++i ) step( classifyC( base[i] ), s );
    return;
  }

  // UTF-8 parameterization. Each vector block needs one byte of lookahead (the
  // second byte of a window whose lead sits at bit 31), hence the i + 33 <= len
  // bound. The carries thread a block-straddling sequence into the next block:
  // carryLead = a clean lead at bit 31 (its continuation is the next block's
  // bit 0), carryS/carryN = that continuation's smeared class. They live in
  // locals, not WordScan, because no block ever straddles a words() call -- the
  // epilogue is always scalar.
  u32 carryS = 0, carryN = 0, carryLead = 0;
  while ( i + 33 <= len && i + 32 <= ownedEnd ) {
    const Blk v = loadBlk( base + i );
    u32 sMask = asciiSep( v );
    u32 pMask = asciiPrint( v );
    const u32 high = highMask( v );
    if ( high != 0 ) {
      // The block vectorizes only if every high byte is part of a well-formed
      // 2-byte sequence (lead C2..DF + exactly one continuation, including a
      // pair straddling the block edge) whose lead is clean per kCandLead.
      const u32 lead2 = rangeMask( v, 0xC2, 0xDF );
      const u32 cont = rangeMask( v, 0x80, 0xBF );
      bool clean =
          high == ( lead2 | cont ) && cont == ( ( lead2 << 1 ) | carryLead );
      if ( clean && ( lead2 >> 31 ) != 0 && ( base[i + 32] & 0xC0 ) != 0x80 )
        clean = false;  // lead at bit 31 with no continuation after the block
      if ( clean ) {
        u32 leads = lead2;
        while ( leads != 0 ) {
          const u32 b = static_cast<u32>( __builtin_ctz( leads ) );
          leads &= leads - 1;
          if ( kCandLead[base[i + b]] != 0 ) {
            clean = false;
            break;
          }
        }
      }
      if ( !clean ) {
        // Scalar-classify just this block; it consumes whole code points, so
        // resume at the first unconsumed byte with no pending carries.
        i = scalarUtf8(
            base, len, i, std::min( i + 32, ownedEnd ), s, m.nbspace
        );
        carryS = carryN = carryLead = 0;
        continue;
      }

      // Exact C2 windows, masked at the lead position via 1-byte lookahead.
      const Blk v1 = loadBlk( base + i + 1 );
      const u32 isC2 = eqMask( v, 0xC2 );
      const u32 s2 = m.nbspace ? isC2 & eqMask( v1, 0xA0 )
                               : 0;  // POSIXLY_CORRECT: NBSP is printable
      const u32 n2 = isC2 & rangeMask( v1, 0x80, 0x9F );  // C1 controls

      // Smear each window across both bytes of its sequence; every other high
      // byte belongs to a printable 2-byte code point (kCandLead certified).
      const u32 sSm = s2 | ( s2 << 1 ) | carryS;
      const u32 nSm = n2 | ( n2 << 1 ) | carryN;
      sMask |= sSm;
      pMask |= high & ~( sSm | nSm );
      pMask &= ~( sSm | nSm );
      carryS = s2 >> 31;
      carryN = n2 >> 31;
      carryLead = lead2 >> 31;
    } else {
      carryS = carryN = carryLead = 0;
    }
    stepMasks( sMask, pMask, 32, s );
    i += 32;
  }

  scalarUtf8( base, len, i, ownedEnd, s, m.nbspace );
}
