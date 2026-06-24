/*
 * Copyright Ji Krochmal 2026
 */
#pragma once
#include <optional>

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

// CLI entry: run validateCsvFile, print `bad row: N` to stderr on an invalid
// file (unless `fast`), and return the process exit code (0 valid, 1 invalid).
// In fast mode the parallel driver exits immediately on the first known
// mismatch, skipping the inspection re-scan.
int validateCsv(
    const char* filename, const CsvDialect& d, bool fast,
    usize bytesPerThread = 64ull * 1024 * 1024
);

// Parse the argv tail after `--validate-csv` into `d`/`fast`/`filename`.
// Returns a process exit code to stop (usage error or --help), or std::nullopt
// to proceed. `filename` is left null for the stdin case.
std::optional<i32> parseValidateCsvArgs(
    int argc, char** argv, CsvDialect& d, bool& fast, const char*& filename
);
