/*
 * Copyright Ji Krochmal 2026
 */
#include <immintrin.h>

#include "validatecsv_kernel.h"

// AVX2 per-chunk kernels for the parallel validate-csv driver -- the x86_64
// counterpart of validatecsv_neon.cpp. csvBlindChunk (Phase 1) vectorizes the
// common no-newline 32-byte block (popcount delimiters, detect quotes) and
// segments newline-bearing blocks via the bit masks; the < 32-byte tail and
// the record bookkeeping reuse the shared steppers in validatecsv_kernel.h, so
// the AVX2 and scalar kernels agree on every input.
//
// csvQuotedChunk (Phase 2) is gated by QWC_CSV_AVX2_PHASE2 (CMake, default ON):
//   ON : the in-quote mask is built with a bit-per-byte movemask + prefix-XOR
//        (PCLMUL via _mm_clmulepi64_si128, unconditionally available at the
//        x86-64-v3 floor); both entry hypotheses share that one construction
//        (Q_in = ~Q_out). The backslash (--esc) dialect additionally clears
//        escaped quotes/delims/newlines via the simdjson odd-backslash-run
//        mask (findEscaped32) before the prefix-XOR, carried across blocks by
//        prevEscaped.
//   OFF: the whole of Phase 2 delegates to the shared scalar walk (the A/B
//        baseline for measuring whether the SIMD construction pays off).

namespace {

// AVX2 movemask: bit i set iff byte i of cmp is 0xFF. _mm256_movemask_epi8 is
// one uop and returns the bit-per-byte mask directly -- simpler than the NEON
// vshrn nibble mask used in validatecsv_neon.cpp.
inline u32 movemask32( const __m256i cmp )
{ return static_cast<u32>( _mm256_movemask_epi8( cmp ) ); }

inline u32 eqMask( const __m256i v, const u8 c )
{
  return movemask32(
      _mm256_cmpeq_epi8( v, _mm256_set1_epi8( static_cast<char>( c ) ) )
  );
}

// Bits for bytes [0, b): bit-per-byte version of validatecsv_neon.cpp's
// nibblesBelow / maskBelow16, widened to 32 bytes per block.
inline u32 maskBelow32( const u32 b )
{ return b >= 32 ? ~0u : ( ( 1u << b ) - 1u ); }

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
  for ( ; i + 32 <= len; i += 32 ) {
    const __m256i v =
        _mm256_loadu_si256( reinterpret_cast<const __m256i*>( p + i ) );
    if ( d.quoting && eqMask( v, quote ) != 0 ) st.sawQuote = true;
    const u32 nlMask = eqMask( v, static_cast<u8>( '\n' ) );
    const u32 delimMask = eqMask( v, delim );

    if ( nlMask == 0 ) {
      // No newline: the whole block extends the open record.
      st.cur += static_cast<u64>( __builtin_popcount( delimMask ) );
      st.sinceNl = true;
      continue;
    }

    // Newline-bearing block: close a record at each newline, counting the
    // delimiters in each inter-newline segment via popcount.
    u32 nn = nlMask;
    u32 prevByte = 0;
    while ( nn != 0 ) {
      const u32 b = static_cast<u32>( __builtin_ctz( nn ) );
      st.cur += static_cast<u64>( __builtin_popcount(
          delimMask & maskBelow32( b ) & ~maskBelow32( prevByte )
      ) );
      csvBlindClose( st );
      prevByte = b + 1;
      nn &= nn - 1;  // clear the lowest set bit
    }
    st.cur += static_cast<u64>(
        __builtin_popcount( delimMask & ~maskBelow32( prevByte ) )
    );
    st.sinceNl = prevByte < 32;
  }

  for ( ; i < len; ++i ) csvBlindByte( p[i], d, st );  // < 32-byte tail
  csvSummaryFinish( st, out );
  sawQuote = st.sawQuote;
}

#if QWC_CSV_AVX2_PHASE2

