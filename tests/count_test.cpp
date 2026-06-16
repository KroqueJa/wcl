#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "countlines.h"
#include "test_util.h"

using qwctest::countStr;
using qwctest::refCount;

// ---------------------------------------------------------------------------
// Basic / edge cases
// ---------------------------------------------------------------------------
TEST( Count, EmptyBuffer )
{
  EXPECT_EQ( countStr( "" ), 0u );
}

TEST( Count, ZeroLengthIgnoresPointer )
{
  // length == 0 must short-circuit before dereferencing the buffer.
  const char* garbage = "abc\n\n\n";
  EXPECT_EQ( count( garbage, 0, '\n' ), 0u );
}

TEST( Count, NullPointerWithZeroLength )
{
  // A common real-world call shape: empty read yields (nullptr-ish, 0).
  EXPECT_EQ( count( nullptr, 0, '\n' ), 0u );
}

TEST( Count, NoTarget )
{
  EXPECT_EQ( countStr( "abcdef" ), 0u );
}

TEST( Count, SingleNewline )
{
  EXPECT_EQ( countStr( "\n" ), 1u );
}

TEST( Count, CountsNewlines )
{
  EXPECT_EQ( countStr( "a\nb\nc\n" ), 3u );
}

TEST( Count, ConsecutiveNewlines )
{
  EXPECT_EQ( countStr( "\n\n\n" ), 3u );
}

TEST( Count, NoTrailingNewline )
{
  EXPECT_EQ( countStr( "a\nb\nc" ), 2u );
}

TEST( Count, OnlyTargets )
{
  EXPECT_EQ( countStr( std::string( 50, '\n' ) ), 50u );
}

// ---------------------------------------------------------------------------
// Length parameter is authoritative (not NUL-termination)
// ---------------------------------------------------------------------------
TEST( Count, RespectsLengthShorterThanBuffer )
{
  const char buf[] = "a\nb\nc\n";          // 3 newlines total
  EXPECT_EQ( count( buf, 3, '\n' ), 1u );  // "a\nb" -> 1
  EXPECT_EQ( count( buf, 4, '\n' ), 2u );  // "a\nb\n" -> 2
}

TEST( Count, CountsEmbeddedNulBytes )
{
  const char buf[] = { 'a', '\0', 'b', '\0', '\0' };
  EXPECT_EQ( count( buf, sizeof( buf ), '\0' ), 3u );
}

TEST( Count, TargetPresentAfterEmbeddedNul )
{
  const char buf[] = { '\n', '\0', '\n' };
  EXPECT_EQ( count( buf, sizeof( buf ), '\n' ), 2u );
}

// ---------------------------------------------------------------------------
// Custom and high-bit targets (plain-char signedness must not matter)
// ---------------------------------------------------------------------------
TEST( Count, CustomTargetComma )
{
  EXPECT_EQ( countStr( "a,b,c,d", ',' ), 3u );
}

TEST( Count, HighByteTarget0xFF )
{
  std::string s;
  for ( int i = 0; i < 100; ++i ) {
    s.push_back( static_cast<char>( 0xFF ) );
    s.push_back( 'x' );
  }
  EXPECT_EQ( countStr( s, static_cast<char>( 0xFF ) ), 100u );
}

TEST( Count, HighByteTarget0x80 )
{
  std::string s( 200, 'a' );
  for ( size_t i = 0; i < s.size(); i += 4 ) s[i] = static_cast<char>( 0x80 );
  EXPECT_EQ(
      countStr( s, static_cast<char>( 0x80 ) ),
      refCount( s, static_cast<char>( 0x80 ) )
  );
}

TEST( Count, EveryByteValueAsTarget )
{
  // Buffer containing each byte value 0..255 exactly four times.
  std::string s;
  for ( int rep = 0; rep < 4; ++rep )
    for ( int v = 0; v < 256; ++v ) s.push_back( static_cast<char>( v ) );

  for ( int v = 0; v < 256; ++v ) {
    const char target = static_cast<char>( v );
    EXPECT_EQ( countStr( s, target ), 4u ) << "byte value " << v;
  }
}

