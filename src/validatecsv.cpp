/*
 * Copyright Ji Krochmal 2026
 */
#include "validatecsv.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "validatecsv_kernel.h"

namespace {

// The reference sequential rectangularity check. A record completes at every
// unquoted '\n'; the first completed record's delimiter count is the reference
// ("expected") and every later record (plus a final record with no trailing
// newline) must match it. Inside a quoted region delimiters and newlines are
// content; in backslash mode `esc` escapes the next byte everywhere; a doubled
// quote `""` cancels to a zero-width region; '\r' is content (so CRLF
// validates); a quote still open at EOF is an error. The byte-level path behind
// validateCsvBuffer; collectBadRows mirrors it but enumerates every bad row.
CsvVerdict validateCsvSequential(
    const char* buf, const usize len, const CsvDialect& d
)
{
  bool inQuotes = false, escaped = false, recordHasContent = false;
  usize delims = 0, row = 0;
  bool haveExpected = false;
  usize expected = 0;
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
void streamFdToBuffer( i32 fd, std::vector<char>& out )
{
  char tmp[usize{ 64 } * 1024];
  isize n;
  while ( ( n = read( fd, tmp, sizeof( tmp ) ) ) > 0 )
    out.insert( out.end(), tmp, tmp + static_cast<usize>( n ) );
}

// Read [0, size) of a regular file into `out` via pread.
void readWholeRegular( i32 fd, usize size, std::vector<char>& out )
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
bool entryEscapedAt( i32 fd, usize start, const CsvDialect& d )
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
  i32 fd;
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
      const usize fileOffset = start + got;  // hoisted: clang-tidy flags a
                                             // widening cast of an arithmetic
                                             // expression, not of a variable
      const isize n = pread(
          ctx->fd, buf.data() + got, len - got, static_cast<off_t>( fileOffset )
      );
      if ( n <= 0 ) break;
      got += static_cast<usize>( n );
    }
    ChunkResult& r = ctx->out[i];
    if ( ctx->d->backslashEsc ) {
      // In backslash mode an escape can flip a delimiter/newline into content
      // anywhere -- including the chunk's first byte, escaped by a `\` carried
      // from the previous chunk -- so the quote-blind clean path is never safe.
      // Every chunk takes the quote-aware path with its resolved entry-escape.
      const bool entryEsc = entryEscapedAt( ctx->fd, start, *ctx->d );
      csvQuotedChunk( buf.data(), got, *ctx->d, entryEsc, r.h0, r.h1 );
      r.dirty = true;
      continue;
    }
    bool sawQuote = false;
    csvBlindChunk( buf.data(), got, *ctx->d, r.clean, sawQuote );
    if ( sawQuote ) {
      csvQuotedChunk(
          buf.data(), got, *ctx->d, /*entryEscaped=*/false, r.h0, r.h1
      );
      r.dirty = true;
    }
  }
}

// Serial seam pass over the per-chunk summaries: chain exit-parity -> next
// entry-parity from "outside", set `expected` from the first completed record,
// and check every record against it. Returns true iff the file is rectangular.
// The authoritative valid/invalid verdict for the parallel path; the exact bad
// rows (when invalid) are enumerated separately by collectBadRows.
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

// Parallel validity check for a regular file: run the two-phase workers and the
// seam reconciliation. Returns true iff the file is rectangular. The exact bad
// rows (when invalid) are enumerated separately by collectBadRows, so the
// common valid case stays the fast path -- no second read, no row enumeration.
bool validateCsvParallelValid(
    i32 fd, usize fileSize, const CsvDialect& d, usize bpt
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
  return reconcile( res.data(), nChunks );
}

// Maximum ragged rows listed by --all before truncating with a trailing "...".
// Bounds both the output line and the scan (collection stops once exceeded).
constexpr usize kAllRowsCap = 1000;

