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

// Defined below (after the parallel machinery); enumerateFlagged calls it.
bool collectBadRowsSeeded(
    const char* buf, usize len, const CsvDialect& d, bool entryInside,
    bool entryEscaped, usize carryDelims, usize expected, bool haveExpected,
    usize firstRow, bool atEof, usize cap, std::vector<usize>& bad
);

// Pick the summary a chunk presents to reconciliation given the entry quote
// parity: a clean chunk uses its naive summary when entered outside, or a
// synthesized "one big quoted blob" when entered inside (no real records, parity
// unchanged, content present since chunks are non-empty); a dirty chunk uses the
// matching hypothesis.
CsvChunkSummary selectSummary( const ChunkResult& r, bool inside )
{
  if ( !r.dirty ) {
    if ( !inside ) return r.clean;
    CsvChunkSummary s{};
    s.exitInside = true;
    s.trailingHasContent = true;
    return s;
  }
  return inside ? r.h1 : r.h0;
}

// What a flagged (violation-bearing) chunk needs to be enumerated independently:
// its byte offset (chunkIndex * bpt) and the resolved seam state at its start.
struct CsvBadChunk
{
  usize chunkIndex;    // byte offset = chunkIndex * bpt
  bool inside;         // resolved entry quote parity
  usize carryDelims;   // open-record delims carried in (for the leading record)
  bool haveExpected;   // was `expected` already set at this chunk's start?
  usize expected;      // global reference field-delim count (0 if not yet set)
  usize firstRow;      // 1-based row of this chunk's first completing record
};

// Serial seam pass: chain quote parity, set `expected` from the first completed
// record, and FLAG every chunk that contains a record whose field count differs
// from `expected` (leading, interior, or the EOF/unterminated-quote record),
// recording each flagged chunk's resolved entry state. Returns true iff the file
// is rectangular (no chunk flagged). Unlike a plain validity check it does not
// early-return: it must find ALL bad chunks. The valid case flags nothing, so
// the fast path is unchanged.
bool reconcileAndFlag(
    const ChunkResult* res, usize nChunks, std::vector<CsvBadChunk>& flagged
)
{
  bool inside = false, haveExpected = false, openContent = false;
  usize expected = 0, carryDelims = 0, recordsSoFar = 0;
  CsvBadChunk lastSeed{};  // entry seed of the final chunk (for the EOF record)

  for ( usize i = 0; i < nChunks; ++i ) {
    const CsvBadChunk seed{ i,          inside,   carryDelims,
                            haveExpected, expected, recordsSoFar + 1 };
    lastSeed = seed;
    const CsvChunkSummary s = selectSummary( res[i], inside );
    bool chunkBad = false;

    if ( s.hasNewline ) {
      const usize leadRow = carryDelims + s.leadingDelims;
      if ( !haveExpected ) {
        haveExpected = true;
        expected = leadRow;  // record 1 defines the reference
      } else if ( leadRow != expected ) {
        chunkBad = true;
      }
      if ( s.realNewlines >= 2 &&
           ( s.interiorMinDelims != s.interiorMaxDelims ||
             s.interiorMinDelims != expected ) )
        chunkBad = true;
      carryDelims = s.trailingDelims;
      openContent = s.trailingHasContent;
    } else {
      carryDelims += s.leadingDelims;
      openContent = openContent || s.trailingHasContent;
    }
    inside = s.exitInside;
    recordsSoFar += s.realNewlines;
    if ( chunkBad ) flagged.push_back( seed );
  }

  // The EOF record (final record with no trailing newline, or an unterminated
  // quote) lives in the last chunk; flag it if bad and not already flagged. The
  // seeded walk on the last chunk reads to EOF and re-detects it.
  const bool eofBad =
      inside || ( openContent && haveExpected && carryDelims != expected );
  if ( eofBad && nChunks > 0 &&
       ( flagged.empty() || flagged.back().chunkIndex != nChunks - 1 ) )
    flagged.push_back( lastSeed );

  return flagged.empty();
}

