/*
 * Copyright Ji Krochmal 2026
 */
#pragma once
#include <optional>
#include <vector>

#include "typedef.h"

// Parsed CSV dialect for `qwc --validate-csv`. `backslashEsc` is true when the
// caller passed --esc (so `esc` escapes the next byte everywhere and
// `esc`+quote is a literal quote); without it the RFC-4180 doubled-quote
// convention applies
// (`""` is a literal quote). `quoting` is false when --quote= disabled quoting,
// in which case the quote byte is ordinary content and every chunk is "clean".
struct CsvDialect
{
  char delim = ',';
  char quote = '"';
  char esc = '\\';
  bool backslashEsc = false;
  bool quoting = true;
};

// Outcome of validating one CSV input. `badRow` is the 1-based logical-record
// number of the first record whose field count differs from the first record's
// (or the record holding an unterminated quote); it is only meaningful when
// `valid` is false.
struct CsvVerdict
{
  bool valid = true;
  usize badRow = 0;
};

// Validate an in-memory CSV buffer in one sequential pass. The reference path:
// also serves the stdin scan and the failure inspection re-scan. A record
// completes at every unquoted '\n'; the first record's delimiter count is the
// reference, and rectangularity means every record matches it.
CsvVerdict validateCsvBuffer( const char* buf, usize len, const CsvDialect& d );

// Validate the named file (an empty name reads standard input). A regular file
// with a trustworthy size takes the parallel two-phase path; everything else is
// streamed and handed to validateCsvBuffer. `bytesPerThread` controls chunking
// (tests pass small values to force many chunk seams).
CsvVerdict validateCsvFile(
    const char* filename, const CsvDialect& d,
    usize bytesPerThread = 64ull * 1024 * 1024
);

// How an invalid file is reported. All modes share the parallel validity check
// (so the valid-file fast path is identical); they differ only in what a
// failure prints and how much of it is computed. A valid file always prints
// nothing and exits 0; any invalid file exits 1. Output goes to stdout (it is
// data, meant to be piped / globbed), prefixed by the file name (`-` for
// stdin).
enum class CsvMode
{
  Fast,   // print nothing (validity is the exit code only)
  List,   // print just "<name>" (no scan to enumerate rows -- stays fast)
  First,  // print "<name>: <first bad row>"
  All     // print "<name>: r1,r2,..." (every ragged row, capped) -- the default
};

// Validate one input (`filename`, empty name = stdin) and report per `mode`.
// Returns the process exit code (0 valid, 1 invalid).
i32 validateCsv(
    const char* filename, const CsvDialect& d, CsvMode mode,
    usize bytesPerThread = 64ull * 1024 * 1024
);

// The enumerated ragged rows of one input: ascending 1-based row numbers (up to
// the requested `cap`) and `truncated` = "more than cap bad rows exist". For a
// valid input `rows` is empty. This is the parallel default-mode (--all/--first)
// inspection; tests pass a small `bytesPerThread` to force chunk seams.
struct CsvBadRows
{
  std::vector<usize> rows;
  bool truncated = false;
};

CsvBadRows validateCsvBadRows(
    const char* filename, const CsvDialect& d, usize cap,
    usize bytesPerThread = 64ull * 1024 * 1024
);

// Validate every file in `files` (empty = standard input) per `mode`, each
// using its own internal chunk-parallelism, printing one report line per
// invalid file in argument order. Returns 1 if any file is invalid, else 0; in
// Fast mode it returns 1 as soon as a file fails (skipping the rest).
i32 validateCsvFiles(
    const std::vector<const char*>& files, const CsvDialect& d, CsvMode mode,
    usize bytesPerThread = 64ull * 1024 * 1024
);

// Parse the argv tail after `--validate-csv` into `d`/`mode`/`files`. Returns a
// process exit code to stop (usage error or --help), or std::nullopt to
// proceed. `files` is left empty for the stdin case; `mode` defaults to
// CsvMode::All and the four mode flags are mutually exclusive.
std::optional<i32> parseValidateCsvArgs(
    i32 argc, char** argv, CsvDialect& d, CsvMode& mode,
    std::vector<const char*>& files
);
