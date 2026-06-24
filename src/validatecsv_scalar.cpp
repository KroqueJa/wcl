/*
 * Copyright Ji Krochmal 2026
 */
#include "validatecsv_kernel.h"

// Scalar per-chunk kernels for the parallel validate-csv driver. Both are plain
// linear walks over the shared steppers in validatecsv_kernel.h; the NEON
// kernels must agree with these on every input.

void csvBlindChunk(
    const char* buf, const usize len, const CsvDialect& d, CsvChunkSummary& out,
    bool& sawQuote
)
{
  CsvBlindState st;
  for ( usize i = 0; i < len; ++i )
    csvBlindByte( static_cast<unsigned char>( buf[i] ), d, st );
  csvSummaryFinish( st, out );
  sawQuote = st.sawQuote;
}

void csvQuotedChunk(
    const char* buf, const usize len, const CsvDialect& d,
    const bool entryEscaped, CsvChunkSummary& h0, CsvChunkSummary& h1
)
{
  csvQuotedWalk( buf, len, d, /*entryInside=*/false, entryEscaped, h0 );
  csvQuotedWalk( buf, len, d, /*entryInside=*/true, entryEscaped, h1 );
}
