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
// the NEON and scalar kernels agree on every input.
//
// csvQuotedChunk (Phase 2) is gated by QWC_CSV_NEON_PHASE2 (CMake, default ON):
//   ON : the in-quote mask is built with a bit-per-byte movemask + prefix-XOR
//        (vmull_p64/PMULL where available, else a shift-XOR ladder); both entry
//        hypotheses share that one construction (Q_in = ~Q_out). The backslash
//        (--esc) dialect additionally clears escaped quotes/delims/newlines via
//        the simdjson odd-backslash-run mask (findEscaped16) before the
//        prefix-XOR, carried across blocks by prevEscaped.
//   OFF: the whole of Phase 2 delegates to the shared scalar walk (the A/B
//        baseline for measuring whether the SIMD construction pays off).

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

#if QWC_CSV_NEON_PHASE2

namespace {

// Bit-per-byte movemask of a compare result (each lane 0x00 / 0xFF): bit i set
// iff byte i matched. The vaddv "powers of two" reduction (cf. words_neon.cpp
// mm()), needed -- unlike the nibble mask -- because the prefix-XOR below
// operates one bit per byte.
inline u32 movemask16( const uint8x16_t cmp )
{
  static const u8 kBits[16] = { 1, 2, 4, 8, 16, 32, 64, 128,
                                1, 2, 4, 8, 16, 32, 64, 128 };
  const uint8x16_t a = vandq_u8( cmp, vld1q_u8( kBits ) );
  const u32 lo = vaddv_u8( vget_low_u8( a ) );
  const u32 hi = vaddv_u8( vget_high_u8( a ) );
  return lo | ( hi << 8 );
}

// Inclusive prefix-XOR of the low 16 bits: result bit i = XOR of input bits
// 0..i, i.e. the in-quote state at byte i for an entry-outside chunk.
inline u32 prefixXor16( u32 x )
{
#if defined( __ARM_FEATURE_AES ) || defined( __ARM_FEATURE_CRYPTO )
  // Carryless-multiply by all-ones is the prefix-XOR (the simdjson idiom).
  const poly128_t prod = vmull_p64(
      static_cast<poly64_t>( static_cast<u64>( x & 0xFFFFu ) ),
      static_cast<poly64_t>( ~0ull )
  );
  return static_cast<u32>( vgetq_lane_u64( vreinterpretq_u64_p128( prod ), 0 )
         ) &
         0xFFFFu;
#else
  // Kogge-Stone prefix-XOR (no crypto extension): 4 steps cover 16 bits.
  x &= 0xFFFFu;
  x ^= x << 1;
  x ^= x << 2;
  x ^= x << 4;
  x ^= x << 8;
  return x & 0xFFFFu;
#endif
}

// Bits for bytes [0, b): a bit-per-byte version of nibblesBelow.
inline u32 maskBelow16( const u32 b )
{
  return b >= 16 ? 0xFFFFu : ( ( 1u << b ) - 1u );
}

// Segment one 16-byte block given its real (outside-quote) delimiter and
// newline bit masks, threading the shared record state. Mirrors csvBlindChunk's
// nibble segmentation, but bit-per-byte (so ctz / popcount are direct).
inline void segmentBits16( const u32 realDelim, u32 realNl, CsvBlindState& st )
{
  if ( realNl == 0 ) {
    st.cur += static_cast<u64>( __builtin_popcount( realDelim ) );
    st.sinceNl = true;  // 16 bytes of content, no real newline
    return;
  }
  u32 prevByte = 0;
  while ( realNl != 0 ) {
    const u32 b = static_cast<u32>( __builtin_ctz( realNl ) );
    st.cur += static_cast<u64>( __builtin_popcount(
        realDelim & maskBelow16( b ) & ~maskBelow16( prevByte )
    ) );
    csvBlindClose( st );
    prevByte = b + 1;
    realNl &= realNl - 1;  // clear the lowest set bit (one bit per byte)
  }
  st.cur += static_cast<u64>(
      __builtin_popcount( realDelim & ~maskBelow16( prevByte ) )
  );
  st.sinceNl = prevByte < 16;
}

// Per-byte quote-aware step for the < 16-byte tail (RFC-4180: no escapes).
inline void quotedStep(
    const unsigned char c, const u8 delim, const u8 quote, bool& inQ,
    CsvBlindState& st
)
{
  if ( c == quote ) {
    inQ = !inQ;
    st.sinceNl = true;
    return;
  }
  if ( !inQ && c == delim ) {
    ++st.cur;
    st.sinceNl = true;
    return;
  }
  if ( !inQ && c == '\n' ) {
    csvBlindClose( st );
    return;
  }
  st.sinceNl = true;
}

// Backslash-escaped variant of quotedStep for the tail: an escaped byte (and
// the escape byte itself) is literal content; the escape state carries.
inline void quotedStepEsc(
    const unsigned char c, const u8 delim, const u8 quote, const u8 esc,
    bool& inQ, bool& escaped, CsvBlindState& st
)
{
  if ( escaped ) {
    escaped = false;
    st.sinceNl = true;
    return;
  }
  if ( c == esc ) {
    escaped = true;
    st.sinceNl = true;
    return;
  }
  if ( c == quote ) {
    inQ = !inQ;
    st.sinceNl = true;
    return;
  }
  if ( !inQ && c == delim ) {
    ++st.cur;
    st.sinceNl = true;
    return;
  }
  if ( !inQ && c == '\n' ) {
    csvBlindClose( st );
    return;
  }
  st.sinceNl = true;
}

// The `escaped` bitmask for one 16-byte block: bit i set iff byte i is escaped
// (preceded by an odd-length run of `esc` bytes). simdjson's branchless
// odd-backslash-run algorithm, adapted to 16-bit blocks; `prevEscaped` (0/1)
// carries the run state across blocks and is seeded by the chunk's
// entryEscaped.
inline u32 findEscaped16( u32 backslash, u32& prevEscaped )
{
  if ( backslash == 0 ) {
    const u32 escaped = prevEscaped;
    prevEscaped = 0;
    return escaped;
  }
  const u32 kEven = 0x5555u;  // bits 0,2,4,...
  backslash &= ~prevEscaped;  // a backslash that is itself escaped is inert
  const u32 followsEscape = ( ( backslash << 1 ) | prevEscaped ) & 0xFFFFu;
  const u32 oddStarts = backslash & ~kEven & ~followsEscape & 0xFFFFu;
  const u32 sum =
      oddStarts + backslash;         // carry-propagate through runs (17 bits)
  prevEscaped = ( sum >> 16 ) & 1u;  // carry out of bit 15 -> next block
  const u32 invert = ( sum << 1 ) & 0xFFFFu;
  return ( kEven ^ invert ) & followsEscape & 0xFFFFu;
}

}  // namespace

