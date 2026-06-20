/*
 * Copyright Ji Krochmal 2026
 */
#include <immintrin.h>

#include <algorithm>

#include "iswprint_table.h"
#include "words.h"
#include "words_kernel.h"

// AVX2 unified word counter. Per 32-byte block it builds two bitmasks -- S
// (separator bytes) and P (printable bytes) -- in vectors, then advances the
// shared run state machine over the masks with ctz hops (see words_kernel.h;
// separators are sparse, so the walk visits runs, not bytes). This TU supplies
// only the AVX2 mask primitives and the words() driver; the decoder,
// classifiers and mask state machine come from the kernel header, shared with
// words_neon.cpp.
//
// C mode: S = ASCII whitespace, P = 0x21..0x7E, every full block vectorizes.
// UTF-8 mode vectorizes exactly the content it can classify bit-for-bit like
// the scalar kernel: ASCII, plus well-formed 2- AND 3-byte sequences whose
// lead (resp. (lead, cont1) sub-row) the generated kCandLead / kCandLead3
// tables certify as "clean" (every code point in the row/sub-row is printable
// and non-separator, bar the exact C2 windows: U+0080-009F non-printable,
// U+00A0 the nbspace separator). Anything else -- 4-byte sequences, invalid
// bytes, dirty leads like 0xCE (the U+03A2 unassigned hole) or 0xE3 (contains
// U+3000 ideographic space) -- punts that one block to the scalar
// classifier, so the two kernels agree on ALL input, not just valid UTF-8.
// ASCII-dominant, Latin-ish and CJK / Hangul / Devanagari text stays fully
// vectorized.

using namespace qwc::words_kernel;

namespace {

// Unsigned in-range compare (bytes >= 0x80 are signed-negative, so bias by
// 0x80 first). Valid for lo >= 1 and hi <= 0xFE -- all uses here qualify.
inline __m256i rangeU( const __m256i v, const u8 lo, const u8 hi )
{
  const __m256i x =
      _mm256_xor_si256( v, _mm256_set1_epi8( static_cast<char>( 0x80 ) ) );
  return _mm256_and_si256(
      _mm256_cmpgt_epi8(
          x, _mm256_set1_epi8( static_cast<char>( ( lo ^ 0x80 ) - 1 ) )
      ),
      _mm256_cmpgt_epi8(
          _mm256_set1_epi8( static_cast<char>( ( hi ^ 0x80 ) + 1 ) ), x
      )
  );
}

inline u32 mm( const __m256i v )
{
  return static_cast<u32>( _mm256_movemask_epi8( v ) );
}

// ASCII separator/printable masks for one block. Bytes >= 0x80 compare
// signed-negative, so they fall in neither mask.
inline u32 asciiSep( const __m256i v )
{
  const __m256i tabCr = _mm256_and_si256(
      _mm256_cmpgt_epi8( v, _mm256_set1_epi8( 0x08 ) ),
      _mm256_cmpgt_epi8( _mm256_set1_epi8( 0x0E ), v )
  );
  return mm(
      _mm256_or_si256( tabCr, _mm256_cmpeq_epi8( v, _mm256_set1_epi8( 0x20 ) ) )
  );
}

inline u32 asciiPrint( const __m256i v )
{
  return mm( _mm256_and_si256(
      _mm256_cmpgt_epi8( v, _mm256_set1_epi8( 0x20 ) ),
      _mm256_cmpgt_epi8( _mm256_set1_epi8( 0x7F ), v )
  ) );
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
      const __m256i v =
          _mm256_loadu_si256( reinterpret_cast<const __m256i*>( base + i ) );
      stepMasks( asciiSep( v ), asciiPrint( v ), 32, s );
    }
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
  // the edge. All live in locals, not WordScan, because no block ever
  // straddles a words() call -- the epilogue is always scalar.
  u32 carryS = 0, carryN = 0;
  u32 carryLead2 = 0, carryLead3a = 0, carryLead3b = 0;
  while ( i + 34 <= len && i + 32 <= ownedEnd ) {
    const __m256i v =
        _mm256_loadu_si256( reinterpret_cast<const __m256i*>( base + i ) );
    u32 sMask = asciiSep( v );
    u32 pMask = asciiPrint( v );
    const u32 high = mm( v );
    if ( high != 0 ) {
      // The block vectorizes only if every high byte is part of a well-formed
      // 2- or 3-byte sequence (lead + exact number of continuations, including
      // a window straddling the block edge) whose lead (resp. (lead, cont1)
      // sub-row) is clean per kCandLead / kCandLead3.
      const u32 lead2 = mm( rangeU( v, 0xC2, 0xDF ) );
      const u32 lead3 = mm( rangeU( v, 0xE0, 0xEF ) );
      const u32 cont = mm( rangeU( v, 0x80, 0xBF ) );
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
      if ( clean ) {
        u32 leads = lead3;
        while ( leads != 0 ) {
          const u32 b = static_cast<u32>( __builtin_ctz( leads ) );
          leads &= leads - 1;
          const u8 L = base[i + b];
          const u8 C =
              base[i + b + 1];  // in-block (b<=29) or lookahead (b=30,31)
          if ( kCandLead3[( ( L - 0xE0u ) << 6 ) | ( C & 0x3Fu )] != 0 ) {
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
        carryS = carryN = 0;
        carryLead2 = carryLead3a = carryLead3b = 0;
        continue;
      }

      // Exact C2 windows, masked at the lead position via 1-byte lookahead.
      const __m256i v1 = _mm256_loadu_si256(
          reinterpret_cast<const __m256i*>( base + i + 1 )
      );
      const u32 isC2 = mm( _mm256_cmpeq_epi8( v, _mm256_set1_epi8( '\xC2' ) ) );
      const u32 s2 =
          m.nbspace
              ? isC2 & mm( _mm256_cmpeq_epi8( v1, _mm256_set1_epi8( '\xA0' ) ) )
              : 0;  // POSIXLY_CORRECT: NBSP is printable word content instead
      const u32 n2 = isC2 & mm( rangeU( v1, 0x80, 0x9F ) );  // C1 controls

      // Smear each window across both bytes of its sequence; every other
      // high byte belongs to a printable 2-byte code point (that is what
      // kCandLead certified).
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

  scalarUtf8( base, len, i, ownedEnd, s, m.nbspace );
}