// Launch the two-phase workers over `res` (one ChunkResult per chunk). Threading
// economy mirrors the counting path: one worker per chunk up to maxThreads().
void runValidityWorkers(
    i32 fd, usize fileSize, const CsvDialect& d, usize bpt,
    std::vector<ChunkResult>& res
)
{
  const usize nChunks = res.size();
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
}

// Validity-only verdict for a regular file (the Fast/List path and
// validateCsvFile): run the two-phase workers and the seam reconciliation,
// discarding the flags. The common valid case stays the fast path -- no second
// read, no row enumeration.
bool validateCsvParallelValid(
    i32 fd, usize fileSize, const CsvDialect& d, usize bpt
)
{
  const usize nChunks = ( fileSize + bpt - 1 ) / bpt;
  std::vector<ChunkResult> res( nChunks );
  runValidityWorkers( fd, fileSize, d, bpt, res );
  std::vector<CsvBadChunk> flagged;
  return reconcileAndFlag( res.data(), nChunks, flagged );
}

// Read [start, start+len) of a regular file into `buf` via pread; returns the
// byte count actually read (the final chunk is short). Mirrors csvWorker's read.
usize preadChunk( i32 fd, std::vector<char>& buf, usize start, usize len )
{
  buf.resize( len );
  usize got = 0;
  while ( got < len ) {
    const usize off = start + got;
    const isize n =
        pread( fd, buf.data() + got, len - got, static_cast<off_t>( off ) );
    if ( n <= 0 ) break;
    got += static_cast<usize>( n );
  }
  return got;
}

// Shared context for the enumeration workers: each claims a flagged chunk off
// the atomic cursor and writes its result into the disjoint slot `perChunk[k]`
// (so no synchronization is needed beyond the cursor).
struct EnumCtx
{
  i32 fd;
  usize fileSize;
  const std::vector<CsvBadChunk>* flagged;
  const CsvDialect* d;
  usize bpt;
  usize cap;
  std::vector<usize>* perChunk;  // [flagged->size()] result lists
  unsigned char* more;           // [flagged->size()] "this chunk exceeded cap"
  std::atomic<usize>* next;
};

void enumWorker( EnumCtx* ctx )
{
  std::vector<char> buf;  // reused across the chunks this worker claims
  while ( true ) {
    const usize k = ctx->next->fetch_add( 1 );
    if ( k >= ctx->flagged->size() ) return;
    const CsvBadChunk& fc = ( *ctx->flagged )[k];
    const usize start = fc.chunkIndex * ctx->bpt;
    const usize len = std::min( ctx->bpt, ctx->fileSize - start );
    const bool atEof = start + len == ctx->fileSize;
    const usize got = preadChunk( ctx->fd, buf, start, len );
    const bool entryEsc = entryEscapedAt( ctx->fd, start, *ctx->d );
    ctx->more[k] = collectBadRowsSeeded(
                       buf.data(), got, *ctx->d, fc.inside, entryEsc,
                       fc.carryDelims, fc.expected, fc.haveExpected, fc.firstRow,
                       atEof, ctx->cap, ctx->perChunk[k]
                   )
                       ? 1
                       : 0;
  }
}

