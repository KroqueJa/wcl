/*
 * Copyright Ji Krochmal 2026
 */
#include "validatecsv.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "validatecsv_kernel.h"

namespace {

// Carried state for the sequential validator, so it can be seeded for the
// failure inspection re-scan (and a fresh-zeroed instance is the from-scratch
// validateCsvBuffer). See validateCsvSeeded for the semantics.
struct CsvSeed
{
  bool inQuotes = false;
  bool escaped = false;
  bool recordHasContent = false;
  usize delims = 0;
  usize row = 0;
  bool haveExpected = false;
  usize expected = 0;
};

// The one true sequential rectangularity check, parameterized by an entry seed.
// A record completes at every unquoted '\n'; the first completed record's
// delimiter count is the reference ("expected") and every later record (plus a
// final record with no trailing newline) must match it. Inside a quoted region
// delimiters and newlines are content; in backslash mode `esc` escapes the next
// byte everywhere; a doubled quote `""` cancels to a zero-width region; '\r' is
// content (so CRLF validates); a quote still open at EOF is an error.
CsvVerdict validateCsvSeeded(
    const char* buf, const usize len, const CsvDialect& d, CsvSeed s
)
{
  bool inQuotes = s.inQuotes, escaped = s.escaped;
  bool recordHasContent = s.recordHasContent;
  usize delims = s.delims, row = s.row;
  bool haveExpected = s.haveExpected;
  usize expected = s.expected;
  const auto delim = static_cast<unsigned char>( d.delim );
  const auto quote = static_cast<unsigned char>( d.quote );
  const auto esc = static_cast<unsigned char>( d.esc );

  for ( usize i = 0; i < len; ++i ) {
    const auto c = static_cast<unsigned char>( buf[i] );
    if ( escaped ) {
      escaped = false;
      recordHasContent = true;
      continue;
    }
    if ( d.quoting && d.backslashEsc && c == esc ) {
      escaped = true;
      recordHasContent = true;
      continue;
    }
    if ( d.quoting && c == quote ) {
      inQuotes = !inQuotes;
      recordHasContent = true;
      continue;
    }
    if ( !inQuotes && c == delim ) {
      ++delims;
      recordHasContent = true;
      continue;
    }
    if ( !inQuotes && c == '\n' ) {
      if ( !haveExpected ) {
        haveExpected = true;
        expected = delims;
      } else if ( delims != expected ) {
        return { false, row + 1 };
      }
      ++row;
      delims = 0;
      recordHasContent = false;
      continue;
    }
    recordHasContent = true;
  }

  if ( inQuotes ) return { false, row + 1 };
  if ( recordHasContent && haveExpected && delims != expected )
    return { false, row + 1 };
  return { true, 0 };
}

// hardware_concurrency() can report 0 when it cannot tell; treat as 1.
u32 maxThreads()
{
  static const u32 n = std::thread::hardware_concurrency() > 0
                           ? std::thread::hardware_concurrency()
                           : 1u;
  return n;
}

// Stream an fd to EOF into `out` (standard input, FIFOs, devices, zero-size
// regular files -- anything fstat cannot size).
void streamFdToBuffer( int fd, std::vector<char>& out )
{
  char tmp[usize{ 64 } * 1024];
  isize n;
  while ( ( n = read( fd, tmp, sizeof( tmp ) ) ) > 0 )
    out.insert( out.end(), tmp, tmp + static_cast<usize>( n ) );
}

// Read [0, size) of a regular file into `out` via pread.
void readWholeRegular( int fd, usize size, std::vector<char>& out )
{
  out.resize( size );
  usize got = 0;
  while ( got < size ) {
    const isize n =
        pread( fd, out.data() + got, size - got, static_cast<off_t>( got ) );
    if ( n <= 0 ) break;
    got += static_cast<usize>( n );
  }
  out.resize( got );
}

// The hypothesis-independent backslash-escape state of the byte at `start`:
// true iff an odd-length run of `esc` bytes immediately precedes it. Bounded by
// the run length (~O(1) on real input); only relevant in backslash mode.
bool entryEscapedAt( int fd, usize start, const CsvDialect& d )
{
  if ( !d.quoting || !d.backslashEsc || start == 0 ) return false;
  const auto esc = static_cast<unsigned char>( d.esc );
  char tmp[256];
  usize runStart = start;
  while ( runStart > 0 ) {
    const usize want = std::min( sizeof( tmp ), runStart );
    const isize got =
        pread( fd, tmp, want, static_cast<off_t>( runStart - want ) );
    if ( got <= 0 ) break;
    usize k = static_cast<usize>( got );
    while ( k > 0 && static_cast<unsigned char>( tmp[k - 1] ) == esc ) --k;
    runStart -= ( static_cast<usize>( got ) - k );
    if ( k > 0 ) break;  // hit a non-esc byte: run ends here
  }
  return ( ( start - runStart ) & 1u ) != 0;
}

// One chunk's contribution to the reconciliation. For a clean (no quote byte)
// chunk only `clean` is filled; for a dirty chunk both quote-parity hypotheses
// (h0 = entered outside, h1 = entered inside) are filled.
struct ChunkResult
{
  bool dirty = false;
  CsvChunkSummary clean;
  CsvChunkSummary h0;
  CsvChunkSummary h1;
};

// Shared worker context (pointer-passed to a plain free function, per project
// convention -- no capturing lambda). Chunks are pulled off a shared atomic
// cursor so chunk size is exactly `bpt`.
struct CsvWorkerCtx
{
  int fd;
  usize fileSize;
  usize bpt;
  usize nChunks;
  const CsvDialect* d;
  ChunkResult* out;
  std::atomic<usize>* next;
};

void csvWorker( CsvWorkerCtx* ctx )
{
  std::vector<char> buf;  // reused across the chunks this worker claims
  while ( true ) {
    const usize i = ctx->next->fetch_add( 1 );
    if ( i >= ctx->nChunks ) return;
    const usize start = i * ctx->bpt;
    const usize end = std::min( start + ctx->bpt, ctx->fileSize );
    const usize len = end - start;
    buf.resize( len );
    usize got = 0;
    while ( got < len ) {
      const isize n = pread(
          ctx->fd, buf.data() + got, len - got,
          static_cast<off_t>( start + got )
      );
      if ( n <= 0 ) break;
      got += static_cast<usize>( n );
    }
    ChunkResult& r = ctx->out[i];
    bool sawQuote = false;
    csvBlindChunk( buf.data(), got, *ctx->d, r.clean, sawQuote );
    if ( sawQuote ) {
      const bool entryEsc = entryEscapedAt( ctx->fd, start, *ctx->d );
      csvQuotedChunk( buf.data(), got, *ctx->d, entryEsc, r.h0, r.h1 );
      r.dirty = true;
    }
  }
}

// Serial seam pass over the per-chunk summaries: chain exit-parity -> next
// entry-parity from "outside", set `expected` from the first completed record,
// and check every record against it. Returns true iff the file is rectangular.
// The authoritative valid/invalid verdict for the parallel path; the exact bad
// row (when invalid) is found by the inspection re-scan.
bool reconcile( const ChunkResult* res, usize nChunks )
{
  bool inside = false, haveExpected = false, openContent = false;
  usize expected = 0, carryDelims = 0;

  for ( usize i = 0; i < nChunks; ++i ) {
    CsvChunkSummary s;
    if ( !res[i].dirty ) {
      if ( !inside ) {
        s = res[i].clean;
      } else {
        // A clean chunk entered inside a quote is one big quoted blob: no real
        // records, parity unchanged, content present (chunks are non-empty).
        s = CsvChunkSummary{};
        s.exitInside = true;
        s.trailingHasContent = true;
      }
    } else {
      s = inside ? res[i].h1 : res[i].h0;
    }

    if ( s.hasNewline ) {
      const usize firstRow = carryDelims + s.leadingDelims;
      if ( !haveExpected ) {
        haveExpected = true;
        expected = firstRow;  // record 1 defines the reference
      } else if ( firstRow != expected ) {
        return false;
      }
      if ( s.realNewlines >= 2 &&
           ( s.interiorMinDelims != s.interiorMaxDelims ||
             s.interiorMinDelims != expected ) )
        return false;
      carryDelims = s.trailingDelims;
      openContent = s.trailingHasContent;
    } else {
      carryDelims += s.leadingDelims;
      openContent = openContent || s.trailingHasContent;
    }
    inside = s.exitInside;
  }

  if ( inside ) return false;  // unterminated quote at EOF
  if ( openContent && haveExpected && carryDelims != expected )
    return false;  // final record with no trailing newline is ragged
  return true;
}

// Parallel two-phase validation of a regular file. `inspect` controls whether,
// on a detected violation, the file is re-scanned sequentially to pinpoint the
// exact bad row (false for --fast: caller exits immediately).
CsvVerdict validateCsvRegularParallel(
    int fd, usize fileSize, const CsvDialect& d, usize bpt, bool inspect
)
{
  const usize nChunks = ( fileSize + bpt - 1 ) / bpt;
  std::vector<ChunkResult> res( nChunks );
  std::atomic<usize> next{ 0 };
  CsvWorkerCtx ctx{ fd, fileSize, bpt, nChunks, &d, res.data(), &next };

  const u32 nWorkers =
      static_cast<u32>( std::min<usize>( nChunks, maxThreads() ) );
  if ( nWorkers <= 1 ) {
    csvWorker( &ctx );
  } else {
    std::vector<std::thread> pool;
    pool.reserve( nWorkers );
    for ( u32 t = 0; t < nWorkers; ++t ) pool.emplace_back( csvWorker, &ctx );
    for ( auto& th: pool ) th.join();
  }

  if ( reconcile( res.data(), nChunks ) ) return { true, 0 };
  if ( !inspect ) return { false, 0 };

  // Failure inspection (non-fast): the sequential reference over the whole file
  // gives the exact first bad row. This is also a cross-check on reconcile --
  // a disagreement surfaces as a parity-test failure.
  std::vector<char> whole;
  readWholeRegular( fd, fileSize, whole );
  const CsvVerdict v = validateCsvSeeded( whole.data(), whole.size(), d, {} );
  return { false, v.badRow };
}

// Open the file, classify it, and validate. `inspect` is threaded down to the
// parallel path; the sequential paths always know the exact row anyway.
CsvVerdict runValidate(
    const char* filename, const CsvDialect& d, usize bytesPerThread,
    bool inspect
)
{
  if ( filename[0] == '\0' ) {  // standard input
    std::vector<char> buf;
    streamFdToBuffer( 0, buf );
    return validateCsvSeeded( buf.data(), buf.size(), d, {} );
  }

  const int fd = open( filename, O_RDONLY );
  if ( fd < 0 ) {
    std::fprintf( stderr, "Error opening file: %s\n", filename );
    std::_Exit( 1 );
  }
  struct stat st{};
  if ( fstat( fd, &st ) < 0 ) {
    std::fprintf( stderr, "Error stating file: %s\n", filename );
    std::_Exit( 1 );
  }
  const usize fileSize = static_cast<usize>( st.st_size );

  CsvVerdict v;
  if ( !S_ISREG( st.st_mode ) || fileSize == 0 ) {
    std::vector<char> buf;
    streamFdToBuffer( fd, buf );  // non-regular or untrustworthy size
    v = validateCsvSeeded( buf.data(), buf.size(), d, {} );
  } else if ( fileSize <= bytesPerThread ) {
    std::vector<char> buf;
    readWholeRegular( fd, fileSize, buf );  // single chunk: just go sequential
    v = validateCsvSeeded( buf.data(), buf.size(), d, {} );
  } else {
    v = validateCsvRegularParallel( fd, fileSize, d, bytesPerThread, inspect );
  }
  close( fd );
  return v;
}

}  // namespace

CsvVerdict validateCsvBuffer(
    const char* buf, const usize len, const CsvDialect& d
)
{
  return validateCsvSeeded( buf, len, d, {} );
}

CsvVerdict validateCsvFile(
    const char* filename, const CsvDialect& d, usize bytesPerThread
)
{
  return runValidate( filename, d, bytesPerThread, /*inspect=*/true );
}

int validateCsv(
    const char* filename, const CsvDialect& d, bool fast, usize bytesPerThread
)
{
  const CsvVerdict v =
      runValidate( filename, d, bytesPerThread, /*inspect=*/!fast );
  if ( v.valid ) return 0;
  if ( fast ) std::_Exit( 1 );  // skip the diagnostic, exit on first mismatch
  std::fprintf(
      stderr, "bad row: %ju\n", static_cast<std::uintmax_t>( v.badRow )
  );
  return 1;
}
