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
// the scalar kernel: ASCII, plus well-formed 2-byte sequences whose lead the
// generated kCandLead table certifies as "clean" (every code point in its row
// is printable, bar the exact C2 windows: U+0080-009F non-printable, U+00A0
// the nbspace separator). Anything else -- 3/4-byte sequences, invalid bytes,
// dirty leads like 0xCE (the U+03A2 unassigned hole) -- punts that one block
// to the scalar classifier, so the two kernels agree on ALL input, not just
// valid UTF-8. ASCII-dominant and Latin-ish text stays fully vectorized.

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

  // UTF-8 parameterization. Each vector block needs one byte of lookahead
  // (the second byte of a window whose lead sits at bit 31), hence the
  // i + 33 <= len bound. The carries thread a block-straddling sequence into
  // the next block: carryLead = a clean lead at bit 31 (its continuation is
  // the next block's bit 0), carryS/carryN = that continuation's smeared
  // class. They live in locals, not WordScan, because no block ever straddles
  // a words() call -- the epilogue is always scalar.
  u32 carryS = 0, carryN = 0, carryLead = 0;
  while ( i + 33 <= len && i + 32 <= ownedEnd ) {
    const __m256i v =
        _mm256_loadu_si256( reinterpret_cast<const __m256i*>( base + i ) );
    u32 sMask = asciiSep( v );
    u32 pMask = asciiPrint( v );
    const u32 high = mm( v );
    if ( high != 0 ) {
      // The block vectorizes only if every high byte is part of a well-formed
      // 2-byte sequence (lead C2..DF + exactly one continuation, including a
      // pair straddling the block edge) whose lead is clean per kCandLead.
      const u32 lead2 = mm( rangeU( v, 0xC2, 0xDF ) );
      const u32 cont = mm( rangeU( v, 0x80, 0xBF ) );
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
      carryLead = lead2 >> 31;
    } else {
      carryS = carryN = carryLead = 0;
    }
    stepMasks( sMask, pMask, 32, s );
    i += 32;
  }

  scalarUtf8( base, len, i, ownedEnd, s, m.nbspace );
}
