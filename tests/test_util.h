#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "chars.h"
#include "countlines.h"
#include "iswprint_table.h"
#include "maxlinelen.h"
#include "processfile.h"
#include "typedef.h"
#include "words.h"

namespace qwctest {

// Default bytes-per-thread, matching processFile's own default.
inline constexpr usize kDefaultBpt = 64u * 1024 * 1024;

// Single-counter convenience wrappers around the unified processFile, so tests
// can ask for one count without spelling out a Workload each time.
inline usize pfLines( const char* path, usize bpt = kDefaultBpt )
{
  Workload w;
  w.lines = true;
  return processFile( path, w, bpt ).lines;
}
inline usize pfWords(
    const char* path, usize bpt = kDefaultBpt, const WordsMode m = {}
)
{
  Workload w;
  w.words = true;
  w.wordsMode = m;
  return processFile( path, w, bpt ).words;
}
inline usize pfBytes( const char* path, usize bpt = kDefaultBpt )
{
  Workload w;
  w.bytes = true;
  return processFile( path, w, bpt ).bytes;
}
inline usize pfChars( const char* path, usize bpt = kDefaultBpt )
{
  Workload w;
  w.chars = true;
  return processFile( path, w, bpt ).chars;
}
inline usize pfMaxLine( const char* path, usize bpt = kDefaultBpt )
{
  Workload w;
  w.maxLine = true;
  return processFile( path, w, bpt ).maxLine;
}
inline usize pfTarget( const char* path, char target, usize bpt = kDefaultBpt )
{
  Workload w;
  w.target = true;
  w.targetByte = target;
  return processFile( path, w, bpt ).target;
}

// Independent, obviously-correct reference implementation of `count`, used as
// the oracle for the SIMD/scalar implementation under test. Compares at the
// byte level so it is immune to plain-char signedness differences.
inline usize refCount( const char* buffer, usize length, char target )
{
  const auto t = static_cast<unsigned char>( target );
  usize n = 0;
  for ( usize i = 0; i < length; ++i )
    if ( static_cast<unsigned char>( buffer[i] ) == t ) ++n;
  return n;
}

inline usize refCount( const std::string& s, char target = '\n' )
{
  return refCount( s.data(), s.size(), target );
}

inline usize countStr( const std::string& s, char target = '\n' )
{
  return count( s.data(), s.size(), target );
}

// Independent reference for `words`. Deliberately structured differently from
// the implementation under test -- it tokenizes by skipping whitespace then
// skipping a word, rather than counting whitespace->word transitions -- so a
// shared bug is unlikely. Whitespace is the C-locale set.
inline bool refIsSpace( unsigned char c )
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

// Reference word count, wc semantics: a word is a maximal non-separator run
// containing >= 1 printable character. Independent of the kernel's structure
// (one linear walk with local state, no owned-region/window machinery); uses
// the same generated iswprint table, which words_test.cpp spot-checks against
// known glibc classifications.
inline usize refWords(
    const std::string& s, const bool utf8 = false, const bool nbspace = true
)
{
  const auto* p = reinterpret_cast<const unsigned char*>( s.data() );
  const usize n = s.size();
  usize count = 0, i = 0;
  bool inWord = false, hasPrint = false;
  while ( i < n ) {
    u32 cp = p[i];
    usize len = 1;
    bool valid = cp < 0x80;
    if ( utf8 && cp >= 0xC2 && cp <= 0xF4 ) {  // possible multibyte lead
      const usize need = cp < 0xE0 ? 2 : cp < 0xF0 ? 3 : 4;
      if ( i + need <= n ) {
        u32 v = cp & ( 0xFFu >> ( need + 1 ) );
        valid = true;
        for ( usize k = 1; k < need; ++k ) {
          if ( ( p[i + k] & 0xC0 ) != 0x80 ) {
            valid = false;
            break;
          }
          v = ( v << 6 ) | ( p[i + k] & 0x3F );
        }
        // Strictness mirrors mbrtowc: no overlongs, no surrogates, <= 10FFFF.
        static const u32 lo[5] = { 0, 0, 0x80, 0x800, 0x10000 };
        if ( valid && ( v < lo[need] || v > 0x10FFFF ||
                        ( v >= 0xD800 && v <= 0xDFFF ) ) )
          valid = false;
        if ( valid ) {
          cp = v;
          len = need;
        } else {
          valid = false;
        }
      } else {
        valid = false;
      }
    } else if ( cp >= 0x80 ) {
      valid = false;  // continuation byte or invalid lead (also the C path)
    }
    const bool sep = utf8 ? ( valid && isSepCp( cp, nbspace ) )
                          : refIsSpace( static_cast<unsigned char>( cp ) );
    const bool print =
        utf8 ? ( valid && qwcIswprint( cp ) ) : ( cp >= 0x21 && cp <= 0x7E );
    if ( sep ) {
      if ( inWord && hasPrint ) ++count;
      inWord = false;
      hasPrint = false;
    } else {
      inWord = true;
      if ( print ) hasPrint = true;
    }
    i += len;
  }
  if ( inWord && hasPrint ) ++count;
  return count;
}

// Run `words` over a whole string in one shot (fresh state, then flushed).
inline usize wordsStr( const std::string& s, const WordsMode m = {} )
{
  WordScan ws;
  words( s.data(), s.size(), 0, s.size(), ws, m );
  wordsFlush( ws );
  return ws.words;
}

// Independent reference for `chars` (UTF-8 code points). Structured differently
// from the implementation under test: it walks the leading byte of each
// sequence and skips the continuation bytes, rather than counting
// non-continuation bytes, so a shared bug is unlikely. For well-formed UTF-8 it
// equals `wc -m`.
inline usize refChars( const std::string& s )
{
  usize n = 0;
  size_t i = 0;
  while ( i < s.size() ) {
    const auto b = static_cast<unsigned char>( s[i] );
    size_t len = 1;
    if ( ( b & 0x80 ) == 0x00 )
      len = 1;  // 0xxxxxxx
    else if ( ( b & 0xE0 ) == 0xC0 )
      len = 2;  // 110xxxxx
    else if ( ( b & 0xF0 ) == 0xE0 )
      len = 3;  // 1110xxxx
    else if ( ( b & 0xF8 ) == 0xF0 )
      len = 4;  // 11110xxx
    i += len;
    ++n;
  }
  return n;
}

// Run `chars` over a whole string in one shot.
inline usize charsStr( const std::string& s )
{
  return chars( s.data(), s.size() );
}

// Run `chars` over the string fed in fixed-size pieces, the way a single thread
// streams successive read buffers. Because each byte is classified on its own,
// this must agree with charsStr regardless of where the splits land.
inline usize charsChunked( const std::string& s, size_t chunk )
{
  usize total = 0;
  for ( size_t i = 0; i < s.size(); i += chunk )
    total += chars( s.data() + i, std::min( chunk, s.size() - i ) );
  return total;
}

// Independent reference for the longest-line length (`wc -L`). Structured by
// splitting on newlines and measuring each segment's byte length, rather than
// the running-counter approach the implementation uses, so a shared bug is
// unlikely. The newline is excluded, and -- matching `wc -L` -- a trailing run
// with no final newline is not a counted line (the bytes after the last newline
// are intentionally ignored).
inline usize refMaxLineLen( const std::string& s )
{
  usize best = 0;
  size_t start = 0;
  for ( size_t i = 0; i < s.size(); ++i ) {
    if ( s[i] == '\n' ) {
      best = std::max( best, i - start );  // line [start, i), newline excluded
      start = i + 1;
    }
  }
  return best;
}

// Run `maxLineLen` over a whole string in one shot. The answer is the longest
// newline-terminated line; the trailing open run (`ls.cur`) is dropped.
inline usize maxLineLenStr( const std::string& s )
{
  LineScan ls;
  maxLineLen( s.data(), s.size(), ls );
  return ls.maxComplete;
}

// Run `maxLineLen` over the string fed in fixed-size pieces, carrying state
// across them, the way a single thread streams successive read buffers within
// one chunk. Must agree with maxLineLenStr regardless of where splits land.
inline usize maxLineLenChunked( const std::string& s, size_t chunk )
{
  LineScan ls;
  for ( size_t i = 0; i < s.size(); i += chunk )
    maxLineLen( s.data() + i, std::min( chunk, s.size() - i ), ls );
  return ls.maxComplete;
}

// Longest line measured in characters, via the dedicated maxLineLen in
// character mode -- the reference the fused -L -m scanner must match.
inline usize charModeMaxLine( const std::string& s )
{
  LineScan ls;
  maxLineLen( s.data(), s.size(), ls, /*countChars=*/true );
  return ls.maxComplete;
}

// The fused `-L -m` scanner over the whole string: returns {longest line in
// characters, character total}.
inline std::pair<usize, usize> fusedLineChars( const std::string& s )
{
  LineScan ls;
  usize cc = 0;
  maxLineLenChars( s.data(), s.size(), ls, cc );
  return { ls.maxComplete, cc };
}

// Same, fed in fixed-size pieces with state carried across them -- the way one
// thread streams successive read buffers within a chunk.
inline std::pair<usize, usize> fusedLineCharsChunked(
    const std::string& s, size_t chunk
)
{
  LineScan ls;
  usize cc = 0;
  for ( size_t i = 0; i < s.size(); i += chunk )
    maxLineLenChars( s.data() + i, std::min( chunk, s.size() - i ), ls, cc );
  return { ls.maxComplete, cc };
}

// Run `words` feeding the string in fixed-size pieces, carrying state across
// them the way a single thread streams successive read buffers: each call owns
// its piece and sees up to 3 bytes of context on both sides, mirroring the
// overlap reads in processfile. Must agree with wordsStr/refWords regardless
// of where the splits land.
inline usize wordsChunked(
    const std::string& s, size_t chunk, const WordsMode m = {}
)
{
  WordScan ws;
  for ( size_t i = 0; i < s.size(); i += chunk ) {
    const size_t end = std::min( i + chunk, s.size() );
    const size_t front = std::min<size_t>( i, 3 );
    const size_t back = std::min<size_t>( s.size() - end, 3 );
    words(
        s.data() + i - front, ( end + back ) - ( i - front ), front,
        front + ( end - i ), ws, m
    );
  }
  wordsFlush( ws );
  return ws.words;
}

}  // namespace qwctest
