/*
 * Copyright Ji Krochmal 2026
 */
#include "processfile.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "chars.h"
#include "countlines.h"
#include "maxlinelen.h"
#include "words.h"

// hardware_concurrency() runs a sysconf reading /sys/devices/system/cpu/online
// on Linux; cache it in a function-local static so the cost is paid once on
// first call and never on the bytes-only paths that bail before line ~263.
// hardware_concurrency() can report 0 when it cannot tell, treated as 1 so the
// thread count never collapses to zero.
static u32 maxThreads()
{
  static const u32 n = std::thread::hardware_concurrency() > 0
                           ? std::thread::hardware_concurrency()
                           : 1u;
  return n;
}

namespace {

// Per-stream/per-chunk running state for every scanned counter. `wordScan` and
// `line` carry across the successive read buffers of one byte range; their
// leading/trailing-run facts let neighbouring chunks be stitched.
struct ScanState
{
  usize lines = 0;
  usize chars = 0;
  usize target = 0;
  WordScan wordScan;  // word-counting carries + seam facts for this range
  LineScan line;      // longest-line carry within this range
};

// Run every requested counter over the owned region [ownedBegin, ownedEnd) of
// one buffer, threading the carries in `s`. Bytes outside the owned region are
// context for the word scanner only (multibyte separator windows may peek up
// to WCTX bytes either side); every other counter sees just the owned bytes,
// so nothing is double-counted. Each counter scans the (cache-resident) buffer
// independently; the data is read from the file only once, by the caller.
inline void scanBuffer(
    const char* buf, const usize len, const usize ownedBegin,
    const usize ownedEnd, const Workload& w, ScanState& s
)
{
  const char* owned = buf + ownedBegin;
  const usize g = ownedEnd - ownedBegin;
  if ( w.lines ) s.lines += count( owned, g, '\n' );
  if ( w.target ) s.target += count( owned, g, w.targetByte );
  if ( w.words )
    words( buf, len, ownedBegin, ownedEnd, s.wordScan, w.wordsMode );

  // chars and the longest line interact: `wc -L -m` measures the longest line
  // in characters, and that count and the character total walk the same UTF-8
  // continuation bytes. When both are wanted, one fused pass produces both;
  // otherwise each is computed on its own. (w.maxLineInChars implies w.chars --
  // both are set exactly when -m is active in a multibyte locale -- so the
  // fused path covers the char total.)
  if ( w.maxLine && w.maxLineInChars ) {
    maxLineLenChars( owned, g, s.line, s.chars );
  } else {
    if ( w.chars ) s.chars += chars( owned, g );
    if ( w.maxLine ) maxLineLen( owned, g, s.line, w.maxLineInChars );
  }
}

// Scan-buffer geometry for the regular-file path. Each scan fetches up to
// BUF_SIZE owned bytes plus up to WCTX context bytes on both sides, so a
// multibyte separator window straddling a chunk seam is visible whole.
// BUF_SIZE was tuned by the 2026-06-13 sweep -- see Finding 6 in
// benchmarks/README.md for the LLC-miss data behind the choice (1 MiB spilled
// L2 on every host we measured; 256 KiB stays L2-resident).
constexpr usize BUF_SIZE = usize{ 256 } << 10;  // 256 KiB
constexpr usize WCTX = 3;  // multibyte window context per side

// The scan buffer is per worker thread and reused across every file that
// thread processes. Small files dominate some workloads (thousands of opens
// per second), so a fresh allocation per file -- let alone a
// value-initialized one -- would cost more than the scan itself.
char* threadBuffer()
{
  static thread_local std::vector<char> buf( BUF_SIZE + 2 * WCTX );
  return buf.data();
}

// Stream the byte range [start, start+size) of fd through `buffer` with
// pread(), running every requested counter and threading the carries into
// `out`. pread() is thread-safe (the offset is per-call, not shared via fd), so
// one fd serves concurrent callers. The whole workload is computed on each
// buffer before moving on -- one read, every requested counter.
void scanRange(
    i32 fd, const usize start, const usize size, const usize fileSize,
    const Workload* work, ScanState* out, char* buffer
)
{
  ScanState s;
  usize remaining = size;
  usize pos = start;
  while ( remaining > 0 ) {
    // Fetch up to WCTX context bytes on both sides of the owned slice so
    // a multibyte separator window straddling the seam is visible whole.
    const usize want = std::min( BUF_SIZE, remaining );
    const usize front = std::min( WCTX, pos );
    const usize back = std::min( WCTX, fileSize - ( pos + want ) );
    const isize got = pread(
        fd, buffer, front + want + back, static_cast<off_t>( pos - front )
    );
    if ( got <= static_cast<isize>( front ) ) break;
    const usize ownedEnd = std::min( static_cast<usize>( got ), front + want );
    scanBuffer( buffer, static_cast<usize>( got ), front, ownedEnd, *work, s );
    remaining -= ownedEnd - front;
    pos += ownedEnd - front;
  }
  *out = s;
}

// std::thread entry: like scanRange, with the spawned thread's own reusable
// buffer. (Plain function rather than a lambda, and pointers rather than
// references, so std::thread's by-value argument copying stays trivial.)
void scanRangeThread(
    const i32 fd, const usize start, const usize size, const usize fileSize,
    const Workload* work, ScanState* out
)
{ scanRange( fd, start, size, fileSize, work, out, threadBuffer() ); }

// Serial single-pass scan of a stream that can only be consumed once and whose
// length fstat cannot give us up front: standard input, and any non-regular
// file (FIFO, character/block device, socket, /proc, ...). Bytes are tallied as
// we read, since there is no reliable st_size. This is the boring,
// always-correct path -- no threads, no pread, no size assumptions.
Counts processStream( i32 fd, const Workload& w )
{
  static constexpr usize WCTX = 3;  // multibyte window context per side
  thread_local char buffer[usize{ 128 } * 4096 + 2 * WCTX];
  ScanState s;
  Counts c{};
  // The buffer front holds up to WCTX already-scanned bytes (context for a
  // multibyte window opening the next read) followed by up to WCTX withheld
  // unscanned bytes (a window the next read may complete); the fresh read
  // lands after them.
  usize ctx = 0;      // already-scanned context bytes at the buffer front
  usize pending = 0;  // withheld unscanned bytes after the context
  isize bytesRead;
  while ( ( bytesRead = read(
                fd, buffer + ctx + pending, sizeof( buffer ) - ctx - pending
            ) ) > 0 ) {
    const auto fresh = static_cast<usize>( bytesRead );
    if ( w.bytes ) c.bytes += fresh;  // only fresh bytes, never re-counted
    const usize len = ctx + pending + fresh;
    // Withhold the final <= WCTX bytes: they may begin a window the next read
    // completes. EOF flushes them below.
    const usize hold = std::min( pending + fresh, WCTX );
    const usize ownedEnd = len - hold;
    scanBuffer( buffer, len, ctx, ownedEnd, w, s );
    // Slide the tail down: the last <= WCTX scanned bytes become the new
    // context, the withheld bytes follow them.
    const usize keep = std::min( ownedEnd, WCTX );
    std::memmove( buffer, buffer + ownedEnd - keep, keep + hold );
    ctx = keep;
    pending = hold;
  }
  // EOF: scan the withheld bytes (their window can no longer grow).
  if ( pending > 0 )
    scanBuffer( buffer, ctx + pending, ctx, ctx + pending, w, s );
  c.lines = s.lines;
  wordsFlush( s.wordScan );  // EOF terminates the stream's trailing word
  c.words = s.wordScan.words;
  c.chars = s.chars;
  c.target = s.target;
  // wc -L ignores a trailing line with no final newline, so drop the open run.
  if ( w.maxLine ) c.maxLine = s.line.maxComplete;
  return c;
}

}  // namespace

