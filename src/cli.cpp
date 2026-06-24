/*
 * Copyright Ji Krochmal 2026
 */
#include "cli.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <vector>

#include "qwc_version.h"
#include "validatecsv.h"

void printHelp()
{
  std::fputs(
      "qwc - quick wc: count lines, words, bytes and more, fast.\n"
      "\n"
      "With no count flag qwc prints the line, word and byte counts of each "
      "file,\n"
      "exactly like `wc`. It reads big files in parallel, so it stays quick "
      "even\n"
      "on very large inputs.\n"
      "\n"
      "Usage:\n"
      "  qwc [options] [file ...]\n"
      "  qwc [options]            (reads from standard input)\n"
      "\n"
      "If you don't name any files, qwc reads from standard input - so you "
      "can\n"
      "pipe data straight into it, e.g.  cat access.log | qwc\n"
      "\n"
      "Count flags (combine freely, e.g. -lw or -l -w; their order never "
      "changes\n"
      "the column order: lines, words, chars, bytes, longest line. With "
      "none, qwc\n"
      "prints lines, words and bytes like bare `wc`. -c and -m together "
      "print both\n"
      "columns, chars first, exactly like GNU wc):\n"
      "  -l, --lines           Count lines (newline characters), like `wc "
      "-l`.\n"
      "  -w, --words           Count whitespace-separated words, like `wc "
      "-w`.\n"
      "  -c, --bytes           Count bytes, like `wc -c`. The size is read\n"
      "                        straight from the filesystem, so no scanning "
      "of\n"
      "                        the file contents is needed.\n"
      "  -m, --chars           Count characters, like `wc -m`. In a UTF-8 "
      "locale\n"
      "                        this counts code points (so an accented "
      "letter is\n"
      "                        one character, not two bytes); in a "
      "single-byte\n"
      "                        locale it falls back to bytes, exactly as wc "
      "does.\n"
      "  -L, --max-line-length Print the length of the longest line in "
      "bytes,\n"
      "                        like `wc -L`. The trailing newline is not "
      "counted,\n"
      "                        and with several files the \"total\" line "
      "reports\n"
      "                        the longest line across all of them (the "
      "maximum,\n"
      "                        not a sum).\n"
      "      --char C          Count occurrences of the byte C (a qwc "
      "extension,\n"
      "                        not in wc). Handy for tallying commas in a "
      "CSV\n"
      "                        (--char ,) or any other single character.\n"
      "\n"
      "Other options:\n"
      "  -r, --recursive       Treat directory arguments as whole trees: "
      "qwc\n"
      "                        walks into them and counts every file it "
      "finds,\n"
      "                        something wc can't do on its own.\n"
      "      --sort-by-count   When listing several files, order them by "
      "their\n"
      "                        count, smallest first - so the biggest ones "
      "end\n"
      "                        up at the bottom, right next to the grand "
      "total\n"
      "                        where they're easy to spot.\n"
      "      --sort-by-name    Order the listing alphabetically by file "
      "name.\n"
      "      --sort-by-size    Order the listing by file size on disk, "
      "smallest\n"
      "                        first.\n"
      "      --reverse         Flip the active sort order (e.g. put the "
      "biggest\n"
      "                        counts at the top instead). Without a sort "
      "flag\n"
      "                        or --top there is no order to flip, so it "
      "does\n"
      "                        nothing.\n"
      "      --top N           Show only the N files that rank highest by "
      "the\n"
      "                        active sort (counts, if none is given). The "
      "grand\n"
      "                        total still covers every file, not just the "
      "N\n"
      "                        shown.\n"
      "      --bytes-per-thread N\n"
      "                        Tune how much data each worker thread "
      "handles\n"
      "                        before another thread is spun up (default "
      "64\n"
      "                        MiB). You rarely need this; it's here for\n"
      "                        squeezing out performance on unusual "
      "hardware.\n"
      "      --version         Print the qwc version and exit. Release\n"
      "                        builds show the release tag (e.g. v1.2.0);\n"
      "                        dev builds show the git commit they were\n"
      "                        built from.\n"
      "  -h, --help            Show this message and exit.\n"
      "\n"
      "Modes (not combinable with the count flags above):\n"
      "      --validate-csv    Prove a CSV file is rectangular (every record "
      "has the\n"
      "                        same field count), faster than a full parse. "
      "Quoted\n"
      "                        content is masked, so embedded delimiters and\n"
      "                        newlines don't split records. Run\n"
      "                        `qwc --validate-csv --help` for its options "
      "(--delim,\n"
      "                        --quote, --esc, and "
      "--all/--first/--list/--fast)."
      "\n"
      "\n"
      "Output:\n"
      "  qwc matches wc's layout so it can stand in for it: each file "
      "prints its\n"
      "  right-aligned count(s) followed by the file name, piped input "
      "prints just\n"
      "  the count(s), and counting several files adds a final \"total\" "
      "line.\n"
      "\n"
      "Examples:\n"
      "  qwc notes.txt                 lines, words and bytes in notes.txt\n"
      "  qwc *.log                     counts for each log, plus a total\n"
      "  qwc -l notes.txt              lines in notes.txt\n"
      "  qwc -c notes.txt              bytes in notes.txt\n"
      "  qwc -w notes.txt              words in notes.txt\n"
      "  qwc -m notes.txt              characters in notes.txt\n"
      "  qwc -L notes.txt              length of the longest line in "
      "notes.txt\n"
      "  qwc --char , data.csv         commas in data.csv\n"
      "  qwc --recursive src           counts for every file under src/\n"
      "  qwc -r --top 10 src           the 10 biggest files under src/\n",
      stdout
  );
}

