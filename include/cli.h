/*
 * Copyright Ji Krochmal 2026
 */
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "processfile.h"
#include "typedef.h"

// How to order the per-file output list. The chosen key governs both the
// display order and which files --top keeps; ordering is ascending by default
// (so the largest land at the bottom), and --reverse flips it.
enum class SortMode
{
  None,
  Count,
  Name,
  Size
};

// Everything the frontend parses out of the command line. The count flags map
// to wc's output columns, which always print in a fixed order regardless of the
// order the flags were given: lines, words, chars, bytes, longest line.
struct Options
{
  usize bytesPerThread = 64ull * 1024 * 1024;

  // Selected columns. With no count flag at all, qwc shows lines, words and
  // bytes -- exactly like bare `wc`.
  bool lines = false;  // -l
  bool words = false;  // -w
  bool bytes = false;  // -c: the byte column
  bool chars = false;  // -m: the character column (GNU prints both for -cm)
  // Resolved in main() after setlocale: in a single-byte locale a character
  // IS a byte, so the -m column displays the byte count and no character
  // scan is needed. The column itself still prints (GNU -cm in the C locale
  // shows two identical numbers).
  bool charsAreBytes = false;
  bool maxLine = false;  // -L
  bool target = false;   // --char: qwc extension (counts targetByte)
  char targetByte = '\n';

  bool recursive = false;  // expand directory arguments
  SortMode sortMode = SortMode::None;
  bool reverse = false;  // flip the display order
  usize topN = 0;        // keep only the highest N (0 == show all)
  // Positional args; expanded by collectFiles. Entries either point into argv
  // or at heap paths allocated by the --recursive walk; `files` itself owns
  // nothing and never copies.
  std::vector<const char*> files;
  // The owning side of the walk-allocated entries in `files`, freed by the
  // destructor. Kept separately because the argv pointers mixed into `files`
  // must never be freed. (Without this the paths would still be reachable
  // right up to the end of main -- but LeakSanitizer scans after main's
  // locals are gone, and the analysis CI runs with detect_leaks on.)
  std::vector<char*> ownedPaths;

  Options() = default;
  Options( const Options& ) = delete;
  Options& operator=( const Options& ) = delete;
  Options( Options&& ) = delete;
  Options& operator=( Options&& ) = delete;
  ~Options();

  // How many output columns are selected.
  int columnCount() const
  {
    return static_cast<int>( lines ) + static_cast<int>( words ) +
           static_cast<int>( chars ) + static_cast<int>( bytes ) +
           static_cast<int>( maxLine ) + static_cast<int>( target );
  }

  // The counting workload these options imply (in a single-byte locale the
  // chars column is served by the byte count, so no character scan runs).
  Workload workload() const;
};

// qwc's output columns, in wc's fixed order: lines, words, chars, bytes,
// longest line. The --char tally (qwc-only) has no wc counterpart, so it is
// appended last. `: u8` keeps the per-element width at one byte so the
// Columns array below stays tight.
enum class Column : u8
{
  Lines,
  Words,
  Chars,
  Bytes,
  MaxLine,
  Target
};

// The selected output columns for a single qwc invocation. Capped at the six
// existing column kinds; `n` is the number of populated entries. Stack-only,
// computed once per invocation in `printResults` / `main`, reused for every
// row and the total.
struct Columns
{
  std::array<Column, 6> data;
  u8 n;
};

// Print the help / usage text to stdout.
void printHelp();

// Parse argv into `opt`. Returns std::nullopt to keep running; otherwise the
// process exit code (0 after --help or --version, 1 on a usage error). Help,
// version and error messages are written here.
std::optional<i32> parseArgs( int argc, char** argv, Options& opt );

// Expand any directory arguments in opt.files into the regular files beneath
// them (only under --recursive); plain file arguments are left as-is. Returns
// false after reporting an unreadable directory.
bool collectFiles( Options& opt );

// The set of output columns the given Options select, in wc's fixed order.
Columns selectedColumns( const Options& opt );

// Print one output row: each selected column right-justified in a min-width-7
// field (matching wc's " %7ju"), then -- when `name` is non-null -- a space
// and the name. Callers compute `cols` once and pass it down. Used for stdin
// (no name), each file, and the "total" line.
void printCounts(
    const Options& opt, const Columns& cols, const Counts& c, const char* name
);

// Render the counted files in wc's layout: one row per file (selected columns +
// name), plus a trailing "total" row when more than one file was counted. With
// exactly one column selected the listing honours --sort-*/--top/--reverse;
// with several columns it keeps the collected order, like bare `wc`.
void printResults( const Options& opt, const std::vector<Counts>& output );