// ---------------------------------------------------------------------------
// SIMD width boundaries: exercise prologue / main loop / epilogue splits.
// Widths cover scalar(1), NEON block(64) and AVX2 block(128) and their pieces.
// ---------------------------------------------------------------------------
TEST( Count, LengthsAroundSimdBoundaries )
{
  const size_t widths[] = { 1,  15, 16, 17, 31,  32,  33,  47,  48,  63,
                            64, 65, 95, 96, 127, 128, 129, 255, 256, 257 };
  for ( size_t len: widths ) {
    std::string all( len, '\n' );
    EXPECT_EQ( countStr( all ), len ) << "all-target len=" << len;

    std::string none( len, 'x' );
    EXPECT_EQ( countStr( none ), 0u ) << "no-target len=" << len;

    // Every other byte is a target.
    std::string alt( len, 'x' );
    size_t expected = 0;
    for ( size_t i = 0; i < len; i += 2 ) {
      alt[i] = '\n';
      ++expected;
    }
    EXPECT_EQ( countStr( alt ), expected ) << "alternating len=" << len;
  }
}

TEST( Count, TargetExactlyAtBoundary )
{
  // Place a single target at each index of a 256-byte buffer.
  for ( size_t pos = 0; pos < 256; ++pos ) {
    std::string s( 256, 'x' );
    s[pos] = '\n';
    EXPECT_EQ( countStr( s ), 1u ) << "single target at pos=" << pos;
  }
}

// ---------------------------------------------------------------------------
// Alignment: the SIMD prologue aligns to 16/32 bytes. Offsetting the start
// pointer by 0..64 sweeps every alignment residue the prologue can hit.
// ---------------------------------------------------------------------------
TEST( Count, UnalignedStartPointer )
{
  const size_t payload = 512;
  for ( size_t off = 0; off < 64; ++off ) {
    std::vector<char> backing( payload + 64, 'x' );
    char* p = backing.data() + off;
    size_t expected = 0;
    for ( size_t i = 0; i < payload; i += 3 ) {
      p[i] = '\n';
      ++expected;
    }
    EXPECT_EQ( count( p, payload, '\n' ), expected ) << "offset=" << off;
  }
}

// ---------------------------------------------------------------------------
// Accumulator-drain correctness: SIMD lanes are byte-wide and only drained
// every 255 iterations. A dense, large all-target buffer would overflow a lane
// if draining were wrong. NEON drains per 255*64 bytes, AVX2 per 255*128.
// ---------------------------------------------------------------------------
TEST( Count, LargeDenseDoesNotOverflowLanes )
{
  for ( size_t len: { size_t( 16320 ), size_t( 32640 ), size_t( 65280 ),
                      size_t( 100000 ), size_t( 262144 ) } ) {
    std::string all( len, '\n' );
    EXPECT_EQ( countStr( all ), len ) << "len=" << len;
  }
}

TEST( Count, LargeBufferSpansManyDrainCycles )
{
  // > 4 NEON drain cycles and > 2 AVX2 drain cycles, mixed density.
  std::string s( 200000, 'x' );
  size_t expected = 0;
  for ( size_t i = 0; i < s.size(); i += 7 ) {
    s[i] = '\n';
    ++expected;
  }
  EXPECT_EQ( countStr( s ), expected );
}

// ---------------------------------------------------------------------------
// Randomized differential test against the reference oracle.
// ---------------------------------------------------------------------------
TEST( Count, FuzzAgainstReference )
{
  std::mt19937_64 rng( 0xC0FFEE );
  std::uniform_int_distribution<int> byteDist( 0, 255 );
  std::uniform_int_distribution<size_t> lenDist( 0, 5000 );

  for ( int iter = 0; iter < 2000; ++iter ) {
    const size_t len = lenDist( rng );
    std::string s( len, '\0' );
    for ( size_t i = 0; i < len; ++i )
      s[i] = static_cast<char>( byteDist( rng ) );

    const char target = static_cast<char>( byteDist( rng ) );
    EXPECT_EQ( countStr( s, target ), refCount( s, target ) )
        << "iter=" << iter << " len=" << len << " target="
        << static_cast<int>( static_cast<unsigned char>( target ) );
  }
}

TEST( Count, FuzzUnalignedAgainstReference )
{
  std::mt19937_64 rng( 0xBADC0DE );
  std::uniform_int_distribution<int> byteDist( 0, 255 );
  std::uniform_int_distribution<size_t> lenDist( 0, 1000 );
  std::uniform_int_distribution<size_t> offDist( 0, 40 );

  for ( int iter = 0; iter < 2000; ++iter ) {
    const size_t len = lenDist( rng );
    const size_t off = offDist( rng );
    std::vector<char> backing( len + 64 );
    for ( auto& c: backing ) c = static_cast<char>( byteDist( rng ) );
    const char target = static_cast<char>( byteDist( rng ) );

    const char* p = backing.data() + off;
    EXPECT_EQ( count( p, len, target ), refCount( p, len, target ) )
        << "iter=" << iter << " len=" << len << " off=" << off;
  }
}
