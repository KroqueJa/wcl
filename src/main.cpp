/*
 * Copyright Ji Krochmal 2026
 */
#include <langinfo.h>

#include <algorithm>
#include <atomic>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

#include "cli.h"
#include "processfile.h"

// hardware_concurrency() runs a sysconf reading /sys/devices/system/cpu/online
// on Linux. Hide it behind a helper so the cost is only paid when a caller
// actually needs the answer, not unconditionally at program startup as a
// static initializer would. hardware_concurrency() can report 0 when it cannot
// tell, treated as 1 so the worker pool never collapses to zero.
inline u32 maxThreads()
{
  return std::thread::hardware_concurrency() > 0
             ? std::thread::hardware_concurrency()
             : 1u;
}
// Files per worker on the no-scan (bytes-only, `-c`) path: each file is a bare
// fstat (~microseconds on a warm local cache), so a worker only earns its
// ~tens-of-microseconds spawn cost once it has this many files to chew through.
// Below this the glob runs serially; above it we scale up to maxThreads(). A
// cold or networked open() costs far more and parallelizes sooner, so this
// warm-tuned default is deliberately conservative. Override with
// -DQWC_FILES_PER_THREAD=N.
#ifndef QWC_FILES_PER_THREAD
#define QWC_FILES_PER_THREAD 32
#endif

// Shared state for the file-fanout worker. A pointer-to-context is passed to
// each std::thread instead of a capturing lambda, keeping the thread entry a
// plain free function (lambdas in C++ are disliked here).
struct ScanContext
{
  std::atomic<usize>* nextFile;
  usize numFiles;
  const Options* opt;
  const Workload* work;
  Counts* output;
};

static void scanWorker( ScanContext* ctx )
{
  while ( true ) {
    const usize idx = ctx->nextFile->fetch_add( 1 );
    if ( idx >= ctx->numFiles ) return;
    ctx->output[idx] = processFile(
        ctx->opt->files[idx], *ctx->work, ctx->opt->bytesPerThread
    );
  }
}

// Map processFile over opt.files across the thread pool: a shared atomic
// cursor hands the next file to whichever worker is free, and results are
// stored at the matching index so the output order mirrors opt.files.
//
// GCC 13's -fanalyzer mis-models the libstdc++ vector constructor and reports a
// bogus "use of uninitialized value" for the value-initialized `output` below.
// The diagnostic carries no source location, so no #pragma region can catch
// it; it is muted for this TU via -Wno-analyzer-use-of-uninitialized-value in
// CMakeLists.txt (GCC only).
static std::vector<Counts> mapFiles(
    const Options& opt, const Workload& work, u32 numThreads
)
{
  const usize numFiles = opt.files.size();
  std::vector<Counts> output( numFiles );

  // One worker (or one file): run inline. Spawning a pool just to hand a single
  // thread the whole glob adds spawn+join latency for no parallelism. Results
  // are still stored by index, so output order matches the input order.
  if ( numThreads <= 1 || numFiles <= 1 ) {
    for ( usize i = 0; i < numFiles; ++i )
      output[i] = processFile( opt.files[i], work, opt.bytesPerThread );
    return output;
  }

  std::atomic<usize> nextFile = 0;
  ScanContext ctx{ &nextFile, numFiles, &opt, &work, output.data() };
  std::vector<std::thread> pool( numThreads );
  for ( auto& t: pool ) t = std::thread( scanWorker, &ctx );
  for ( auto& t: pool ) t.join();
  return output;
}