std::optional<i32> parseArgs( i32 argc, char** argv, Options& opt )
{
  // Track whether any count flag was given. With none, qwc prints lines, words
  // and bytes -- the three columns bare `wc` shows. Count flags accumulate (and
  // their order never affects the column order), so `-l -w` and `-lw` and `-wl`
  // are all "lines and words".
  bool countFlag = false;

  // Apply one short count flag letter. Returns false if the letter is unknown.
  auto applyShort = [&]( char c ) -> bool {
    switch ( c ) {
      case 'l':
        opt.lines = true;
        countFlag = true;
        return true;
      case 'w':
        opt.words = true;
        countFlag = true;
        return true;
      case 'c':
        opt.bytes = true;
        countFlag = true;
        return true;
      case 'm':
        opt.chars = true;
        countFlag = true;
        return true;
      case 'L':
        opt.maxLine = true;
        countFlag = true;
        return true;
      case 'r':
        opt.recursive = true;
        return true;
      default:
        return false;
    }
  };

  // Parse leading options; the first non-flag argument begins the file list. A
  // bare "-" is treated as a file, not a flag.
  i32 fileStart = 1;
  while ( fileStart < argc && argv[fileStart][0] == '-' &&
          argv[fileStart][1] != '\0' ) {
    const char* arg = argv[fileStart];
    if ( arg[1] == '-' ) {
      // Long options.
      if ( std::strcmp( arg, "--bytes-per-thread" ) == 0 ) {
        if ( fileStart + 1 >= argc ) {
          std::fputs( "Error: --bytes-per-thread requires a value\n", stderr );
          return 1;
        }
        opt.bytesPerThread = std::strtoull( argv[fileStart + 1], nullptr, 10 );
        if ( opt.bytesPerThread == 0 ) {
          std::fputs( "Error: --bytes-per-thread must be > 0\n", stderr );
          return 1;
        }
        fileStart += 2;
      } else if ( std::strcmp( arg, "--char" ) == 0 ) {
        if ( fileStart + 1 >= argc || argv[fileStart + 1][0] == '\0' ) {
          std::fputs(
              "Error: --char requires a single-character value\n", stderr
          );
          return 1;
        }
        opt.target = true;
        opt.targetByte = argv[fileStart + 1][0];
        countFlag = true;
        fileStart += 2;
      } else if ( std::strcmp( arg, "--lines" ) == 0 ) {
        opt.lines = true;
        countFlag = true;
        fileStart += 1;
      } else if ( std::strcmp( arg, "--words" ) == 0 ) {
        opt.words = true;
        countFlag = true;
        fileStart += 1;
      } else if ( std::strcmp( arg, "--bytes" ) == 0 ) {
        opt.bytes = true;
        countFlag = true;
        fileStart += 1;
      } else if ( std::strcmp( arg, "--chars" ) == 0 ) {
        opt.chars = true;
        countFlag = true;
        fileStart += 1;
      } else if ( std::strcmp( arg, "--max-line-length" ) == 0 ) {
        opt.maxLine = true;
        countFlag = true;
        fileStart += 1;
      } else if ( std::strcmp( arg, "--recursive" ) == 0 ) {
        opt.recursive = true;
        fileStart += 1;
      } else if ( std::strcmp( arg, "--sort-by-count" ) == 0 ) {
        opt.sortMode = SortMode::Count;
        fileStart += 1;
      } else if ( std::strcmp( arg, "--sort-by-name" ) == 0 ) {
        opt.sortMode = SortMode::Name;
        fileStart += 1;
      } else if ( std::strcmp( arg, "--sort-by-size" ) == 0 ) {
        opt.sortMode = SortMode::Size;
        fileStart += 1;
      } else if ( std::strcmp( arg, "--reverse" ) == 0 ) {
        opt.reverse = true;
        fileStart += 1;
      } else if ( std::strcmp( arg, "--top" ) == 0 ) {
        if ( fileStart + 1 >= argc ) {
          std::fputs( "Error: --top requires a value\n", stderr );
          return 1;
        }
        opt.topN = std::strtoull( argv[fileStart + 1], nullptr, 10 );
        if ( opt.topN == 0 ) {
          std::fputs( "Error: --top must be > 0\n", stderr );
          return 1;
        }
        fileStart += 2;
      } else if ( std::strcmp( arg, "--version" ) == 0 ) {
        std::fputs( "qwc " QWC_VERSION "\n", stdout );
        return 0;
      } else if ( std::strcmp( arg, "--help" ) == 0 ) {
        printHelp();
        return 0;
      } else {
        std::fprintf( stderr, "Error: unknown flag %s\n", arg );
        return 1;
      }
    } else if ( std::strcmp( arg, "-h" ) == 0 ) {
      printHelp();
      return 0;
    } else {
      // A bundle of short flags, e.g. "-l", "-lw", "-lwcmL". Each letter is its
      // own count/option flag; wc allows them stacked, so we do too.
      for ( const char* p = arg + 1; *p != '\0'; ++p ) {
        if ( !applyShort( *p ) ) {
          std::fprintf( stderr, "Error: unknown flag -%c\n", *p );
          return 1;
        }
      }
      fileStart += 1;
    }
  }

  // --top needs a ranking criterion; default it to counts when none was asked
  // for, so `qwc -l --top 10 ...` means "the 10 files with the most lines".
  if ( opt.topN > 0 && opt.sortMode == SortMode::None )
    opt.sortMode = SortMode::Count;

  // No count flag means the bare-`wc` view: lines, words and bytes together.
  if ( !countFlag ) {
    opt.lines = true;
    opt.words = true;
    opt.bytes = true;
  }

  for ( i32 i = fileStart; i < argc; ++i ) opt.files.push_back( argv[i] );
  return std::nullopt;
}