// Collect the 1-based row numbers of every ragged record (field count != the
// first record's), plus an unterminated-quote / ragged-final-record error at
// EOF. Same semantics as validateCsvSequential, but enumerating rather than
// returning the first; stops once more than `cap` are found and returns true
// (truncated). `bad` holds up to `cap` rows.
bool collectBadRows(
    const char* buf, usize len, const CsvDialect& d, usize cap,
    std::vector<usize>& bad
)
{
  // `buf` is a std::vector<char>'s data(); for an empty vector data() may be
  // null, in which case len is 0 and the loop below is a no-op anyway. The
  // explicit guard also makes the impossible (null, len>0) state unreachable
  // for GCC's -fanalyzer, which otherwise mis-models the vector that backs
  // `buf` (it thinks .data() can be null while .size() > 0) and flags `buf[i]`.
  if ( buf == nullptr ) return false;
  bool inQuotes = false, escaped = false, recordHasContent = false;
  usize delims = 0, row = 0;
  bool haveExpected = false;
  usize expected = 0;
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
        if ( bad.size() == cap ) return true;
        bad.push_back( row + 1 );
      }
      ++row;
      delims = 0;
      recordHasContent = false;
      continue;
    }
    recordHasContent = true;
  }
  // At EOF the final record is bad if a quote is still open (unterminated) or
  // it is a ragged final record with no trailing newline -- either way, one
  // row.
  if ( inQuotes ||
       ( recordHasContent && haveExpected && delims != expected ) ) {
    if ( bad.size() == cap ) return true;
    bad.push_back( row + 1 );
  }
  return false;
}

// Classified input handle. `fd == 0` with `!owned` is standard input.
struct InputInfo
{
  i32 fd;
  bool owned;       // we opened it (must close)
  usize size;       // st_size (regular files)
  bool regular;     // S_ISREG && size > 0
  bool bigRegular;  // regular && size > bytesPerThread (worth chunking)
};

InputInfo openInput( const char* filename, usize bpt )
{
  if ( filename[0] == '\0' ) return { 0, false, 0, false, false };  // stdin
  const i32 fd = open( filename, O_RDONLY );
  if ( fd < 0 ) {
    std::fprintf( stderr, "Error opening file: %s\n", filename );
    std::_Exit( 1 );
  }
  struct stat st{};
  if ( fstat( fd, &st ) < 0 ) {
    std::fprintf( stderr, "Error stating file: %s\n", filename );
    std::_Exit( 1 );
  }
  const usize size = static_cast<usize>( st.st_size );
  const bool reg = S_ISREG( st.st_mode ) && size > 0;
  return { fd, true, size, reg, reg && size > bpt };
}

void closeInput( const InputInfo& in )
{
  if ( in.owned ) close( in.fd );
}

// Read the whole input into `buf`: stdin / non-regular streamed to EOF, a
// regular file read by size with pread.
void readInputBuffer( const InputInfo& in, std::vector<char>& buf )
{
  if ( !in.owned )
    streamFdToBuffer( 0, buf );
  else if ( !in.regular )
    streamFdToBuffer( in.fd, buf );
  else
    readWholeRegular( in.fd, in.size, buf );
}

// Enumerate ragged rows in `buf` and print the First/All report to stdout.
// Returns the exit code (0 when the buffer turns out valid -- reachable only on
// the stdin/small path, where validity is not pre-checked).
i32 reportRows(
    const char* name, CsvMode mode, const std::vector<char>& buf,
    const CsvDialect& d
)
{
  const usize cap = mode == CsvMode::First ? usize{ 1 } : kAllRowsCap;
  std::vector<usize> bad;
  const bool truncated = collectBadRows( buf.data(), buf.size(), d, cap, bad );
  if ( bad.empty() ) return 0;
  // Emit "<name>: r1,r2,...\n" straight to the (buffered) stream -- this is the
  // failure path, so a few fputc/fprintf per number costs nothing, and it keeps
  // the TU free of <string> (matching qwc's lean static-link posture). fputs on
  // the name avoids treating it as a format string.
  std::fputs( name, stdout );
  std::fputs( ": ", stdout );
  for ( usize i = 0; i < bad.size(); ++i ) {
    if ( i ) std::fputc( ',', stdout );
    std::fprintf( stdout, "%zu", bad[i] );  // usize == size_t
  }
  // Only --all signals "there were more"; --first inherently lists just one.
  if ( truncated && mode == CsvMode::All ) std::fputs( ",...", stdout );
  std::fputc( '\n', stdout );
  return 1;
}

}  // namespace