Counts processFile(
    const char* filename, const Workload& work, const usize bytesPerThread
)
{
  if ( filename[0] == '\0' ) return processStream( 0, work );

  // processFile runs on worker threads (see mapFiles), so these fatal I/O-error
  // paths use _Exit: it terminates immediately without running atexit handlers
  // or static destructors, which would otherwise race with the live workers
  // (stderr is unbuffered so the message is already out, and no counts have
  // been printed yet).
  i32 fd = open( filename, O_RDONLY );
  if ( fd < 0 ) {
    std::fprintf( stderr, "Error opening file: %s\n", filename );
    std::_Exit( 1 );
  }

  struct stat st{};
  if ( fstat( fd, &st ) < 0 ) {
    std::fprintf( stderr, "Error stating file: %s\n", filename );
    std::_Exit( 1 );
  }

  const usize fileSize = st.st_size;

  // Take the parallel fast path only for a regular file with a trustworthy,
  // nonzero size. Everything else goes through the serial stream scan:
  //   * non-regular inputs (FIFO, device, socket) -- no size, and pread can't
  //     seek them; and
  //   * regular files whose fstat size is 0 -- either a genuinely empty file,
  //   or
  //     a procfs/sysfs file that lies about its length and actually has
  //     content.
  // We cannot chunk what we cannot size, and must not assume 0 means empty, so
  // such inputs are simply read to EOF: correct on everything, while the common
  // case (a normal file with a real size) keeps the threaded fast path.
  if ( !S_ISREG( st.st_mode ) || fileSize == 0 ) {
    const Counts c = processStream( fd, work );
    close( fd );
    return c;
  }

  Counts result{};
  if ( work.bytes ) result.bytes = fileSize;  // bytes come straight from fstat

  // Nothing to scan (e.g. a bare `-c`): the fstat byte count is all we need.
  if ( !work.needsScan() ) {
    close( fd );
    return result;
  }

  // We will read every byte sequentially, so ask the kernel to start pulling
  // the whole file into the page cache up front. Only worth the syscall(s) for
  // a file bigger than one scan buffer: anything smaller is consumed by the
  // very first read, which the kernel's default readahead already covers, and
  // on a many-small-files run the advice itself becomes measurable overhead.
  if ( fileSize > BUF_SIZE ) {
#if defined( __APPLE__ )
    // On macOS F_RDADVISE is the most effective hint (MADV_WILLNEED is largely
    // a no-op). radvisory::ra_count is an int, so issue the advice in <=INT_MAX
    // chunks.
    for ( usize off = 0; off < fileSize; ) {
      usize remaining = fileSize - off;
      radvisory ra{};
      ra.ra_offset = static_cast<i64>( off );
      ra.ra_count = static_cast<int>(
          std::min( remaining, static_cast<usize>( INT_MAX ) )
      );
      fcntl( fd, F_RDADVISE, &ra );
      off += static_cast<usize>( ra.ra_count );
    }
#elif defined( POSIX_FADV_SEQUENTIAL )
    // On Linux posix_fadvise gives the kernel both the access pattern and an
    // explicit readahead hint over the whole range.
    posix_fadvise(
        fd, 0, static_cast<off_t>( fileSize ), POSIX_FADV_SEQUENTIAL
    );
    posix_fadvise( fd, 0, static_cast<off_t>( fileSize ), POSIX_FADV_WILLNEED );
#endif
  }

  u32 numThreads = static_cast<u32>( std::min(
      static_cast<usize>( maxThreads() ),
      ( fileSize + bytesPerThread - 1 ) / bytesPerThread
  ) );
  numThreads = std::max( numThreads, 1u );

  // One chunk: scan inline on the calling thread, into its reused buffer. This
  // is every file below bytesPerThread -- on a many-small-files run, all of
  // them -- and spawning a std::thread per file just to join it immediately
  // costs more (two context switches plus a stack/TLS setup) than scanning a
  // small file does. `single` keeps the lone result on the stack so this path
  // allocates nothing at all.
  // A dedicated lone--l fast path (skipping scanBuffer's flag dispatch, the
  // seam context and the merge) was prototyped here and benchmarked at exactly
  // 1.00x against this generic path on the many-small-files corpus: with the
  // scan inlined onto the calling thread and the buffer reused (below), the
  // generic machinery costs a few predictable branches per MiB-sized buffer,
  // which is unmeasurable. Recorded in benchmarks/README.md; don't re-add it.
  ScanState single;
  std::vector<ScanState> results;
  const ScanState* res = &single;
  if ( numThreads == 1 ) {
    scanRange( fd, 0, fileSize, fileSize, &work, &single, threadBuffer() );
  } else {
    // Multiple chunks: one thread per byte range, each streaming through its
    // own per-thread buffer rather than faulting an mmap page-by-page; the
    // kernel bulk-copies straight from the warmed page cache.
    results.resize( numThreads );
    res = results.data();
    const usize chunkSize = fileSize / numThreads;
    std::vector<std::thread> threads;
    threads.reserve( numThreads );
    for ( u32 i = 0; i < numThreads; ++i ) {
      const usize start = i * chunkSize;
      const usize size =
          ( i == numThreads - 1 ) ? fileSize - i * chunkSize : chunkSize;
      threads.emplace_back(
          scanRangeThread, fd, start, size, fileSize, &work, &results[i]
      );
    }
    for ( auto& t: threads ) t.join();
  }
  close( fd );

  // Lines, chars and target just sum. Words are counted at run END inside each
  // chunk, so a run straddling a seam was counted only by the chunk where it
  // ends -- no dupe to subtract. What the merge must add is judgement over the
  // seam: a run's barren-ness spans its whole extent, so the carry threads
  // inWord+hasPrintable forward and (a) rescues a run the ending chunk saw as
  // barren but an earlier chunk satisfied, (b) counts a run that ended exactly
  // at a seam (which neither side counted). The run still open after the last
  // chunk is the file's trailing word -- EOF terminates it.
  bool carryInWord = false, carryHasP = false;
  for ( u32 i = 0; i < numThreads; ++i ) {
    const ScanState& s = res[i];
    result.lines += s.lines;
    result.chars += s.chars;
    result.target += s.target;
    const WordScan& wsc = s.wordScan;
    result.words += wsc.words;
    if ( !wsc.sawByte ) continue;  // empty chunk: carry passes through
    if ( carryInWord ) {
      if ( wsc.startsInWord ) {
        if ( wsc.leadingEnded && !wsc.leadingHasPrintable && carryHasP )
          ++result.words;  // rescue: the earlier part had the printable
      } else if ( carryHasP ) {
        ++result.words;  // run ended exactly at the seam
      }
    }
    if ( carryInWord && wsc.startsInWord && !wsc.sawSeparator ) {
      carryHasP = carryHasP || wsc.runHasPrintable;  // one run spans the chunk
    } else {
      carryInWord = wsc.inWord;
      carryHasP = wsc.runHasPrintable;
    }
  }
  if ( carryInWord && carryHasP ) ++result.words;

  // Longest line: a line split across a boundary is the previous chunk's open
  // run (`carry`) plus this chunk's prefix (bytes up to its first newline), but
  // only a newline realizes it as a counted line. The run still open after the
  // last chunk has no terminating newline, so -- like wc -L -- it is dropped.
  usize maxLen = 0;
  usize lineCarry = 0;
  for ( u32 i = 0; i < numThreads; ++i ) {
    const ScanState& s = res[i];
    maxLen = std::max( maxLen, s.line.maxComplete );
    if ( s.line.hasNewline ) {
      maxLen = std::max( maxLen, lineCarry + s.line.prefixLen );
      lineCarry = s.line.cur;
    } else {
      lineCarry += s.line.cur;
    }
  }
  result.maxLine = maxLen;

  return result;
}
