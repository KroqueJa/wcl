#include "maxlinelen.h"

#include <gtest/gtest.h>

#include <random>
#include <string>

#include "test_util.h"

using qwctest::charModeMaxLine;
using qwctest::charsStr;
using qwctest::fusedLineChars;
using qwctest::fusedLineCharsChunked;
using qwctest::maxLineLenChunked;
using qwctest::maxLineLenStr;
using qwctest::refMaxLineLen;

// The longest-line length, like `wc -L`: the most bytes between newlines, with
// the newline itself excluded. Only newline-terminated lines count -- a
// trailing run with no final newline is ignored (matching macOS/BSD wc).

// ---------------------------------------------------------------------------
// Basic / edge cases
// ---------------------------------------------------------------------------
TEST( MaxLineLen, EmptyBuffer )
{ EXPECT_EQ( maxLineLenStr( "" ), 0u ); }

TEST( MaxLineLen, ZeroLengthIgnoresPointer )
{
  LineScan ls;
  const char* garbage = "abcdef";
  maxLineLen( garbage, 0, ls );
  EXPECT_EQ( ls.maxComplete, 0u );
}

TEST( MaxLineLen, SingleLineNoNewlineNotCounted )
{
  // No terminating newline -> wc -L reports 0, not 4.
  EXPECT_EQ( maxLineLenStr( "abcd" ), 0u );
}

TEST( MaxLineLen, TrailingNewlineExcluded )
{ EXPECT_EQ( maxLineLenStr( "abcd\n" ), 4u ); }

TEST( MaxLineLen, BlankLineIsZero )
{
  EXPECT_EQ( maxLineLenStr( "\n" ), 0u );
  EXPECT_EQ( maxLineLenStr( "\n\n\n" ), 0u );
}

TEST( MaxLineLen, LongestAmongSeveral )
{ EXPECT_EQ( maxLineLenStr( "ab\nabcd\nx\n" ), 4u ); }

TEST( MaxLineLen, UnterminatedFinalLineIgnored )
{
  // "ccccc" has no trailing newline, so the longest *terminated* line is "bb".
  EXPECT_EQ( maxLineLenStr( "a\nbb\nccccc" ), 2u );
}

TEST( MaxLineLen, TabsAndControlBytesCountAsOne )
{
  // No tab-stop expansion: "a\tb" is 3 bytes, "\a" (bell) + x is 2.
  EXPECT_EQ( maxLineLenStr( "a\tb\n" ), 3u );
  EXPECT_EQ( maxLineLenStr( "\ax\n" ), 2u );
}

TEST( MaxLineLen, CarriageReturnCounts )
{
  // CR is an ordinary byte here; "a\r" before the newline is length 2.
  EXPECT_EQ( maxLineLenStr( "a\r\n" ), 2u );
}

TEST( MaxLineLen, EmbeddedNulCounts )
{
  const char buf[] = { 'a', '\0', 'b', '\n' };  // line "a\0b" -> 3
  LineScan ls;
  maxLineLen( buf, sizeof( buf ), ls );
  EXPECT_EQ( ls.maxComplete, 3u );
}

// ---------------------------------------------------------------------------
// Streaming carry: the same data fed in pieces, carrying state, must match a
// single-shot scan. This is the property the per-thread read loop relies on.
// ---------------------------------------------------------------------------
TEST( MaxLineLen, CarryAcrossBuffersMatchesWhole )
{
  const std::string s = "short\nmuch longer line here\ntiny\n";
  const usize expected = refMaxLineLen( s );
  ASSERT_EQ( maxLineLenStr( s ), expected );
  for ( size_t chunk: { size_t( 1 ), size_t( 2 ), size_t( 3 ), size_t( 5 ),
                        size_t( 7 ), size_t( 8 ), size_t( 16 ) } )
    EXPECT_EQ( maxLineLenChunked( s, chunk ), expected ) << "chunk=" << chunk;
}

