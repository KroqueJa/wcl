/*
 * Copyright Ji Krochmal 2026
 */
#include "maxlinelen.h"

// Naive, scalar longest-line scanner. Like words_scalar and chars_scalar it is
// unvectorised by hand; the parallel chunking in processFile supplies the
// throughput, and the per-chunk results are stitched there.
void maxLineLen(
    const char* buffer, const usize length, LineScan& s, const bool countChars
)
{
  for ( usize i = 0; i < length; ++i ) {
    if ( buffer[i] == '\n' ) {
      // The first line of the range is its prefix: it may join a line left open
      // by the previous chunk, so the merge needs its length on its own.
      if ( !s.hasNewline ) {
        s.prefixLen = s.cur;
        s.hasNewline = true;
      }
      if ( s.cur > s.maxComplete ) s.maxComplete = s.cur;
      s.cur = 0;  // the newline is not counted; start the next line
    } else if ( countChars &&
                ( static_cast<unsigned char>( buffer[i] ) & 0xC0 ) == 0x80 ) {
      // A UTF-8 continuation byte continues the current character; in character
      // mode it does not advance the line length.
      continue;
    } else {
      ++s.cur;
    }
  }
}

void maxLineLenChars(
    const char* buffer, const usize length, LineScan& s, usize& charCount
)
{
  for ( usize i = 0; i < length; ++i ) {
    const unsigned char c = static_cast<unsigned char>( buffer[i] );
    if ( c == '\n' ) {
      ++charCount;  // the newline is itself a character...
      if ( !s.hasNewline ) {
        s.prefixLen = s.cur;
        s.hasNewline = true;
      }
      if ( s.cur > s.maxComplete ) s.maxComplete = s.cur;
      s.cur = 0;  // ...but it does not extend the line's character length
    } else if ( ( c & 0xC0 ) == 0x80 ) {
      // UTF-8 continuation byte: neither a new character nor a length advance.
    } else {
      ++charCount;
      ++s.cur;
    }
  }
}