// Enumerate up to `cap` ragged rows by re-walking ONLY the flagged chunks, each
// seeded with its resolved entry state, in parallel. `flagged` is ascending by
// chunkIndex and each chunk owns a disjoint ascending row range, so the serial
// merge concatenates the per-chunk lists in flag order under the global cap --
// no sort, and the output is byte-identical to the whole-file serial scan.
CsvBadRows enumerateFlagged(
    i32 fd, usize fileSize, const std::vector<CsvBadChunk>& flagged,
    const CsvDialect& d, usize bpt, usize cap
)
{
  const usize n = flagged.size();
  std::vector<std::vector<usize>> perChunk( n );
  std::vector<unsigned char> more( n, 0 );
  std::atomic<usize> next{ 0 };
  EnumCtx ctx{ fd,  fileSize,        &flagged,    &d,        bpt,
               cap, perChunk.data(), more.data(), &next };

  const u32 nWorkers = static_cast<u32>( std::min<usize>( n, maxThreads() ) );
  if ( nWorkers <= 1 ) {
    enumWorker( &ctx );
  } else {
    std::vector<std::thread> pool;
    pool.reserve( nWorkers );
    for ( u32 t = 0; t < nWorkers; ++t ) pool.emplace_back( enumWorker, &ctx );
    for ( auto& th: pool ) th.join();
  }

  CsvBadRows br;
  for ( usize k = 0; k < n; ++k ) {
    for ( const usize r: perChunk[k] ) {
      if ( br.rows.size() >= cap ) {
        br.truncated = true;
        return br;
      }
      br.rows.push_back( r );
    }
    if ( more[k] ) br.truncated = true;
  }
  return br;
}

// Maximum ragged rows listed by --all before truncating with a trailing "...".
// Bounds both the output line and the scan (collection stops once exceeded).
constexpr usize kAllRowsCap = 1000;

// Seeded enumeration of ragged rows in [buf, buf+len). Starts from the resolved
// entry state (`entryInside` quote parity, `entryEscaped` backslash state,
// `carryDelims` open-record delimiters carried from prior chunks). When
// `haveExpected` is true the global reference field-count is supplied (the
// per-chunk enumeration path); when false it is derived from the first completed
// record (the whole-buffer / stdin path). The i-th completing record is numbered
// `firstRow + i`. `atEof` is true only when the buffer's end is the real end of
// file: only then is a trailing record (no final '\n') or an unterminated quote
// judged. For a non-final chunk the trailing partial record continues into the
// next chunk and must NOT be judged here -- it is judged when it completes there
// (as that chunk's leading record). Stops once more than `cap` bad rows are
// found and returns true (truncated). Mirrors validateCsvSequential.
bool collectBadRowsSeeded(
    const char* buf, usize len, const CsvDialect& d, bool entryInside,
    bool entryEscaped, usize carryDelims, usize expected, bool haveExpected,
    usize firstRow, bool atEof, usize cap, std::vector<usize>& bad
)
{
  // `buf` is a std::vector<char>'s data(); for an empty vector data() may be
  // null, in which case len is 0 and the loop below is a no-op anyway. The
  // explicit guard also makes the impossible (null, len>0) state unreachable
  // for GCC's -fanalyzer, which otherwise mis-models the vector that backs
  // `buf` (it thinks .data() can be null while .size() > 0) and flags `buf[i]`.
  if ( buf == nullptr ) return false;
  bool inQuotes = entryInside, escaped = entryEscaped, recordHasContent = false;
  usize delims = carryDelims, row = firstRow - 1;  // first completion -> firstRow
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
  // Only at the real EOF: the final record is bad if a quote is still open
  // (unterminated) or it is a ragged final record with no trailing newline.
  if ( atEof &&
       ( inQuotes ||
         ( recordHasContent && haveExpected && delims != expected ) ) ) {
    if ( bad.size() == cap ) return true;
    bad.push_back( row + 1 );
  }
  return false;
}