// Report a validate-csv usage error and return the exit code (1). A free
// function rather than a lambda (project convention), shared by the option
// checks below.
static std::optional<i32> csvUsageErr( const char* msg )
{
  std::fprintf( stderr, "Error: %s\n", msg );
  return 1;
}

// Apply a mode flag, rejecting a second one (the modes are mutually exclusive).
// `seen` tracks whether any mode flag was already given.
static std::optional<i32> setMode( CsvMode m, CsvMode& mode, bool& seen )
{
  if ( seen )
    return csvUsageErr(
        "--fast, --list, --first and --all are mutually exclusive"
    );
  mode = m;
  seen = true;
  return std::nullopt;
}

std::optional<i32> parseValidateCsvArgs(
    int argc, char** argv, CsvDialect& d, CsvMode& mode, const char*& filename
)
{
  mode = CsvMode::All;  // default: list every ragged row
  bool modeSeen = false;
  filename = nullptr;
  // argv[0] is the program, argv[1] is "--validate-csv"; parse from argv[2].
  for ( int i = 2; i < argc; ++i ) {
    const char* a = argv[i];
    if ( std::strncmp( a, "--delim=", 8 ) == 0 ) {
      if ( a[8] == '\0' || a[9] != '\0' )
        return csvUsageErr( "--delim needs exactly one byte" );
      d.delim = a[8];
    } else if ( std::strncmp( a, "--quote=", 8 ) == 0 ) {
      if ( a[8] == '\0' )
        d.quoting = false;  // --quote= disables quote handling entirely
      else if ( a[9] != '\0' )
        return csvUsageErr( "--quote needs exactly one byte" );
      else
        d.quote = a[8];
    } else if ( std::strncmp( a, "--esc=", 6 ) == 0 ) {
      if ( a[6] == '\0' || a[7] != '\0' )
        return csvUsageErr( "--esc needs exactly one byte" );
      d.esc = a[6];
      d.backslashEsc = true;
    } else if ( std::strcmp( a, "--fast" ) == 0 ) {
      if ( const std::optional<i32> e =
               setMode( CsvMode::Fast, mode, modeSeen ) )
        return e;
    } else if ( std::strcmp( a, "--list" ) == 0 ) {
      if ( const std::optional<i32> e =
               setMode( CsvMode::List, mode, modeSeen ) )
        return e;
    } else if ( std::strcmp( a, "--first" ) == 0 ) {
      if ( const std::optional<i32> e =
               setMode( CsvMode::First, mode, modeSeen ) )
        return e;
    } else if ( std::strcmp( a, "--all" ) == 0 ) {
      if ( const std::optional<i32> e =
               setMode( CsvMode::All, mode, modeSeen ) )
        return e;
    } else if ( std::strcmp( a, "-h" ) == 0 ||
                std::strcmp( a, "--help" ) == 0 ) {
      std::fputs(
          "Usage: qwc --validate-csv [--delim=,] [--quote=\"] [--esc=\\]\n"
          "                          [--fast|--list|--first|--all] FILE\n"
          "\n"
          "Prove a CSV file is rectangular: every record has the same number "
          "of\n"
          "fields. Quoted content (and, with --esc, backslash-escaped bytes) "
          "is\n"
          "masked, so embedded delimiters and newlines do not split records. "
          "Reads\n"
          "standard input when no FILE is given. Exits 0 when valid (printing\n"
          "nothing), 1 when invalid. The report (on stdout, prefixed by the "
          "file\n"
          "name, or `-` for stdin) is selected by:\n"
          "  --all    <name>: r1,r2,...  every ragged row, comma-separated "
          "(default)\n"
          "  --first  <name>: r1         just the first ragged row\n"
          "  --list   <name>             just the name (handy across many "
          "files)\n"
          "  --fast   (no output)        validity as the exit code only\n",
          stdout
      );
      return 0;
    } else if ( a[0] == '-' && a[1] != '\0' ) {
      std::fprintf( stderr, "Error: unknown validate-csv flag %s\n", a );
      return 1;
    } else {
      if ( filename ) return csvUsageErr( "validate-csv takes a single file" );
      filename = a;
    }
  }

  // The delimiter, quote, escape and newline must be distinct so each byte has
  // one meaning. With quoting disabled only the delimiter/newline clash
  // matters.
  if ( d.quoting ) {
    if ( d.delim == d.quote || d.delim == '\n' || d.quote == '\n' )
      return csvUsageErr( "--delim, --quote and newline must be distinct" );
    if ( d.backslashEsc &&
         ( d.esc == d.delim || d.esc == d.quote || d.esc == '\n' ) )
      return csvUsageErr(
          "--esc must differ from --delim, --quote and newline"
      );
  } else if ( d.delim == '\n' ) {
    return csvUsageErr( "--delim must not be newline" );
  }
  return std::nullopt;
}

