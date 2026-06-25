/*
 * Copyright Ji Krochmal 2026
 */
#include <arm_neon.h>

#include <algorithm>

#include "iswprint_table.h"
#include "words.h"
#include "words_kernel.h"

// NEON unified word counter -- the AArch64 counterpart of words_avx2.cpp.
// Builds per-byte S (separator) and P (printable) masks and advances the shared
// run state machine over them (see words_kernel.h); scalarUtf8 and the carry
// logic are reused either way.
//
// NEON has no native movemask, so it is emulated, and there are two builds
// selected by QWC_NEON_NIBBLE (CMake; default ON):
//   ON  (default): the vshrn "shift-narrow" trick over 16-byte blocks -> u64
//     nibble masks (4 bits per byte), stepped by the NEON-local
//     stepMasksNibble. No horizontal reduction; faster on the common UTF-8 and
//     C word paths.
//   OFF (fallback): 32-byte blocks (two uint8x16_t) compacted to 32-bit bit
//     masks via vaddv, stepped by the shared stepMasks. The A/B baseline for
//     the open dense-2-byte regression (qwc-companion Finding 15).
//
// C mode: S = ASCII whitespace, P = 0x21..0x7E, every full block vectorizes.
// UTF-8 mode vectorizes exactly the content it can classify bit-for-bit like
// the scalar kernel: ASCII, plus well-formed 2- AND 3-byte sequences whose lead
// (resp. (lead, cont1) sub-row) the generated kCandLead / kCandLead3 tables
// certify as "clean" (every code point in the row/sub-row is printable and
// non-separator, bar the exact C2 windows: U+0080-009F non-printable, U+00A0
// the nbspace separator). Anything else -- 4-byte sequences, invalid bytes,
// dirty leads like 0xCE (the U+03A2 unassigned hole) or 0xE3 (contains U+3000
// ideographic space) -- punts that one block to the scalar classifier, so the
// kernels agree on ALL input. ASCII-dominant, Latin-ish and CJK / Hangul /
// Devanagari text stays fully vectorized.

using namespace qwc::words_kernel;