namespace {

// Inclusive prefix-XOR of the low 32 bits via PCLMUL: result bit i = XOR of
// input bits 0..i, i.e. the in-quote state at byte i for an entry-outside
// chunk. Carryless-multiply by the all-ones mask of bits 0..31 is the
// prefix-XOR (the simdjson idiom; the AVX2 counterpart of validatecsv_neon's
// vmull_p64). PCLMUL is part of x86-64-v2 -> unconditional at the v3 floor.
inline u32 prefixXor32( const u32 x )
{
  const __m128i xx =
      _mm_set_epi64x( 0, static_cast<long long>( static_cast<u64>( x ) ) );
  const __m128i ones = _mm_set_epi64x( 0, 0xFFFFFFFFLL );  // bits 0..31
  const __m128i prod = _mm_clmulepi64_si128( xx, ones, 0x00 );
  return static_cast<u32>( _mm_cvtsi128_si64( prod ) );
}

// Segment one 32-byte block given its real (outside-quote) delimiter and
// newline bit masks, threading the shared record state. Bit-per-byte clone of
// validatecsv_neon.cpp's segmentBits16, widened to 32 bytes.
inline void segmentBits32( const u32 realDelim, u32 realNl, CsvBlindState& st )
{
  if ( realNl == 0 ) {
    st.cur += static_cast<u64>( __builtin_popcount( realDelim ) );
    st.sinceNl = true;  // 32 bytes of content, no real newline
    return;
  }
  u32 prevByte = 0;
  while ( realNl != 0 ) {
    const u32 b = static_cast<u32>( __builtin_ctz( realNl ) );
    st.cur += static_cast<u64>( __builtin_popcount(
        realDelim & maskBelow32( b ) & ~maskBelow32( prevByte )
    ) );
    csvBlindClose( st );
    prevByte = b + 1;
    realNl &= realNl - 1;
  }
  st.cur += static_cast<u64>(
      __builtin_popcount( realDelim & ~maskBelow32( prevByte ) )
  );
  st.sinceNl = prevByte < 32;
}

// Per-byte quote-aware step for the < 32-byte tail (RFC-4180: no escapes).
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

// Backslash-escaped variant of quotedStep for the tail.
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

// The `escaped` bitmask for one 32-byte block: bit i set iff byte i is escaped
// (preceded by an odd-length run of `esc` bytes). simdjson's branchless
// odd-backslash-run algorithm, widened from validatecsv_neon's findEscaped16
// to 32 bits; the sum can carry into bit 32, so it lives in u64. `prevEscaped`
// (0/1) carries the run state across blocks and is seeded by the chunk's
// entryEscaped.
inline u32 findEscaped32( u32 backslash, u32& prevEscaped )
{
  if ( backslash == 0 ) {
    const u32 escaped = prevEscaped;
    prevEscaped = 0;
    return escaped;
  }
  const u32 kEven = 0x55555555u;  // bits 0,2,4,...,30
  backslash &= ~prevEscaped;      // a backslash that is itself escaped is inert
  const u32 followsEscape = ( backslash << 1 ) | prevEscaped;
  const u32 oddStarts = backslash & ~kEven & ~followsEscape;
  const u64 sum = static_cast<u64>( oddStarts ) + static_cast<u64>( backslash );
  prevEscaped = static_cast<u32>( ( sum >> 32 ) & 1u );  // carry -> next block
  const u32 invert = static_cast<u32>( sum << 1 );
  return ( kEven ^ invert ) & followsEscape;
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

  CsvBlindState s0;     // entry outside a quote (h0)
  CsvBlindState s1;     // entry inside a quote (h1)
  bool parity = false;  // running entry-outside quote parity across blocks
  u32 prevEscaped = ( hasEsc && entryEscaped ) ? 1u : 0u;

  usize i = 0;
  for ( ; i + 32 <= len; i += 32 ) {
    const __m256i v =
        _mm256_loadu_si256( reinterpret_cast<const __m256i*>( p + i ) );
    u32 q = eqMask( v, quote );
    u32 dl = eqMask( v, delim );
    u32 nlm = eqMask( v, static_cast<u8>( '\n' ) );
    if ( hasEsc ) {
      // Drop escaped quotes/delims/newlines: they are literal content, so they
      // neither toggle the quote state nor split records.
      const u32 bs = eqMask( v, esc );
      const u32 keep = ~findEscaped32( bs, prevEscaped );
      q &= keep;
      dl &= keep;
      nlm &= keep;
    }
    const u32 px = prefixXor32( q );     // q is now the real quote toggles
    const u32 qOut = parity ? ~px : px;  // in-quote, entry=outside
    // h0 reads outside (~qOut); h1 enters inside, so Q_in = ~Q_out and the real
    // bytes are exactly those qOut selects.
    segmentBits32( dl & ~qOut, nlm & ~qOut, s0 );
    segmentBits32( dl & qOut, nlm & qOut, s1 );
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

#else  // !QWC_CSV_AVX2_PHASE2

void csvQuotedChunk(
    const char* buf, const usize len, const CsvDialect& d,
    const bool entryEscaped, CsvChunkSummary& h0, CsvChunkSummary& h1
)
{
  csvQuotedWalk( buf, len, d, /*entryInside=*/false, entryEscaped, h0 );
  csvQuotedWalk( buf, len, d, /*entryInside=*/true, entryEscaped, h1 );
}

#endif  // QWC_CSV_AVX2_PHASE2