void csvQuotedChunk(
    const char* buf, const usize len, const CsvDialect& d,
    const bool entryEscaped, CsvChunkSummary& h0, CsvChunkSummary& h1
)
{
  // quoting-off never reaches here (no dirty chunks); guard defensively.
  if ( !d.quoting ) {
    csvQuotedWalk( buf, len, d, /*entryInside=*/false, entryEscaped, h0 );
    csvQuotedWalk( buf, len, d, /*entryInside=*/true, entryEscaped, h1 );
    return;
  }

  const auto* p = reinterpret_cast<const u8*>( buf );
  const u8 delim = static_cast<u8>( d.delim );
  const u8 quote = static_cast<u8>( d.quote );
  const u8 esc = static_cast<u8>( d.esc );
  const bool hasEsc = d.backslashEsc;
  const uint8x16_t vq = vdupq_n_u8( quote );
  const uint8x16_t vd = vdupq_n_u8( delim );
  const uint8x16_t vn = vdupq_n_u8( static_cast<u8>( '\n' ) );
  const uint8x16_t ve = vdupq_n_u8( esc );

  CsvBlindState s0;     // entry outside a quote (h0)
  CsvBlindState s1;     // entry inside a quote (h1)
  bool parity = false;  // running entry-outside quote parity across blocks
  u32 prevEscaped = ( hasEsc && entryEscaped ) ? 1u : 0u;

  usize i = 0;
  for ( ; i + 16 <= len; i += 16 ) {
    const uint8x16_t v = vld1q_u8( p + i );
    u32 q = movemask16( vceqq_u8( v, vq ) );
    u32 dl = movemask16( vceqq_u8( v, vd ) );
    u32 nlm = movemask16( vceqq_u8( v, vn ) );
    if ( hasEsc ) {
      // Drop escaped quotes/delims/newlines: they are literal content, so they
      // neither toggle the quote state nor split records.
      const u32 bs = movemask16( vceqq_u8( v, ve ) );
      const u32 keep = ~findEscaped16( bs, prevEscaped ) & 0xFFFFu;
      q &= keep;
      dl &= keep;
      nlm &= keep;
    }
    const u32 px = prefixXor16( q );  // q is now the real quote toggles
    const u32 qOut =
        ( parity ? ~px : px ) & 0xFFFFu;  // in-quote, entry=outside
    // h0 reads outside (~qOut); h1 enters inside, so Q_in = ~Q_out and the real
    // bytes are exactly those qOut selects.
    segmentBits16( dl & ~qOut & 0xFFFFu, nlm & ~qOut & 0xFFFFu, s0 );
    segmentBits16( dl & qOut, nlm & qOut, s1 );
    parity ^= ( __builtin_popcount( q ) & 1 ) != 0;
  }

  bool inQ0 = parity;            // h0's quote state at the tail
  bool inQ1 = !parity;           // h1's
  bool esc0 = prevEscaped != 0;  // escape state at the tail (quote-independent)
  bool esc1 = esc0;
  for ( ; i < len; ++i ) {
    if ( hasEsc ) {
      quotedStepEsc( p[i], delim, quote, esc, inQ0, esc0, s0 );
      quotedStepEsc( p[i], delim, quote, esc, inQ1, esc1, s1 );
    } else {
      quotedStep( p[i], delim, quote, inQ0, s0 );
      quotedStep( p[i], delim, quote, inQ1, s1 );
    }
  }

  csvSummaryFinish( s0, h0 );
  h0.exitInside = inQ0;
  csvSummaryFinish( s1, h1 );
  h1.exitInside = inQ1;
}

#else  // !QWC_CSV_NEON_PHASE2

void csvQuotedChunk(
    const char* buf, const usize len, const CsvDialect& d,
    const bool entryEscaped, CsvChunkSummary& h0, CsvChunkSummary& h1
)
{
  csvQuotedWalk( buf, len, d, /*entryInside=*/false, entryEscaped, h0 );
  csvQuotedWalk( buf, len, d, /*entryInside=*/true, entryEscaped, h1 );
}

#endif  // QWC_CSV_NEON_PHASE2