namespace {

#if !QWC_NEON_NIBBLE
// One 32-byte block: two NEON registers, low half then high half.
struct Blk
{
  uint8x16_t lo;
  uint8x16_t hi;
};

inline Blk loadBlk( const u8* p )
{ return { vld1q_u8( p ), vld1q_u8( p + 16 ) }; }

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
{ return mm( rangeHalf( v.lo, lo, hi ), rangeHalf( v.hi, lo, hi ) ); }

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
{ return mm( rangeHalf( v.lo, 0x21, 0x7E ), rangeHalf( v.hi, 0x21, 0x7E ) ); }

// Walk the set bits of lead3 and probe kCandLead3 for each. Mirrors the AVX2
// helper of the same name -- pure scalar, no NEON -- so the table probe is
// identical across ISAs. The [[gnu::noinline, gnu::cold]] pair was measured on
// GCC/x86 to fix a binary-layout regression in words() (see words_avx2.cpp for
// the full rationale and qwc-companion/CLAUDE.md "Diagnosing small
// regressions"). Kept here to match the AVX2 structure; on clang/AArch64 the
// inliner heuristics differ, so its effect on this side is an empirical
// question settled by the C-locale + CJK sweep, not assumed.
[[gnu::noinline, gnu::cold]] bool lead3SubrowDirty(
    const u8* p, const u32 lead3
)
{
  u32 leads = lead3;
  while ( leads != 0 ) {
    const u32 b = static_cast<u32>( __builtin_ctz( leads ) );
    leads &= leads - 1;
    const u8 L = p[b];
    const u8 C = p[b + 1];  // in-block (b<=29) or lookahead (b=30,31)
    if ( kCandLead3[( ( L - 0xE0u ) << 6 ) | ( C & 0x3Fu )] != 0 ) return true;
  }
  return false;
}
#endif  // !QWC_NEON_NIBBLE

#if QWC_NEON_NIBBLE
// vshrn nibble-mask movemask: byte i -> nibble i of a u64 (bits 4i..4i+3),
// value 0xF if the compare lane was set else 0x0. One shift-narrow + one GPR
// move, no horizontal reduction (cf. mm()'s 4x vaddv per 32 bytes). Little
// endian (AArch64): vget_lane_u64 reads narrowed[0] as the low byte, so byte i
// lands in nibble i.
inline u64 nibbleMask( const uint8x16_t cmp )
{
  return vget_lane_u64(
      vreinterpret_u64_u8( vshrn_n_u16( vreinterpretq_u16_u8( cmp ), 4 ) ), 0
  );
}

inline u64 nibbleSep( const uint8x16_t v )
{
  const uint8x16_t ws = vorrq_u8(
      vandq_u8(
          vcgeq_u8( v, vdupq_n_u8( 0x09 ) ), vcleq_u8( v, vdupq_n_u8( 0x0D ) )
      ),
      vceqq_u8( v, vdupq_n_u8( 0x20 ) )
  );
  return nibbleMask( ws );
}

inline u64 nibblePrint( const uint8x16_t v )
{
  return nibbleMask( vandq_u8(
      vcgeq_u8( v, vdupq_n_u8( 0x21 ) ), vcleq_u8( v, vdupq_n_u8( 0x7E ) )
  ) );
}

// UTF-8 nibble helpers: the same compares as the Blk-based rangeMask / eqMask /
// highMask, but over a single 16-byte register compacted to a u64 nibble mask.
inline u64 nibbleRange( const uint8x16_t v, const u8 lo, const u8 hi )
{
  return nibbleMask( vandq_u8(
      vcgeq_u8( v, vdupq_n_u8( lo ) ), vcleq_u8( v, vdupq_n_u8( hi ) )
  ) );
}

inline u64 nibbleEq( const uint8x16_t v, const u8 c )
{ return nibbleMask( vceqq_u8( v, vdupq_n_u8( c ) ) ); }

inline u64 nibbleHigh( const uint8x16_t v )
{ return nibbleMask( vtstq_u8( v, vdupq_n_u8( 0x80 ) ) ); }

// Nibble-stride twin of lead3SubrowDirty: walk the SET NIBBLES of lead3,
// clearing the whole nibble each step (`leads &= leads - 1` would drop only one
// of the four bits), and probe kCandLead3 for each 3-byte lead.
[[gnu::noinline, gnu::cold]] bool lead3SubrowDirtyNibble(
    const u8* p, const u64 lead3
)
{
  u64 leads = lead3;
  while ( leads != 0 ) {
    const u32 b = static_cast<u32>( __builtin_ctzll( leads ) ) >> 2;
    leads &= ~( 0xFull << ( 4 * b ) );
    const u8 L = p[b];
    const u8 C = p[b + 1];  // in-block (b<=14) or lookahead (b=15)
    if ( kCandLead3[( ( L - 0xE0u ) << 6 ) | ( C & 0x3Fu )] != 0 ) return true;
  }
  return false;
}

// Nibble-stride port of words_kernel.h::stepMasks. Masks are u64 with 4 bits
// per byte (byte i in bits 4i..4i+3, value 0xF or 0x0); n = byte count (16 for
// full blocks). 16 nibbles fill the u64 exactly, so the top-boundary "all
// remaining are X" cases fall out of the word width with no padding mask.
// NEON-local on purpose: the shared stepMasks is bit-per-byte and the AVX2
// kernel consumes it, so it must not be perturbed.
inline void stepMasksNibble(
    const u64 sMask, const u64 pMask, const u32 n, WordScan& s
)
{
  const u64 nonSep = ~sMask;
  if ( n == 16 && ( nonSep & ~pMask ) == 0 ) {
    if ( !s.sawByte ) {
      s.sawByte = true;
      s.startsInWord = ( nonSep & 0xFull ) != 0;
    }
    const u64 ends = sMask & ( ( nonSep << 4 ) | ( s.inWord ? 0xFull : 0ull ) );
    u32 ended = static_cast<u32>( __builtin_popcountll( ends ) ) >> 2;
    if ( ( ends & 0xFull ) && !s.runHasPrintable ) --ended;
    s.words += ended;
    if ( sMask != 0 ) {
      if ( !s.sawSeparator && s.startsInWord ) {
        s.leadingEnded = true;
        s.leadingHasPrintable =
            ( static_cast<u32>( __builtin_ctzll( sMask ) ) >> 2 ) > 0 ||
            s.runHasPrintable;
      }
      s.sawSeparator = true;
      const bool open = ( sMask >> 60 ) == 0;
      s.inWord = open;
      s.runHasPrintable = open;
    } else {
      s.inWord = true;
      s.runHasPrintable = true;
    }
    return;
  }

  u32 b = 0;
  while ( b < n ) {
    if ( ( sMask >> ( 4 * b ) ) & 0xFull ) {
      if ( !s.sawByte ) {
        s.sawByte = true;
        s.startsInWord = false;
      }
      if ( s.inWord ) {
        if ( s.runHasPrintable ) ++s.words;
        if ( !s.sawSeparator && s.startsInWord ) {
          s.leadingEnded = true;
          s.leadingHasPrintable = s.runHasPrintable;
        }
        s.inWord = false;
        s.runHasPrintable = false;
      }
      s.sawSeparator = true;
      const u64 rest = ~( sMask >> ( 4 * b ) );
      b += rest == 0 ? ( n - b )
                     : ( static_cast<u32>( __builtin_ctzll( rest ) ) >> 2 );
    } else {
      if ( !s.sawByte ) {
        s.sawByte = true;
        s.startsInWord = true;
      }
      s.inWord = true;
      const u64 rest = sMask >> ( 4 * b );
      const u32 len =
          rest == 0 ? ( n - b )
                    : ( static_cast<u32>( __builtin_ctzll( rest ) ) >> 2 );
      const u64 segMask = len >= 16 ? ~0ull : ( ( 1ull << ( 4 * len ) ) - 1 );
      if ( ( pMask >> ( 4 * b ) ) & segMask ) s.runHasPrintable = true;
      b += len;
    }
  }
}
#endif  // QWC_NEON_NIBBLE

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
#if QWC_NEON_NIBBLE
    for ( ; i + 16 <= ownedEnd; i += 16 ) {
      const uint8x16_t v = vld1q_u8( base + i );
      stepMasksNibble( nibbleSep( v ), nibblePrint( v ), 16, s );
    }
#else
    for ( ; i + 32 <= ownedEnd; i += 32 ) {
      const Blk v = loadBlk( base + i );
      stepMasks( asciiSep( v ), asciiPrint( v ), 32, s );
    }
#endif
    for ( ; i < ownedEnd; ++i ) step( classifyC( base[i] ), s );
    return;
  }

  // UTF-8 parameterization. Each vector block needs up to two bytes of
  // lookahead (a clean 3-byte lead at bit 31 needs cont1 at byte 32 and cont2
  // at byte 33), hence the i + 34 <= len bound. The carries thread a
  // block-straddling sequence into the next block. carryLead2 = a clean 2-byte
  // lead at bit 31 (cont at next bit 0). carryLead3a = a clean 3-byte lead at
  // bit 30 (cont1 in-block at bit 31, cont2 spills to next bit 0).
  // carryLead3b = a clean 3-byte lead at bit 31 (cont1 spills to next bit 0,
  // cont2 to next bit 1). carryS/carryN smear the C2 windows' separator /
  // non-printable class across both bytes of a 2-byte sequence that straddles
  // the edge. All live in locals, not WordScan, because no block ever straddles
  // a words() call -- the epilogue is always scalar.