CsvVerdict validateCsvBuffer(
    const char* buf, const usize len, const CsvDialect& d
)
{
  return validateCsvSequential( buf, len, d );
}

CsvVerdict validateCsvFile(
    const char* filename, const CsvDialect& d, usize bytesPerThread
)
{
  const InputInfo in = openInput( filename, bytesPerThread );
  if ( in.bigRegular ) {
    if ( validateCsvParallelValid( in.fd, in.size, d, bytesPerThread ) ) {
      closeInput( in );
      return { true, 0 };
    }
    std::vector<char> buf;  // invalid: read to pinpoint the first bad row
    readWholeRegular( in.fd, in.size, buf );
    closeInput( in );
    std::vector<usize> bad;
    collectBadRows( buf.data(), buf.size(), d, 1, bad );
    return { false, bad.empty() ? usize{ 0 } : bad[0] };
  }
  std::vector<char> buf;
  readInputBuffer( in, buf );
  closeInput( in );
  std::vector<usize> bad;
  collectBadRows( buf.data(), buf.size(), d, 1, bad );
  if ( bad.empty() ) return { true, 0 };
  return { false, bad[0] };
}

i32 validateCsv(
    const char* filename, const CsvDialect& d, CsvMode mode,
    usize bytesPerThread
)
{
  const char* name = filename[0] != '\0' ? filename : "-";
  const InputInfo in = openInput( filename, bytesPerThread );

  // Big regular file: parallel validity check first (the fast path), reading
  // the whole file only when a failure must be enumerated (First/All).
  if ( in.bigRegular ) {
    const bool valid =
        validateCsvParallelValid( in.fd, in.size, d, bytesPerThread );
    if ( valid ) {
      closeInput( in );
      return 0;
    }
    if ( mode == CsvMode::Fast ) {
      closeInput( in );
      return 1;
    }
    if ( mode == CsvMode::List ) {
      closeInput( in );
      std::printf( "%s\n", name );
      return 1;
    }
    std::vector<char> buf;
    readWholeRegular( in.fd, in.size, buf );
    closeInput( in );
    return reportRows( name, mode, buf, d );
  }

  // stdin / non-regular / small regular: one buffered sequential pass.
  std::vector<char> buf;
  readInputBuffer( in, buf );
  closeInput( in );
  if ( mode == CsvMode::Fast || mode == CsvMode::List ) {
    std::vector<usize> bad;
    collectBadRows( buf.data(), buf.size(), d, 1, bad );
    if ( bad.empty() ) return 0;
    if ( mode == CsvMode::List ) std::printf( "%s\n", name );
    return 1;
  }
  return reportRows( name, mode, buf, d );
}

i32 validateCsvFiles(
    const std::vector<const char*>& files, const CsvDialect& d, CsvMode mode,
    usize bytesPerThread
)
{
  if ( files.empty() )  // no file arguments: validate standard input
    return validateCsv( "", d, mode, bytesPerThread );

  // Validate each file in argument order, each with its own internal
  // chunk-parallelism; validateCsv prints that file's report (nothing when it
  // is valid). Exit 1 if any file fails; --fast bails on the first failure.
  i32 rc = 0;
  for ( const char* f: files ) {
    if ( validateCsv( f, d, mode, bytesPerThread ) != 0 ) {
      rc = 1;
      if ( mode == CsvMode::Fast ) return 1;
    }
  }
  return rc;
}
