/*
 * Copyright Ji Krochmal 2026
 */
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "validatecsv.h"

// Argument parsing for `qwc --validate-csv`. Kept in its own translation unit
// rather than cli.cpp so the counting frontend stays lean -- and, concretely,
// so this code does not enlarge cli.cpp past the point where GCC 13's
// -fanalyzer starts exploring (and falsely flagging a leak in) walkDir's
// MallocOwner/ownedPaths handoff. See CMakeLists' -fanalyzer notes.

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
    i32 argc, char** argv, CsvDialect& d, CsvMode& mode,
    std::vector<const char*>& files
)
{
  mode = CsvMode::All;  // default: list every ragged row
  bool modeSeen = false;
  // argv[0] is the program, argv[1] is "--validate-csv"; parse from argv[2].
  for ( i32 i = 2; i < argc; ++i ) {
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
          "                          [--fast|--list|--first|--all] [FILE ...]\n"
          "\n"
          "Prove CSV files are rectangular: every record has the same number "
          "of\n"
          "fields. Quoted content (and, with --esc, backslash-escaped bytes) "
          "is\n"
          "masked, so embedded delimiters and newlines do not split records. "
          "Reads\n"
          "standard input when no FILE is given; with several files each is\n"
          "validated independently and only the invalid ones are reported, "
          "one\n"
          "line each in argument order. Exits 0 when every input is valid\n"
          "(printing nothing), 1 when any is invalid. The per-file report (on\n"
          "stdout, prefixed by the file name, or `-` for stdin) is selected "
          "by:\n"
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
      files.push_back( a );  // a file argument (the shell expands any glob)
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