Options::~Options()
{
  for ( char* p: ownedPaths ) std::free( p );
}

Workload Options::workload() const
{
  Workload w;
  w.lines = lines;
  w.words = words;
  w.maxLine = maxLine;
  w.target = target;
  w.targetByte = targetByte;
  // In a single-byte locale the -m column is served by the byte count
  // (fstat-cheap, no scan); otherwise -m scans for characters. -c and -m are
  // independent columns, so both may be requested at once (GNU wc -cm).
  w.chars = chars && !charsAreBytes;
  w.bytes = bytes || ( chars && charsAreBytes );
  // wc measures the longest line in characters the moment -m is in effect
  // (even alongside -c), bytes otherwise.
  w.maxLineInChars = chars && !charsAreBytes;
  return w;
}

namespace {

// Small hand-rolled RAII guards for the heap path and DIR* in walkDir below.
// Under -fno-exceptions (Release) the would-be-throwing edges in walkDir
// (std::vector::push_back, recursive walkDir) all degrade to terminate(),
// so leaks are impossible there. Debug builds keep exceptions on per the
// binary-size branch's CMake gate, and the previous manual std::free /
// closedir pattern was exception-unsafe: GCC 16's -fanalyzer correctly
// flagged that push_back's bad_alloc could unwind out of walkDir leaving
// `path` and the DIR* leaked (GCC 13 doesn't model this; CI didn't catch
// it). Two tiny structs in the project style; no <memory> include
// (unique_ptr<DIR, decltype(&closedir)> works but the deleter-type spelling
// at every call site doesn't pay rent for two sites).
struct MallocOwner
{
  char* ptr;
  ~MallocOwner() { std::free( ptr ); }
  char* release()
  {
    char* p = ptr;
    ptr = nullptr;
    return p;
  }
  MallocOwner( const MallocOwner& ) = delete;
  MallocOwner& operator=( const MallocOwner& ) = delete;
};

struct DirCloser
{
  DIR* d;
  ~DirCloser() { closedir( d ); }
  DirCloser( const DirCloser& ) = delete;
  DirCloser& operator=( const DirCloser& ) = delete;
};

// "dir/name" in a fresh heap string the caller owns (the separator is omitted
// after a trailing slash, so `qwc -r /` joins to "/etc", not "//etc").
char* joinPath( const char* dir, const char* name )
{
  const usize dlen = std::strlen( dir );
  const usize nlen = std::strlen( name );
  const bool sep = dlen > 0 && dir[dlen - 1] != '/';
  char* out = static_cast<char*>( std::malloc( dlen + sep + nlen + 1 ) );
  if ( !out ) {
    std::fputs( "Error: out of memory\n", stderr );
    std::_Exit( 1 );
  }
  // The terminator arrives with the second memcpy: `nlen + 1` copies name's
  // NUL. The not-null-terminated-result heuristic only sees the first copy.
  // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
  std::memcpy( out, dir, dlen );
  if ( sep ) out[dlen] = '/';
  std::memcpy( out + dlen + sep, name, nlen + 1 );
  return out;
}

// Append every regular file beneath `dir` to `out` as a heap path, recording
// ownership in `owned` (freed by ~Options). Unreadable or unstattable entries
// are skipped, directory symlinks are not walked, and a symlink to a regular
// file counts as that file -- the same stance
// fs::recursive_directory_iterator(skip_permission_denied) + is_regular_file
// took before this was hand-rolled. Returns false only when `dir` itself
// cannot be opened; deeper failures just prune that subtree.
bool walkDir(
    const char* dir, std::vector<const char*>& out, std::vector<char*>& owned
)
{
  DIR* dptr = opendir( dir );
  if ( !dptr ) return false;
  DirCloser d{ dptr };
  // Like the setlocale/nl_langinfo calls in main(): collectFiles runs on the
  // main thread before any workers are spawned, so the thread-safety warning
  // does not apply (and glibc's readdir is in fact safe on distinct DIR
  // streams; only sharing one stream races).
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  while ( const dirent* e = readdir( d.d ) ) {
    const char* name = e->d_name;
    if ( name[0] == '.' &&
         ( name[1] == '\0' || ( name[1] == '.' && name[2] == '\0' ) ) )
      continue;
    MallocOwner path{ joinPath( dir, name ) };
    // d_type is free when the filesystem provides it; DT_UNKNOWN means "go
    // ask" (lstat, so a symlink is still recognized as one), and a symlink is
    // followed one step (stat) to see whether a regular file is behind it.
    unsigned char type = e->d_type;
    if ( type == DT_UNKNOWN || type == DT_LNK ) {
      struct stat st{};
      if ( type == DT_UNKNOWN && lstat( path.ptr, &st ) == 0 ) {
        if ( S_ISDIR( st.st_mode ) )
          type = DT_DIR;
        else if ( S_ISREG( st.st_mode ) )
          type = DT_REG;
        else if ( S_ISLNK( st.st_mode ) )
          type = DT_LNK;
      }
      if ( type == DT_LNK && stat( path.ptr, &st ) == 0 &&
           S_ISREG( st.st_mode ) )
        type = DT_REG;
    }
    if ( type == DT_REG ) {
      // Take ownership in `owned` first (so a bad_alloc from THIS push leaves
      // path with its MallocOwner intact and the guard frees it during
      // unwinding); release the guard so the second push's bad_alloc won't
      // double-free (owned has it, ~Options will free it via opt.ownedPaths).
      char* p = path.ptr;
      owned.push_back( p );
      path.release();
      out.push_back( p );
      continue;
    }
    // For directories the recursive walkDir's own MallocOwner / DirCloser
    // guards manage its allocations; if it throws, our `path` is freed by
    // its MallocOwner during unwinding here, and `d` is closed by DirCloser.
    if ( type == DT_DIR ) walkDir( path.ptr, out, owned );
    // path's MallocOwner dtor runs at end of scope, freeing it (the DT_REG
    // branch above continues out before this).
  }
  return true;  // ~DirCloser closes d.d
}

}  // namespace

