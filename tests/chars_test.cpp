#include "chars.h"

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "test_util.h"

using qwctest::charsChunked;
using qwctest::charsStr;
using qwctest::refChars;

// A "character" here is a UTF-8 code point, matching `wc -m` in a UTF-8 locale.
// The implementation counts every byte that is not a continuation byte
// (10xxxxxx), which equals the code-point count for well-formed UTF-8.

// ---------------------------------------------------------------------------
// Basic / edge cases
// ---------------------------------------------------------------------------
TEST( Chars, EmptyBuffer )
{
  EXPECT_EQ( charsStr( "" ), 0u );
}

TEST( Chars, ZeroLengthIgnoresPointer )
{
  const char* garbage = "abc";
  EXPECT_EQ( chars( garbage, 0 ), 0u );
}

TEST( Chars, AsciiCountsLikeBytes )
{
  EXPECT_EQ( charsStr( "hello world" ), 11u );
}

TEST( Chars, AsciiWithNewlines )
{
  EXPECT_EQ( charsStr( "a\nb\nc\n" ), 6u );
}

TEST( Chars, EmbeddedNulCounts )
{
  const char buf[] = { 'a', '\0', 'b' };
  EXPECT_EQ( chars( buf, sizeof( buf ) ), 3u );
}

// ---------------------------------------------------------------------------
// Multibyte UTF-8: a code point is one character regardless of its byte length.
// ---------------------------------------------------------------------------
TEST( Chars, TwoByteCodePoint )
{
  // "é" is U+00E9 -> 0xC3 0xA9 (2 bytes, 1 character).
  EXPECT_EQ( charsStr( "\xC3\xA9" ), 1u );
}

TEST( Chars, ThreeByteCodePoint )
{
  // "☃" snowman U+2603 -> 0xE2 0x98 0x83 (3 bytes, 1 character).
  EXPECT_EQ( charsStr( "\xE2\x98\x83" ), 1u );
}

TEST( Chars, FourByteCodePoint )
{
  // "😀" U+1F600 -> 0xF0 0x9F 0x98 0x80 (4 bytes, 1 character).
  EXPECT_EQ( charsStr( "\xF0\x9F\x98\x80" ), 1u );
}

TEST( Chars, MixedAsciiAndMultibyte )
{
  // "héllo wörld\n" == 12 characters, 14 bytes.
  const std::string s = "h\xC3\xA9llo w\xC3\xB6rld\n";
  EXPECT_EQ( s.size(), 14u );
  EXPECT_EQ( charsStr( s ), 12u );
  EXPECT_EQ( charsStr( s ), refChars( s ) );
}

// ---------------------------------------------------------------------------
// Chunk independence: each byte is classified on its own, so feeding the data
// in pieces -- even splitting a multibyte sequence -- must match a single pass.
// This is the property the per-thread chunking in processFile relies on.
// ---------------------------------------------------------------------------
TEST( Chars, SplitInsideMultibyteDoesNotMiscount )
{
  // Three snowmen (9 bytes, 3 characters); any split must still yield 3.
  const std::string s = "\xE2\x98\x83\xE2\x98\x83\xE2\x98\x83";
  ASSERT_EQ( charsStr( s ), 3u );
  for ( size_t chunk = 1; chunk <= s.size(); ++chunk )
    EXPECT_EQ( charsChunked( s, chunk ), 3u ) << "chunk=" << chunk;
}

TEST( Chars, ChunkedMatchesWholeMixed )
{
  std::string s;
  for ( int i = 0; i < 500; ++i ) s += "a\xC3\xA9z\xE2\x98\x83 ";
  const usize expected = refChars( s );
  ASSERT_EQ( charsStr( s ), expected );
  for ( size_t chunk: { size_t( 1 ), size_t( 2 ), size_t( 3 ), size_t( 5 ),
                        size_t( 7 ), size_t( 16 ), size_t( 64 ) } )
    EXPECT_EQ( charsChunked( s, chunk ), expected ) << "chunk=" << chunk;
}