// Whole-buffer enumeration from a clean start (the stdin / small-file path):
// derive `expected` from the first record, row 1; the buffer end is the EOF.
bool collectBadRows(
    const char* buf, usize len, const CsvDialect& d, usize cap,
    std::vector<usize>& bad
)
{
  return collectBadRowsSeeded(
      buf, len, d, /*entryInside=*/false, /*entryEscaped=*/false,
      /*carryDelims=*/0, /*expected=*/0, /*haveExpected=*/false, /*firstRow=*/1,
      /*atEof=*/true, cap, bad
  );
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

// Print the First/All report from an already-collected bad-row list. Returns
// the exit code (1 if any rows, else 0).
i32 printBadRows( const char* name, CsvMode mode, const CsvBadRows& br )
{
  if ( br.rows.empty() ) return 0;
  // Emit "<name>: r1,r2,...\n" straight to the (buffered) stream -- this is the
  // failure path, so a few fputc/fprintf per number costs nothing, and it keeps
  // the TU free of <string> (matching qwc's lean static-link posture). fputs on
  // the name avoids treating it as a format string.
  std::fputs( name, stdout );
  std::fputs( ": ", stdout );
  for ( usize i = 0; i < br.rows.size(); ++i ) {
    if ( i ) std::fputc( ',', stdout );
    std::fprintf( stdout, "%zu", br.rows[i] );  // usize == size_t
  }
  // Only --all signals "there were more"; --first inherently lists just one.
  if ( br.truncated && mode == CsvMode::All ) std::fputs( ",...", stdout );
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

CsvBadRows validateCsvBadRows(
    const char* filename, const CsvDialect& d, usize cap, usize bytesPerThread
)
{
  const InputInfo in = openInput( filename, bytesPerThread );
  CsvBadRows br;
  if ( in.bigRegular ) {
    // Validity pass first; on failure, re-walk only the flagged chunks (no
    // whole-file second read). The valid case returns here untouched.
    const usize nChunks = ( in.size + bytesPerThread - 1 ) / bytesPerThread;
    std::vector<ChunkResult> res( nChunks );
    runValidityWorkers( in.fd, in.size, d, bytesPerThread, res );
    std::vector<CsvBadChunk> flagged;
    if ( reconcileAndFlag( res.data(), nChunks, flagged ) ) {
      closeInput( in );
      return br;  // valid -> empty
    }
    br = enumerateFlagged( in.fd, in.size, flagged, d, bytesPerThread, cap );
    closeInput( in );
    return br;
  }
  // stdin / non-regular / small regular: one buffered sequential pass.
  std::vector<char> buf;
  readInputBuffer( in, buf );
  closeInput( in );
  br.truncated = collectBadRows( buf.data(), buf.size(), d, cap, br.rows );
  return br;
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
    closeInput( in );  // invalid: enumerate just the first bad row
    const CsvBadRows br =
        validateCsvBadRows( filename, d, /*cap=*/1, bytesPerThread );
    return { false, br.rows.empty() ? usize{ 0 } : br.rows[0] };
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

  // Big regular file: run the two-phase workers once; the seam pass gives the
  // verdict and (when invalid) the flagged chunks, which First/All enumerate
  // directly -- no whole-file second read.
  if ( in.bigRegular ) {
    const usize nChunks = ( in.size + bytesPerThread - 1 ) / bytesPerThread;
    std::vector<ChunkResult> res( nChunks );
    runValidityWorkers( in.fd, in.size, d, bytesPerThread, res );
    std::vector<CsvBadChunk> flagged;
    if ( reconcileAndFlag( res.data(), nChunks, flagged ) ) {
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
    const usize cap = mode == CsvMode::First ? usize{ 1 } : kAllRowsCap;
    const CsvBadRows br =
        enumerateFlagged( in.fd, in.size, flagged, d, bytesPerThread, cap );
    closeInput( in );
    return printBadRows( name, mode, br );
  }

  // stdin / non-regular / small regular: one buffered sequential pass.
  std::vector<char> buf;
  readInputBuffer( in, buf );
  closeInput( in );
  const usize cap = mode == CsvMode::First ? usize{ 1 } : kAllRowsCap;
  CsvBadRows br;
  br.truncated = collectBadRows( buf.data(), buf.size(), d, cap, br.rows );
  if ( br.rows.empty() ) return 0;
  if ( mode == CsvMode::Fast ) return 1;
  if ( mode == CsvMode::List ) { std::printf( "%s\n", name ); return 1; }
  return printBadRows( name, mode, br );
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