bool collectFiles( Options& opt )
{
  // Without --recursive there is nothing to expand: every entry in opt.files
  // is already a file argument straight from argv. Building a fresh vector and
  // moving it over the old one would just be a pointer-copy round-trip, so
  // bail before allocating anything.
  if ( !opt.recursive ) return true;

  // With --recursive, a directory argument is replaced by every regular file
  // beneath it; plain file arguments and the top-level argument order are left
  // untouched. Files come back in directory-iteration order, which is
  // unspecified and may differ between runs. That is fine: without a sort flag
  // qwc does not promise any output order (and with one, the whole list is
  // re-ordered at print time anyway), so alphabetizing here would be pure
  // wasted work on the file-heavy recursive runs.
  std::vector<const char*> expanded;
  for ( const char* path: opt.files ) {
    struct stat st{};
    if ( opt.recursive && stat( path, &st ) == 0 && S_ISDIR( st.st_mode ) ) {
      if ( !walkDir( path, expanded, opt.ownedPaths ) ) {
        std::fprintf( stderr, "Error reading directory: %s\n", path );
        return false;
      }
    } else {
      expanded.push_back( path );
    }
  }
  opt.files = std::move( expanded );
  return true;
}

Columns selectedColumns( const Options& opt )
{
  Columns out{};
  if ( opt.lines ) out.data[out.n++] = Column::Lines;
  if ( opt.words ) out.data[out.n++] = Column::Words;
  if ( opt.chars ) out.data[out.n++] = Column::Chars;
  if ( opt.bytes ) out.data[out.n++] = Column::Bytes;
  if ( opt.maxLine ) out.data[out.n++] = Column::MaxLine;
  if ( opt.target ) out.data[out.n++] = Column::Target;
  return out;
}