#if QWC_NEON_NIBBLE
  // Nibble path: 16-byte blocks -> u64 masks (4 bits/byte). Lookahead is
  // i + 18 <= len (a clean 3-byte lead at byte 15 needs cont1 at 16, cont2 at
  // 17). Every stride is x4 vs the 32-byte path: << 1 -> << 4, << 2 -> << 8,
  // >> 31 -> >> 60, (>> 30 & 1) -> (>> 56 & 0xF). Carries are u64 nibble values
  // (0xF / 0x0) at nibble 0.
  u64 carryS = 0, carryN = 0;
  u64 carryLead2 = 0, carryLead3a = 0, carryLead3b = 0;
  while ( i + 18 <= len && i + 16 <= ownedEnd ) {
    const uint8x16_t v = vld1q_u8( base + i );
    u64 sMask = nibbleSep( v );
    u64 pMask = nibblePrint( v );
    const u64 high = nibbleHigh( v );
    if ( high != 0 ) {
      const u64 lead2 = nibbleRange( v, 0xC2, 0xDF );
      const u64 lead3 = nibbleRange( v, 0xE0, 0xEF );
      const u64 cont = nibbleRange( v, 0x80, 0xBF );
      const u64 carryC1 = carryLead2 | carryLead3a | carryLead3b;  // -> nib 0
      const u64 carryC2 = carryLead3b << 4;                        // -> nib 1
      bool clean = high == ( lead2 | lead3 | cont ) &&
                   cont == ( ( lead2 << 4 ) | ( lead3 << 4 ) | ( lead3 << 8 ) |
                             carryC1 | carryC2 );
      // Edge-byte validation: a lead whose continuations spill past byte 15
      // must see actual continuation bytes in the lookahead.
      if ( clean ) {
        const bool needB16 = ( lead2 >> 60 ) != 0 ||
                             ( ( lead3 >> 56 ) & 0xFull ) != 0 ||
                             ( lead3 >> 60 ) != 0;
        const bool needB17 = ( lead3 >> 60 ) != 0;
        if ( needB16 && ( base[i + 16] & 0xC0 ) != 0x80 ) clean = false;
        if ( clean && needB17 && ( base[i + 17] & 0xC0 ) != 0x80 )
          clean = false;
      }
      if ( clean ) {
        u64 leads = lead2;
        while ( leads != 0 ) {
          const u32 b = static_cast<u32>( __builtin_ctzll( leads ) ) >> 2;
          leads &= ~( 0xFull << ( 4 * b ) );
          if ( kCandLead[base[i + b]] != 0 ) {
            clean = false;
            break;
          }
        }
      }
      if ( clean && lead3 != 0 && lead3SubrowDirtyNibble( base + i, lead3 ) )
        clean = false;
      if ( !clean ) {
        i = scalarUtf8(
            base, len, i, std::min( i + 16, ownedEnd ), s, m.nbspace
        );
        carryS = carryN = 0;
        carryLead2 = carryLead3a = carryLead3b = 0;
        continue;
      }

      const uint8x16_t v1 = vld1q_u8( base + i + 1 );
      const u64 isC2 = nibbleEq( v, 0xC2 );
      const u64 s2 = m.nbspace ? isC2 & nibbleEq( v1, 0xA0 ) : 0;
      const u64 n2 = isC2 & nibbleRange( v1, 0x80, 0x9F );  // C1 controls

      const u64 sSm = s2 | ( s2 << 4 ) | carryS;
      const u64 nSm = n2 | ( n2 << 4 ) | carryN;
      sMask |= sSm;
      pMask |= high & ~( sSm | nSm );
      pMask &= ~( sSm | nSm );
      carryS = s2 >> 60;
      carryN = n2 >> 60;
      carryLead2 = lead2 >> 60;
      carryLead3a = ( lead3 >> 56 ) & 0xFull;
      carryLead3b = lead3 >> 60;
    } else {
      carryS = carryN = 0;
      carryLead2 = carryLead3a = carryLead3b = 0;
    }
    stepMasksNibble( sMask, pMask, 16, s );
    i += 16;
  }