TEST( MaxLineLen, SplitInsideLongLineDoesNotMiscount )
{
  // One long (terminated) line split anywhere must still report its full
  // length.
  const std::string s = std::string( 50, 'x' ) + "\n";
  for ( size_t chunk = 1; chunk <= s.size(); ++chunk )
    EXPECT_EQ( maxLineLenChunked( s, chunk ), 50u ) << "chunk=" << chunk;
}

// ---------------------------------------------------------------------------
// Randomized differential test against the reference, including random splits.
// ---------------------------------------------------------------------------
TEST( MaxLineLen, FuzzAgainstReference )
{
  std::mt19937_64 rng( 0x1EAF );
  // An alphabet rich in newlines makes lines short and boundaries dense.
  const std::string alphabet = "ab\n\ncd\n";
  std::uniform_int_distribution<size_t> lenDist( 0, 3000 );
  std::uniform_int_distribution<size_t> charDist( 0, alphabet.size() - 1 );

  for ( int iter = 0; iter < 2000; ++iter ) {
    const size_t len = lenDist( rng );
    std::string s( len, ' ' );
    for ( size_t i = 0; i < len; ++i ) s[i] = alphabet[charDist( rng )];

    const usize expected = refMaxLineLen( s );
    EXPECT_EQ( maxLineLenStr( s ), expected )
        << "iter=" << iter << " len=" << len;

    std::uniform_int_distribution<size_t> splitDist( 1, len + 1 );
    EXPECT_EQ( maxLineLenChunked( s, splitDist( rng ) ), expected )
        << "iter=" << iter << " len=" << len;
  }
}

// ---------------------------------------------------------------------------
// Boundary cases for the 4-wide (128-byte) outer iteration. The 4x32 path's
// hot loop processes 128 bytes per iteration with a single newline test
// across four 32-byte sub-blocks; these tests pin down each sub-block
// position and the trailing 0..3 32-byte blocks so the rewrite's hit and
// tail paths can't silently regress. They pass against the baseline.
// ---------------------------------------------------------------------------
TEST( MaxLineLen, CleanWindow128NoNewline )
{
  // Two full 128-byte clean iterations + 0-byte tail. No newline anywhere ->
  // wc -L is 0 (no terminated line).
  const std::string s( 256, 'x' );
  EXPECT_EQ( maxLineLenStr( s ), 0u );
}

TEST( MaxLineLen, CleanWindow128ThenTerminated )
{
  // 200 'x' bytes then newline -> one terminated line of length 200 spanning
  // the first 128-byte window and into the trailing 32-byte tail.
  const std::string s = std::string( 200, 'x' ) + "\n";
  EXPECT_EQ( maxLineLenStr( s ), 200u );
}

TEST( MaxLineLen, NewlineInEachSubBlockPosition )
{
  // For each sub-block offset (0, 32, 64, 96) inside a 128-byte window: the
  // hit path has to identify and process exactly the right 32-byte block.
  for ( size_t pos:
        { size_t( 0 ), size_t( 32 ), size_t( 64 ), size_t( 96 ) } ) {
    std::string s( 128, 'x' );
    s[pos] = '\n';
    // Longest terminated line is the prefix [0, pos).
    const usize expected = pos;
    EXPECT_EQ( maxLineLenStr( s ), expected ) << "newline at pos=" << pos;
  }
}

TEST( MaxLineLen, TrailingTailOf1To3Blocks )
{
  // 128 clean + N*32 clean + "\n" for N in {1,2,3}: exercises 32-byte tail
  // loop after the 4-wide outer loop runs once.
  for ( size_t n: { size_t( 1 ), size_t( 2 ), size_t( 3 ) } ) {
    const size_t lineLen = 128 + n * 32;
    const std::string s = std::string( lineLen, 'x' ) + "\n";
    EXPECT_EQ( maxLineLenStr( s ), lineLen ) << "n=" << n;
  }
}

