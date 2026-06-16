/*
 * Copyright Ji Krochmal 2026
 */
#pragma once
#include "typedef.h"
#include "words.h"

// Which counts to produce for an input. The frontend sets these from the
// command line once, up front, and a single pass computes every requested
// counter at once -- so `qwc -l -w -m -L` reads the data once, not four times.
//
// `bytes` and `chars` may both be set (GNU wc -cm shows both columns); bytes
// alone still needs no scan (fstat). `target` is the qwc-only `--char`
// extension (count occurrences of `targetByte`).
struct Workload
{
  bool lines = false;
  bool words = false;
  bool bytes = false;
  bool chars = false;
  bool maxLine = false;
  bool maxLineInChars =
      false;  // -L measures characters (when -m is active), not bytes
  bool target = false;
  char targetByte = '\n';
  WordsMode wordsMode;  // locale-resolved word splitting (set once in main)

  // Does anything here require scanning the file contents? Bytes alone come
  // from fstat, so a pure `-c` needs no scan at all.
  bool needsScan() const
  {
    return lines || words || chars || maxLine || target;
  }
};

// Every count qwc can produce for one input. Only the fields the Workload asked
// for are populated; the rest stay zero.
struct Counts
{
  usize lines = 0;
  usize words = 0;
  usize bytes = 0;
  usize chars = 0;
  usize maxLine = 0;
  usize target = 0;
};

// Compute the requested counts for `filename` (an empty name reads standard
// input) in one pass. A regular file with a known, nonzero size takes the fast
// path: bytes come straight from fstat and the scanned counters are computed in
// parallel across chunks, with words and the longest line stitched back
// together across chunk boundaries. Anything whose size fstat cannot be trusted
// for -- standard input, FIFOs, devices, sockets, and procfs/sysfs files that
// report a zero size yet have content -- is instead read serially to EOF (bytes
// tallied as read): slower, but correct everywhere. Either way every requested
// counter shares the same read of the data.
Counts processFile(
    const char* filename, const Workload& work,
    usize bytesPerThread = 64ull * 1024 * 1024
);