#else
  // 32-byte blocks -> two uint8x16_t -> 32-bit masks.
  u32 carryS = 0, carryN = 0;
  u32 carryLead2 = 0, carryLead3a = 0, carryLead3b = 0;
  while ( i + 34 <= len && i + 32 <= ownedEnd ) {
    const Blk v = loadBlk( base + i );
    u32 sMask = asciiSep( v );
    u32 pMask = asciiPrint( v );
    const u32 high = highMask( v );
    if ( high != 0 ) {
      // The block vectorizes only if every high byte is part of a well-formed
      // 2- or 3-byte sequence (lead + exact number of continuations, including
      // a window straddling the block edge) whose lead (resp. (lead, cont1)
      // sub-row) is clean per kCandLead / kCandLead3.
      const u32 lead2 = rangeMask( v, 0xC2, 0xDF );
      const u32 lead3 = rangeMask( v, 0xE0, 0xEF );
      const u32 cont = rangeMask( v, 0x80, 0xBF );
      // Carry contributions to the "expected continuation positions" of THIS
      // block. carryLead3b forces both bit 0 (cont1) and bit 1 (cont2).
      const u32 carryC1 = carryLead2 | carryLead3a | carryLead3b;  // -> bit 0
      const u32 carryC2 = carryLead3b << 1;                        // -> bit 1
      bool clean = high == ( lead2 | lead3 | cont ) &&
                   cont == ( ( lead2 << 1 ) | ( lead3 << 1 ) | ( lead3 << 2 ) |
                             carryC1 | carryC2 );
      // Edge-byte validation: any lead whose continuations spill past bit 31
      // must see actual continuation bytes in the lookahead.
      if ( clean ) {
        const bool needB32 = ( lead2 >> 31 ) != 0 ||
                             ( ( lead3 >> 30 ) & 1u ) != 0 ||
                             ( lead3 >> 31 ) != 0;
        const bool needB33 = ( lead3 >> 31 ) != 0;
        if ( needB32 && ( base[i + 32] & 0xC0 ) != 0x80 ) clean = false;
        if ( clean && needB33 && ( base[i + 33] & 0xC0 ) != 0x80 )
          clean = false;
      }
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
      if ( clean && lead3 != 0 && lead3SubrowDirty( base + i, lead3 ) )
        clean = false;
      if ( !clean ) {
        // Scalar-classify just this block; it consumes whole code points, so
        // resume at the first unconsumed byte with no pending carries.
        i = scalarUtf8(
            base, len, i, std::min( i + 32, ownedEnd ), s, m.nbspace
        );
        carryS = carryN = 0;
        carryLead2 = carryLead3a = carryLead3b = 0;
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
      carryLead2 = lead2 >> 31;
      carryLead3a = ( lead3 >> 30 ) & 1u;
      carryLead3b = lead3 >> 31;
    } else {
      carryS = carryN = 0;
      carryLead2 = carryLead3a = carryLead3b = 0;
    }
    stepMasks( sMask, pMask, 32, s );
    i += 32;
  }
#endif

  scalarUtf8( base, len, i, ownedEnd, s, m.nbspace );
}