// ---------------------------------------------------------------------------
// Randomized differential test: build valid UTF-8 from random code points so
// the expected character count is known by construction and matches the
// reference.
// ---------------------------------------------------------------------------
TEST( Chars, FuzzValidUtf8AgainstReference )
{
  std::mt19937_64 rng( 0xC0DECAFE );
  std::uniform_int_distribution<int> nDist( 0, 800 );
  std::uniform_int_distribution<int> cpKind( 0, 3 );

  auto appendCodePoint = []( std::string& s, int kind, std::mt19937_64& r ) {
    switch ( kind ) {
      case 0:
      {  // 1 byte: U+0000..U+007F
        std::uniform_int_distribution<int> d( 0, 0x7F );
        s.push_back( static_cast<char>( d( r ) ) );
        break;
      }
      case 1:
      {  // 2 bytes: U+0080..U+07FF
        std::uniform_int_distribution<int> d( 0x80, 0x7FF );
        const int cp = d( r );
        s.push_back( static_cast<char>( 0xC0 | ( cp >> 6 ) ) );
        s.push_back( static_cast<char>( 0x80 | ( cp & 0x3F ) ) );
        break;
      }
      case 2:
      {  // 3 bytes: U+0800..U+FFFF
        std::uniform_int_distribution<int> d( 0x800, 0xFFFF );
        const int cp = d( r );
        s.push_back( static_cast<char>( 0xE0 | ( cp >> 12 ) ) );
        s.push_back( static_cast<char>( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) );
        s.push_back( static_cast<char>( 0x80 | ( cp & 0x3F ) ) );
        break;
      }
      default:
      {  // 4 bytes: U+10000..U+10FFFF
        std::uniform_int_distribution<int> d( 0x10000, 0x10FFFF );
        const int cp = d( r );
        s.push_back( static_cast<char>( 0xF0 | ( cp >> 18 ) ) );
        s.push_back( static_cast<char>( 0x80 | ( ( cp >> 12 ) & 0x3F ) ) );
        s.push_back( static_cast<char>( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) );
        s.push_back( static_cast<char>( 0x80 | ( cp & 0x3F ) ) );
        break;
      }
    }
  };

  for ( int iter = 0; iter < 2000; ++iter ) {
    const int count = nDist( rng );
    std::string s;
    for ( int i = 0; i < count; ++i ) appendCodePoint( s, cpKind( rng ), rng );

    EXPECT_EQ( charsStr( s ), static_cast<usize>( count ) ) << "iter=" << iter;
    EXPECT_EQ( charsStr( s ), refChars( s ) ) << "iter=" << iter;

    // Split at a random byte (possibly mid-sequence) and verify it still sums.
    std::uniform_int_distribution<size_t> splitDist( 1, s.size() + 1 );
    EXPECT_EQ(
        charsChunked( s, splitDist( rng ) ), static_cast<usize>( count )
    ) << "iter="
      << iter;
  }
}

// ---------------------------------------------------------------------------
// SIMD seam coverage. A vectorized chars() has an alignment prologue, a wide
// main loop and a scalar epilogue; sweeping start offsets and slice lengths
// exercises every boundary so a lane- or seam-handling bug shows up. The oracle
// is chars()'s own byte-definition -- the number of bytes that are NOT UTF-8
// continuation bytes -- computed with a plain loop. Unlike refChars it is
// slice-safe: it does not assume the slice begins on a code-point boundary.
// ---------------------------------------------------------------------------
static usize nonContinuationBytes( const char* p, size_t n )
{
  usize c = 0;
  for ( size_t i = 0; i < n; ++i )
    if ( ( static_cast<unsigned char>( p[i] ) & 0xC0 ) != 0x80 ) ++c;
  return c;
}

TEST( Chars, SimdSeamsAcrossOffsetsAndLengths )
{
  // Mix 1-, 2-, 3- and 4-byte sequences so continuation bytes land at many
  // positions relative to the SIMD lanes.
  std::string buf;
  while ( buf.size() < 512 ) buf += "a\xC3\xA9z\xE2\x98\x83q\xF0\x9F\x98\x80";
  for ( size_t off = 0; off <= 40 && off < buf.size(); ++off )
    for ( size_t len = 0; off + len <= buf.size() && len <= 200; ++len ) {
      const char* p = buf.data() + off;
      EXPECT_EQ( chars( p, len ), nonContinuationBytes( p, len ) )
          << "off=" << off << " len=" << len;
    }
}

TEST( Chars, CrossesAccumulatorDrainBoundary )
{
  // A byte-accumulator SIMD loop drains only periodically (e.g. every 255
  // iterations); exceed that so the drain/refill path runs, with continuation
  // bytes throughout. "a\xC3\xA9" is 3 bytes / 2 characters.
  std::string s;
  const size_t reps =
      15000;  // 45000 bytes, well past any per-lane drain window
  for ( size_t i = 0; i < reps; ++i ) s += "a\xC3\xA9";
  const usize expected = 2 * reps;
  EXPECT_EQ( charsStr( s ), expected );
  EXPECT_EQ( charsStr( s ), refChars( s ) );
}
