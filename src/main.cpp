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
#include "validatecsv.h"

// The C++ runtime overrides below are Linux-only AND Release-only.
//
// Linux-only: they assume the qwc build statically links libstdc++ + libgcc
// and depend on the static-archive link semantics (an object file is
// contributed only when it satisfies an unresolved reference). Apple
// targets get libc++ + libunwind from the dyld shared cache instead, so
// the same symbols are resolved at runtime against system libraries and
// overriding them in our binary would either be inert or actively wrong.
//
// Release-only: Debug builds keep the full libstdc++/libgcc machinery so
// debuggers, sanitizers, the verbose terminate message ("terminate called
// after throwing an instance of 'X' / what(): Y") and crash-time stack
// unwinding work normally during development. The size cost is irrelevant
// when iterating; the ~110 KB cut is only paid in Release.
//
// The matching `-fno-exceptions` / `-fno-rtti` /
// `-fno-asynchronous-unwind-tables` / `-fno-unwind-tables` flags in
// CMakeLists.txt are gated on the same conditions
// (`NOT APPLE AND Release|RelWithDebInfo`).
#if !defined( __APPLE__ ) && defined( NDEBUG )

// Each override stub exits with a distinct code so that if any of these
// invisible paths ever fires in production the exit status names the leaf
// (no stderr message; the visible OOM / I/O failures in processfile.cpp /
// cli.cpp deliberately still exit 1 to keep wc parity). Codes start at 64
// to stay clear of common shell-meaningful codes (0 success, 1 generic
// error, 2 misuse, 126 noexec, 127 not-found, 128+signal).
enum class ExitPoint : i32
{
  VerboseTerminate = 64,
  PersonalityV0 = 65,
  UnwindResume = 66,
  UnwindRaiseException = 67,
  UnwindResumeOrRethrow = 68,
  UnwindForcedUnwind = 69,
  UnwindBacktrace = 70,
  UnwindDeleteException = 71,
  UnwindFindEnclosingFunction = 72,
  UnwindGetCFA = 73,
  UnwindGetDataRelBase = 74,
  UnwindGetGR = 75,
  UnwindGetIP = 76,
  UnwindGetIPInfo = 77,
  UnwindGetLanguageSpecificData = 78,
  UnwindGetRegionStart = 79,
  UnwindGetTextRelBase = 80,
  UnwindSetGR = 81,
  UnwindSetIP = 82,
  FrameStateFor = 83,
};
[[noreturn]] static void exitFrom( ExitPoint p )
{
  std::_Exit( static_cast<i32>( p ) );
}

// Override libstdc++'s default std::terminate handler. The stock one
// (__verbose_terminate_handler in libsupc++/vterminate.cc) formats "terminate
// called after throwing an instance of 'X' / what(): Y" by demangling the
// typeid name -- dragging d_print_comp_inner and the rest of the C++ demangler
// (~24 KB of text) into the static image even though qwc is built with
// -fno-exceptions and never throws. The libstdc++.a archive only pulls
// vterminate.o if this symbol is unresolved when the archive is scanned;
// defining it here (main.cpp.o is on the link line before -lstdc++) means
// the linker resolves the reference against qwc and leaves the demangler
// unreachable, so --gc-sections can free it. A no-throw qwc still needs
// SOMETHING here in case of std::bad_alloc on a vector grow -- exitFrom
// matches the contract qwc already gives on fatal errors.
namespace __gnu_cxx {
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
void __verbose_terminate_handler()
{
  exitFrom( ExitPoint::VerboseTerminate );
}
}  // namespace __gnu_cxx

// Override the C++ exception personality routine: libstdc++.a's
// eh_personality.o is no longer pulled into the link.
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" i32 __gxx_personality_v0( i32, i32, u64, void*, void* )
{
  exitFrom( ExitPoint::PersonalityV0 );
}