int main( int argc, char** argv )
{
  Options opt;

  // Pre-parse fast paths: bare `qwc` (stdin) and `qwc onefile` (a single
  // non-flag argument) both fall through parseArgs straight to the no-flag
  // default (lines + words + bytes). Setting those flags here and skipping
  // parseArgs tightens the setup path on the two most common shapes.
  // Anything starting with '-' (including bare '-', '--', and any flag)
  // takes the regular parseArgs path so the wc-style stdin convention and
  // every flag combination stay handled exactly as today.
  if ( argc == 1 ) {
    opt.lines = opt.words = opt.bytes = true;
  } else if ( argc == 2 && argv[1][0] != '-' ) {
    opt.lines = opt.words = opt.bytes = true;
    opt.files.push_back( argv[1] );
  } else if ( const std::optional<int> rc = parseArgs( argc, argv, opt ) ) {
    return *rc;
  }

  // Adopt the environment's locale once, before any worker threads exist.
  // setlocale(LC_CTYPE, "") performs the POSIX LC_ALL > LC_CTYPE > LANG
  // resolution -- the same environment handling as wc. The codeset decides
  // both the -m collapse (a character is just a byte in single-byte locales,
  // so the -m column displays the byte count) and the word-splitting flavour;
  // POSIXLY_CORRECT disables coreutils' non-breaking-space separators, exactly
  // like wc. None of this matters when the workload neither counts words nor
  // counts characters (the common `qwc -c` and `qwc -l` cases): skip the lot
  // and the cold libc locale pages never get faulted in.
  if ( opt.words || opt.chars ) {
    std::setlocale( LC_CTYPE, "" );  // NOLINT(concurrency-mt-unsafe)
    if ( opt.chars && MB_CUR_MAX <= 1 ) opt.charsAreBytes = true;
  }

  // Resolve the requested columns into a single counting workload, computed
  // once per file in a single pass.
  Workload work = opt.workload();
  // Only the words kernel consults utf8/nbspace; nl_langinfo and getenv stay
  // unread when -w is not requested, and WordsMode's defaults (utf8=false,
  // nbspace=true) are inert because work.words is false too.
  if ( opt.words ) {
    // Like setlocale above: called once at startup, before any worker threads
    // are spawned, so the thread-safety warnings do not apply here.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* codeset = nl_langinfo( CODESET );
    work.wordsMode.utf8 =
        codeset != nullptr && std::strcmp( codeset, "UTF-8" ) == 0;
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    work.wordsMode.nbspace = std::getenv( "POSIXLY_CORRECT" ) == nullptr;
  }

  // No file arguments: count standard input. wc prints just the padded
  // count(s), with no name.
  if ( opt.files.empty() ) {
    const Columns cols = selectedColumns( opt );
    const Counts c = processFile( "", work, opt.bytesPerThread );
    printCounts( opt, cols, c, nullptr );
    return 0;
  }

  if ( !collectFiles( opt ) ) return 1;
  const usize numFiles = opt.files.size();

  // Choose the worker count before spawning anything. A bytes-only workload is
  // a bare fstat per file, so it scales workers with the glob (one per
  // QWC_FILES_PER_THREAD files) and small globs stay serial. A scanning
  // workload is heavy per file, so it uses as many workers as files (capped)
  // and leans on each file's own internal chunk-parallelism for the rest.
  //
  // Both branches collapse to numThreads=1 below their respective thresholds
  // (scan: 1 file; no-scan: <2*QWC_FILES_PER_THREAD files, since the clamp
  // bottom is 1). In those ranges maxThreads() cannot change the answer, so
  // skip the sysconf -- the headline `qwc -c onefile` invocation calls it
  // zero times.
  u32 numThreads = 1;
  if ( work.needsScan() ) {
    if ( numFiles > 1 )
      numThreads =
          static_cast<u32>( std::min<usize>( numFiles, maxThreads() ) );
  } else if ( numFiles >= 2 * usize{ QWC_FILES_PER_THREAD } ) {
    numThreads = static_cast<u32>(
        std::clamp<usize>( numFiles / QWC_FILES_PER_THREAD, 1, maxThreads() )
    );
  }

  const std::vector<Counts> output = mapFiles( opt, work, numThreads );
  printResults( opt, output );
  return 0;
}
