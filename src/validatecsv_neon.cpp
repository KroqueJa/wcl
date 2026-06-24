/*
 * Copyright Ji Krochmal 2026
 */
#include <arm_neon.h>

#include "validatecsv_kernel.h"

// NEON per-chunk kernels for the parallel validate-csv driver -- the AArch64
// counterpart of validatecsv_scalar.cpp. csvBlindChunk (Phase 1) vectorizes the
// common no-newline 16-byte block (popcount delimiters, detect quotes) and
// segments newline-bearing blocks via the nibble masks; the < 16-byte tail and
// the record bookkeeping reuse the shared steppers in validatecsv_kernel.h, so
// the NEON and scalar kernels agree on every input. csvQuotedChunk (Phase 2)
// is added in a later task; for now it delegates to the shared scalar walk.

namespace {

// vshrn nibble-mask movemask (see words_neon.cpp): byte i -> nibble i of a u64
// (bits 4i..4i+3), 0xF if the compare lane was set else 0x0. One shift-narrow +
// one GPR move, no horizontal reduction.
inline u64 nibbleMask( const uint8x16_t cmp )
{
  return vget_lane_u64(
      vreinterpret_u64_u8( vshrn_n_u16( vreinterpretq_u16_u8( cmp ), 4 ) ), 0
  );
}

inline u64 nibbleEq( const uint8x16_t v, const u8 c )
{
  return nibbleMask( vceqq_u8( v, vdupq_n_u8( c ) ) );
}

// Nibble mask for bytes [0, byteIdx): each counted byte is a 0xF nibble.
inline u64 nibblesBelow( const usize byteIdx )
{
  return byteIdx >= 16 ? ~0ull : ( ( 1ull << ( 4 * byteIdx ) ) - 1 );
}

// Delimiter bytes selected by `mask` (a nibble mask), as a count of bytes.
inline u64 delimsIn( const u64 delimNib, const u64 mask )
{
  return static_cast<u64>( __builtin_popcountll( delimNib & mask ) ) >> 2;
}

}  // namespace

void csvBlindChunk(
    const char* buf, const usize len, const CsvDialect& d, CsvChunkSummary& out,
    bool& sawQuote
)
{
  CsvBlindState st;
  const auto* p = reinterpret_cast<const u8*>( buf );
  const u8 delim = static_cast<u8>( d.delim );
  const u8 quote = static_cast<u8>( d.quote );

  usize i = 0;
  for ( ; i + 16 <= len; i += 16 ) {
    const uint8x16_t v = vld1q_u8( p + i );
    if ( d.quoting && nibbleEq( v, quote ) != 0 ) st.sawQuote = true;
    const u64 nlNib = nibbleEq( v, static_cast<u8>( '\n' ) );
    const u64 delimNib = nibbleEq( v, delim );

    if ( nlNib == 0 ) {
      // No newline: the whole block extends the open record.
      st.cur += delimsIn( delimNib, ~0ull );
      st.sinceNl = true;
      continue;
    }

    // Newline-bearing block: close a record at each newline, counting the
    // delimiters in each inter-newline segment via popcount.
    u64 nn = nlNib;
    usize prevByte = 0;
    while ( nn != 0 ) {
      const usize b = static_cast<usize>( __builtin_ctzll( nn ) ) >> 2;
      st.cur +=
          delimsIn( delimNib, nibblesBelow( b ) & ~nibblesBelow( prevByte ) );
      csvBlindClose( st );
      prevByte = b + 1;
      nn &= ~( 0xFull << ( 4 * b ) );  // clear the whole newline nibble
    }
    st.cur += delimsIn( delimNib, ~nibblesBelow( prevByte ) );  // trailing
    st.sinceNl = prevByte < 16;  // bytes after the last newline in this block
  }

  for ( ; i < len; ++i ) csvBlindByte( p[i], d, st );  // < 16-byte tail
  csvSummaryFinish( st, out );
  sawQuote = st.sawQuote;
}

void csvQuotedChunk(
    const char* buf, const usize len, const CsvDialect& d,
    const bool entryEscaped, CsvChunkSummary& h0, CsvChunkSummary& h1
)
{
  // Phase 2 NEON kernel arrives in the next task; the shared scalar walk is
  // correct in the meantime.
  csvQuotedWalk( buf, len, d, /*entryInside=*/false, entryEscaped, h0 );
  csvQuotedWalk( buf, len, d, /*entryInside=*/true, entryEscaped, h1 );
}