// Override every public symbol that libgcc's unwind-dw2.o defines. libstdc++
// TUs reference these (thread.o, eh_throw.o, stdexcept.o, ...; traced with
// `ld -y`) from cleanup landing pads even though qwc never throws; if any
// one of them stays unresolved, the linker pulls unwind-dw2.o and the whole
// machinery (uw_frame_state_for, execute_cfa_program_*, FDE-lookup btree,
// the public-API wrappers) comes with it. Resolving them all here against
// stubs means unwind-dw2.o stays unpulled. Each stub aborts because qwc
// never throws and -fno-exceptions turned the implicit bad_alloc edges into
// std::terminate(); any reach into these is a last-resort path.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" void _Unwind_Resume( void* )
{
  exitFrom( ExitPoint::UnwindResume );
}
extern "C" i32 _Unwind_RaiseException( void* )
{
  exitFrom( ExitPoint::UnwindRaiseException );
}
extern "C" i32 _Unwind_Resume_or_Rethrow( void* )
{
  exitFrom( ExitPoint::UnwindResumeOrRethrow );
}
extern "C" i32 _Unwind_ForcedUnwind( void*, void*, void* )
{
  exitFrom( ExitPoint::UnwindForcedUnwind );
}
extern "C" i32 _Unwind_Backtrace( void*, void* )
{
  exitFrom( ExitPoint::UnwindBacktrace );
}
extern "C" void _Unwind_DeleteException( void* )
{
  exitFrom( ExitPoint::UnwindDeleteException );
}
extern "C" void* _Unwind_FindEnclosingFunction( void* )
{
  exitFrom( ExitPoint::UnwindFindEnclosingFunction );
}
extern "C" u64 _Unwind_GetCFA( void* )
{
  exitFrom( ExitPoint::UnwindGetCFA );
}
extern "C" u64 _Unwind_GetDataRelBase( void* )
{
  exitFrom( ExitPoint::UnwindGetDataRelBase );
}
extern "C" u64 _Unwind_GetGR( void*, i32 )
{
  exitFrom( ExitPoint::UnwindGetGR );
}
extern "C" u64 _Unwind_GetIP( void* )
{
  exitFrom( ExitPoint::UnwindGetIP );
}
extern "C" u64 _Unwind_GetIPInfo( void*, i32* )
{
  exitFrom( ExitPoint::UnwindGetIPInfo );
}
extern "C" u64 _Unwind_GetLanguageSpecificData( void* )
{
  exitFrom( ExitPoint::UnwindGetLanguageSpecificData );
}
extern "C" u64 _Unwind_GetRegionStart( void* )
{
  exitFrom( ExitPoint::UnwindGetRegionStart );
}
extern "C" u64 _Unwind_GetTextRelBase( void* )
{
  exitFrom( ExitPoint::UnwindGetTextRelBase );
}
extern "C" void _Unwind_SetGR( void*, i32, u64 )
{
  exitFrom( ExitPoint::UnwindSetGR );
}
extern "C" void _Unwind_SetIP( void*, u64 )
{
  exitFrom( ExitPoint::UnwindSetIP );
}
extern "C" void __frame_state_for( void*, void* )
{
  exitFrom( ExitPoint::FrameStateFor );
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

#endif  // !__APPLE__ && NDEBUG

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

  // One worker: run inline. Spawning a pool just to hand a single thread the
  // whole glob adds spawn+join latency for no parallelism. The numFiles==1
  // case is handled by main's single-file short-circuit before mapFiles is
  // ever called, so this branch is the small-no-scan-glob path only. Results
  // are stored by index, so output order matches the input order.
  if ( numThreads <= 1 ) {
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

i32 main( i32 argc, char** argv )
{
  // validate-csv is a mode, not a counting column, so it leads and routes to
  // its own parser + driver before the counting path. One strcmp that
  // short-circuits on the first byte for the common `qwc onefile` case, so the
  // bare-file fast path below stays untouched.
  if ( argc >= 2 && std::strcmp( argv[1], "--validate-csv" ) == 0 ) {
    CsvDialect dialect;
    CsvMode mode = CsvMode::All;
    std::vector<const char*> files;
    if ( const std::optional<i32> rc =
             parseValidateCsvArgs( argc, argv, dialect, mode, files ) )
      return *rc;
    return validateCsvFiles( files, dialect, mode );
  }

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
  } else if ( const std::optional<i32> rc = parseArgs( argc, argv, opt ) ) {
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

  // Single-file short-circuit: skip mapFiles' one-element vector allocation
  // AND printResults' total / iota / per-row / sort cascade (all no-ops on
  // one file anyway). Sort, --top and --reverse on a single file are
  // identity operations, so collapsing them is semantics-preserving.
  if ( numFiles == 1 ) {
    const Columns cols = selectedColumns( opt );
    const Counts c = processFile( opt.files[0], work, opt.bytesPerThread );
    printCounts( opt, cols, c, opt.files[0] );
    return 0;
  }

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