namespace {

usize columnValue( const Counts& c, const Column col, const Options& opt )
{
  switch ( col ) {
    case Column::Lines:
      return c.lines;
    case Column::Words:
      return c.words;
    case Column::Chars:
      // Single-byte locale: a character is a byte, and only bytes were
      // counted (see Options::workload).
      return opt.charsAreBytes ? c.bytes : c.chars;
    case Column::Bytes:
      return c.bytes;
    case Column::MaxLine:
      return c.maxLine;
    case Column::Target:
      return c.target;
  }
  return 0;
}

// Side-channel for sortCompare: qsort takes no user-data pointer (unlike the
// non-portable qsort_r), so printResults sets this before each qsort call.
// Safe because printResults runs once on the main thread before exit.
struct SortCtx
{
  const Options* opt;
  const std::vector<Counts>* output;
  const std::vector<usize>* sizes;
  Column col;
};
const SortCtx* g_sortCtx = nullptr;

i32 sortCompare( const void* pa, const void* pb )
{
  const usize a = *static_cast<const usize*>( pa );
  const usize b = *static_cast<const usize*>( pb );
  const SortCtx& ctx = *g_sortCtx;
  if ( ctx.opt->sortMode == SortMode::Count ) {
    const usize va = columnValue( ( *ctx.output )[a], ctx.col, *ctx.opt );
    const usize vb = columnValue( ( *ctx.output )[b], ctx.col, *ctx.opt );
    if ( va != vb ) return va < vb ? -1 : 1;
  } else if ( ctx.opt->sortMode == SortMode::Size &&
              ( *ctx.sizes )[a] != ( *ctx.sizes )[b] ) {
    return ( *ctx.sizes )[a] < ( *ctx.sizes )[b] ? -1 : 1;
  }
  return std::strcmp( ctx.opt->files[a], ctx.opt->files[b] );
}

}  // namespace