TEST( MaxLineLen, CharModeCleanWindow128WithContBytes )
{
  // Character mode: a clean 128-byte window made of two-byte UTF-8 (0xC3,
  // 0xA9 = é) followed by a newline. 64 characters in 128 bytes; wc -L
  // counts characters here.
  std::string s;
  s.reserve( 130 );
  for ( int i = 0; i < 64; ++i ) s.append( "\xC3\xA9" );  // 64 'é'
  s.push_back( '\n' );
  LineScan ls;
  maxLineLen( s.data(), s.size(), ls, /*countChars=*/true );
  EXPECT_EQ( ls.maxComplete, 64u );
}

// ---------------------------------------------------------------------------
// Fused `-L -m` scanner: one pass must produce exactly what the two separate
// passes would -- the char-mode longest line (maxLineLen with countChars) and
// the character total (chars()). All three classify a byte as a continuation
// byte the same way and never actually decode UTF-8, so the equality must hold
// for arbitrary byte input, not just well-formed text.
// ---------------------------------------------------------------------------
TEST( MaxLineLenChars, MatchesSeparatePassesKnown )
{
  // "héllo wörld" (11 chars) is the longest line; total = 11 + nl + 5 + nl
  // = 18.
  const std::string s = "h\xC3\xA9llo w\xC3\xB6rld\nshort\n";
  const auto [maxLine, chars] = fusedLineChars( s );
  EXPECT_EQ( maxLine, 11u );
  EXPECT_EQ( chars, 18u );
  EXPECT_EQ( maxLine, charModeMaxLine( s ) );
  EXPECT_EQ( chars, charsStr( s ) );
}

TEST( MaxLineLenChars, CleanWindow128MatchesSeparatePasses )
{
  // Same shape as the characterization test for byte mode, but verifies the
  // fused -L -m path: 200 ASCII bytes + newline, plus a few multibyte chars
  // before the newline so the contAcc lanes get exercised.
  std::string s = std::string( 200, 'x' );
  s.append( "\xC3\xA9\xC3\xA9" );  // 2 'é' (4 bytes, 2 chars)
  s.push_back( '\n' );
  const auto [maxLine, chars] = fusedLineChars( s );
  EXPECT_EQ( maxLine, charModeMaxLine( s ) );
  EXPECT_EQ( chars, charsStr( s ) );
}

TEST( MaxLineLenChars, EmptyAndNoNewline )
{
  EXPECT_EQ( fusedLineChars( "" ), ( std::pair<usize, usize>{ 0u, 0u } ) );
  // No trailing newline: the line is not "complete", so maxLine stays 0, but
  // its characters still count toward the total (5 here).
  const auto [maxLine, chars] = fusedLineChars( "h\xC3\xA9llo" );
  EXPECT_EQ( maxLine, 0u );
  EXPECT_EQ( chars, 5u );
}

TEST( MaxLineLenChars, FuzzMatchesSeparatePasses )
{
  std::mt19937_64 rng( 0xFA5ED );
  std::uniform_int_distribution<int> byteDist( 0, 255 );
  std::uniform_int_distribution<size_t> lenDist( 0, 5000 );

  for ( int iter = 0; iter < 2000; ++iter ) {
    const size_t len = lenDist( rng );
    std::string s( len, '\0' );
    for ( size_t i = 0; i < len; ++i )
      s[i] = static_cast<char>( byteDist( rng ) );

    const usize expMaxLine = charModeMaxLine( s );
    const usize expChars = charsStr( s );

    const auto [maxLine, chars] = fusedLineChars( s );
    EXPECT_EQ( maxLine, expMaxLine ) << "iter=" << iter << " len=" << len;
    EXPECT_EQ( chars, expChars ) << "iter=" << iter << " len=" << len;

    // Split at a random point (state carried) -- must still match.
    std::uniform_int_distribution<size_t> splitDist( 1, len + 1 );
    const auto [mlChunk, ccChunk] =
        fusedLineCharsChunked( s, splitDist( rng ) );
    EXPECT_EQ( mlChunk, expMaxLine ) << "iter=" << iter << " len=" << len;
    EXPECT_EQ( ccChunk, expChars ) << "iter=" << iter << " len=" << len;
  }
}
