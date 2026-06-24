/*
 * Copyright Ji Krochmal 2026
 */
#pragma once
#include <algorithm>

#include "typedef.h"
#include "validatecsv.h"

// What a single chunk reports to the serial reconciliation, computed under a
// known/assumed entry quote-parity. A chunk's real newlines partition it into:
// a leading record (continuation of a record opened in an earlier chunk), zero
// or more interior records wholly inside the chunk, and a trailing open record.
struct CsvChunkSummary
{
  u64 leadingDelims = 0;            // real delims before the first real newline
  bool hasNewline = false;          // chunk has >= 1 real newline
  u64 interiorMinDelims = 0;        // min over interior complete records
  u64 interiorMaxDelims = 0;        // max over interior complete records
  u64 trailingDelims = 0;           // real delims after the last real newline
  bool trailingHasContent = false;  // any byte after the last real newline
  u64 realNewlines = 0;             // records terminated in this chunk
  bool exitInside = false;          // quote parity at chunk end
};

// Running record-segmentation state, shared by the scalar kernel, the NEON
// scalar tail, and the quote-aware walk. `cur` is the delimiter count of the
// open record (carried across blocks); the first real newline closes the
// leading record, later ones close interior records (min/max tracked).
struct CsvBlindState
{
  u64 cur = 0;
  u64 nlCount = 0;
  u64 leadingDelims = 0;
  u64 interiorMin = 0;
  u64 interiorMax = 0;
  bool haveInterior = false;
  bool sinceNl = false;  // any byte since the last newline
  bool sawQuote = false;
};

// Close the open record (its delimiter count is `st.cur`): the first real
// newline ends the leading record, later ones end interior records.
inline void csvBlindClose( CsvBlindState& st )
{
  if ( st.nlCount == 0 ) {
    st.leadingDelims = st.cur;
  } else if ( !st.haveInterior ) {
    st.interiorMin = st.interiorMax = st.cur;
    st.haveInterior = true;
  } else {
    st.interiorMin = std::min( st.interiorMin, st.cur );
    st.interiorMax = std::max( st.interiorMax, st.cur );
  }
  ++st.nlCount;
  st.cur = 0;
  st.sinceNl = false;
}

// One quote-blind byte: delimiters bump `cur`, '\n' closes a record, a quote
// byte is detected but treated as content (Phase 1).
inline void csvBlindByte(
    unsigned char c, const CsvDialect& d, CsvBlindState& st
)
{
  if ( d.quoting && c == static_cast<unsigned char>( d.quote ) )
    st.sawQuote = true;
  if ( c == static_cast<unsigned char>( d.delim ) ) {
    ++st.cur;
    st.sinceNl = true;
    return;
  }
  if ( c == '\n' ) {
    csvBlindClose( st );
    return;
  }
  st.sinceNl = true;
}

// Finalize the running state into a chunk summary (exitInside left false; the
// quote-aware walk sets it).
inline void csvSummaryFinish( const CsvBlindState& st, CsvChunkSummary& out )
{
  out = CsvChunkSummary{};
  out.realNewlines = st.nlCount;
  out.hasNewline = st.nlCount > 0;
  if ( st.nlCount == 0 )
    out.leadingDelims = st.cur;  // whole chunk is one open-record fragment
  else {
    out.leadingDelims = st.leadingDelims;
    out.trailingDelims = st.cur;
  }
  out.interiorMinDelims = st.interiorMin;
  out.interiorMaxDelims = st.interiorMax;
  out.trailingHasContent = st.sinceNl;
}

// Quote-aware segmentation of [buf, buf+len) given the entry quote-state and
// the entry backslash-escape state. Delimiters and newlines inside a quoted
// region are content; in backslash mode `esc` escapes the next byte everywhere;
// a doubled quote `""` cancels (two toggles). Shared by both ISAs (Phase 2's
// expensive part is the same regardless of how Phase 1's masks were built).
inline void csvQuotedWalk(
    const char* buf, const usize len, const CsvDialect& d,
    const bool entryInside, const bool entryEscaped, CsvChunkSummary& out
)
{
  CsvBlindState st;
  bool inQ = entryInside, esc = entryEscaped;
  const auto delim = static_cast<unsigned char>( d.delim );
  const auto quote = static_cast<unsigned char>( d.quote );
  const auto escb = static_cast<unsigned char>( d.esc );

  for ( usize i = 0; i < len; ++i ) {
    const auto c = static_cast<unsigned char>( buf[i] );
    if ( esc ) {
      esc = false;
      st.sinceNl = true;
      continue;
    }
    if ( d.quoting && d.backslashEsc && c == escb ) {
      esc = true;
      st.sinceNl = true;
      continue;
    }
    if ( d.quoting && c == quote ) {
      inQ = !inQ;
      st.sinceNl = true;
      continue;
    }
    if ( !inQ && c == delim ) {
      ++st.cur;
      st.sinceNl = true;
      continue;
    }
    if ( !inQ && c == '\n' ) {
      csvBlindClose( st );
      continue;
    }
    st.sinceNl = true;
  }
  csvSummaryFinish( st, out );
  out.exitInside = inQ;
}

// Phase 1: quote-blind. Treats every byte literally (no quote masking), fills
// `out` under the entry=outside assumption, and sets `sawQuote` if any quote
// byte is present in [buf, buf+len). For a chunk with no quote byte this is the
// real summary when entered outside a quoted region. Arch-dispatched.
void csvBlindChunk(
    const char* buf, usize len, const CsvDialect& d, CsvChunkSummary& out,
    bool& sawQuote
);

// Phase 2: quote-aware. Produces summaries under both entry hypotheses
// (h0 = entered outside a quote, h1 = entered inside). `entryEscaped` is the
// hypothesis-independent backslash-escape state of the chunk's first byte.
void csvQuotedChunk(
    const char* buf, usize len, const CsvDialect& d, bool entryEscaped,
    CsvChunkSummary& h0, CsvChunkSummary& h1
);