void printCounts(
    const Options& opt, const Columns& cols, const Counts& c, const char* name
)
{
  // Each selected column right-justified in a min-width-7 field that grows for
  // larger counts exactly as wc does, then the optional name.
  for ( u8 i = 0; i < cols.n; ++i )
    std::printf( " %7zu", columnValue( c, cols.data[i], opt ) );
  if ( name ) std::printf( " %s", name );
  std::putchar( '\n' );
}

void printResults( const Options& opt, const std::vector<Counts>& output )
{
  const usize numFiles = output.size();
  const Columns cols = selectedColumns( opt );

  // The grand total covers every file, independent of sorting or --top. Each
  // column sums, except the longest line, which is a maximum (matching wc -L).
  Counts total{};
  for ( const Counts& c: output ) {
    total.lines += c.lines;
    total.words += c.words;
    total.bytes += c.bytes;
    total.chars += c.chars;
    total.target += c.target;
    total.maxLine = std::max( total.maxLine, c.maxLine );
  }

  // Display order via an index permutation, leaving `output` untouched.
  // Sorting,
  // --top and --reverse only apply to a single-column listing -- there is no
  // one value to rank by otherwise -- so bare/multi-column qwc keeps the
  // collected order, exactly like wc.
  std::vector<usize> order( numFiles );
  std::iota( order.begin(), order.end(), 0 );

  const bool single = opt.columnCount() == 1;
  if ( single && opt.sortMode != SortMode::None ) {
    const Column col = cols.data[0];

    // File sizes are only needed for --sort-by-size, so fetch them lazily.
    // An unstattable file ranks as size 0, like the old error_code path.
    std::vector<usize> sizes;
    if ( opt.sortMode == SortMode::Size ) {
      sizes.resize( numFiles );
      for ( usize i = 0; i < numFiles; ++i ) {
        struct stat st{};
        sizes[i] = stat( opt.files[i], &st ) == 0
                       ? static_cast<usize>( st.st_size )
                       : 0;
      }
    }

    // Sort ascending by the chosen key, tie-broken by filename so output is
    // deterministic regardless of which thread finished first. qsort instead
    // of std::sort to keep introsort + heap-adjust + insertion-sort out of
    // the static image (~16 KB on a cold --sort-by-* path); the comparator
    // reaches its context via the g_sortCtx side-channel since qsort takes
    // no user-data pointer.
    const SortCtx ctx{ &opt, &output, &sizes, col };
    g_sortCtx = &ctx;
    std::qsort( order.data(), order.size(), sizeof( usize ), sortCompare );
    g_sortCtx = nullptr;
  }

  // --top N keeps the N highest-ranked files: the tail of the ascending order.
  if ( single && opt.topN > 0 && opt.topN < order.size() )
    order.erase( order.begin(), order.end() - static_cast<isize>( opt.topN ) );

  // --reverse flips the final display order (e.g. biggest counts first). With
  // no sort key in effect there is no promised order to flip -- qwc emits the
  // collected order, which for -r is unspecified -- so it is a no-op.
  if ( single && opt.reverse && opt.sortMode != SortMode::None )
    std::reverse( order.begin(), order.end() );

  // wc prints one row per file -- including for a single file (with its name).
  for ( const usize i: order )
    printCounts( opt, cols, output[i], opt.files[i] );

  // The "total" row appears only when more than one file was counted, matching
  // wc. (--top may narrow the listing above, but the total still covers every
  // file.) A recursive walk that matched no files has nothing to name, so it
  // just prints a bare zero row rather than nothing at all.
  if ( numFiles > 1 )
    printCounts( opt, cols, total, "total" );
  else if ( numFiles == 0 )
    printCounts( opt, cols, total, nullptr );
}
