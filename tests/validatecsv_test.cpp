/*
 * Copyright Ji Krochmal 2026
 */
#include "validatecsv.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <random>
#include <string>

#include "test_util.h"

using qwctest::refValidateCsv;

namespace {

CsvDialect rfc()
{
  return CsvDialect{};  // ',' '"' RFC-4180 doubled-quote
}

CsvDialect bsl()
{
  CsvDialect d;
  d.backslashEsc = true;
  return d;
}

void expectValid( const std::string& s, const CsvDialect& d )
{
  const CsvVerdict v = validateCsvBuffer( s.data(), s.size(), d );
  EXPECT_TRUE( v.valid ) << "expected valid; got bad row " << v.badRow;
  EXPECT_EQ( v.valid, refValidateCsv( s, d ).valid );
}

void expectBadRow( const std::string& s, const CsvDialect& d, usize row )
{
  const CsvVerdict v = validateCsvBuffer( s.data(), s.size(), d );
  EXPECT_FALSE( v.valid );
  EXPECT_EQ( v.badRow, row );
  const CsvVerdict ref = refValidateCsv( s, d );
  EXPECT_EQ( v.valid, ref.valid );
  EXPECT_EQ( v.badRow, ref.badRow );
}

}  // namespace

TEST( ValidateCsvBuffer, EmptyIsValid )
{
  expectValid( "", rfc() );
}

TEST( ValidateCsvBuffer, SingleRecordIsValid )
{
  expectValid( "a,b,c\n", rfc() );
}

TEST( ValidateCsvBuffer, RectangularIsValid )
{
  expectValid( "a,b,c\n1,2,3\nx,y,z\n", rfc() );
}

TEST( ValidateCsvBuffer, NoFinalNewlineValidated )
{
  expectValid( "a,b\n1,2", rfc() );
}

TEST( ValidateCsvBuffer, RaggedRowReported )
{
  expectBadRow( "a,b,c\n1,2\n", rfc(), 2 );
}

TEST( ValidateCsvBuffer, RaggedFinalNoNewline )
{
  expectBadRow( "a,b\n1,2,3", rfc(), 2 );
}

TEST( ValidateCsvBuffer, QuotedEmbeddedDelimIsContent )
{
  expectValid( "a,\"b,b\",c\n1,2,3\n", rfc() );
}

TEST( ValidateCsvBuffer, QuotedEmbeddedNewlineMergesRecord )
{
  expectValid( "a,\"b\nb\",c\n1,2,3\n", rfc() );
}

TEST( ValidateCsvBuffer, DoubledQuoteIsLiteral )
{
  // a | "b""c" | d  -> 3 fields (the doubled quote is consumed, not a delim).
  expectValid( "a,\"b\"\"c\",d\n1,2,3\n", rfc() );
}

TEST( ValidateCsvBuffer, BackslashEscapedQuote )
{
  // a | "b\"c" | d  -> 3 fields (the escaped quote is content, not a toggle).
  expectValid( "a,\"b\\\"c\",d\n1,2,3\n", bsl() );
}

TEST( ValidateCsvBuffer, BackslashEscapedDelimIsContent )
{
  expectValid( "a\\,b,c\nx,y\n", bsl() );  // row 1 has 1 real delim
}

TEST( ValidateCsvBuffer, CrlfIsValid )
{
  expectValid( "a,b\r\n1,2\r\n", rfc() );
}

TEST( ValidateCsvBuffer, UnterminatedQuoteIsInvalid )
{
  expectBadRow( "a,b\n1,\"2\n", rfc(), 2 );
}

TEST( ValidateCsvBuffer, BlankLineIsRaggedInWideFile )
{
  expectBadRow( "a,b\n\n", rfc(), 2 );
}

TEST( ValidateCsvBuffer, QuotingDisabledTreatsQuoteAsContent )
{
  CsvDialect d;
  d.quoting = false;
  expectValid( "\"a,b\nc,d\n", d );  // 1 delim each row
}

// ---------------------------------------------------------------------------
// Parallel driver: write the buffer to a temp file and validate it through
// validateCsvFile with a tiny bytesPerThread, so chunk boundaries fall mid
// record / mid quoted region / mid backslash run. The parallel verdict and bad
// row must match the sequential reference at every chunk size.
// ---------------------------------------------------------------------------
namespace {

CsvVerdict viaFile( const std::string& s, const CsvDialect& d, usize bpt )
{
  static std::atomic<unsigned> counter{ 0 };
  const std::string path = "/tmp/qwc_vseam_" + std::to_string( getpid() ) +
                           "_" + std::to_string( counter.fetch_add( 1 ) ) +
                           ".csv";
  FILE* f = std::fopen( path.c_str(), "wb" );
  if ( !s.empty() ) std::fwrite( s.data(), 1, s.size(), f );
  std::fclose( f );
  const CsvVerdict v = validateCsvFile( path.c_str(), d, bpt );
  std::remove( path.c_str() );
  return v;
}

void seamCheck( const std::string& s, const CsvDialect& d )
{
  const CsvVerdict want = refValidateCsv( s, d );
  for ( const usize bpt: { usize{ 1 }, usize{ 2 }, usize{ 3 }, usize{ 5 },
                           usize{ 8 }, usize{ 16 } } ) {
    const CsvVerdict got = viaFile( s, d, bpt );
    EXPECT_EQ( got.valid, want.valid ) << "bpt=" << bpt << " s=[" << s << "]";
    if ( !want.valid ) EXPECT_EQ( got.badRow, want.badRow ) << "bpt=" << bpt;
  }
}

}  // namespace

TEST( ValidateCsvParallel, RectangularAllSeams )
{
  seamCheck( "aa,bb,cc\n11,22,33\nxx,yy,zz\n", rfc() );
}

TEST( ValidateCsvParallel, RaggedAcrossSeams )
{
  seamCheck( "aa,bb,cc\n11,22\nxx,yy,zz\n", rfc() );
}

TEST( ValidateCsvParallel, QuotedEmbeddedDelimAcrossSeams )
{
  seamCheck( "aa,\"b,b,b\",cc\n11,22,33\n", rfc() );
}

TEST( ValidateCsvParallel, QuotedEmbeddedNewlineAcrossSeams )
{
  seamCheck( "aa,\"b\nb\nb\",cc\n11,22,33\n", rfc() );
}

TEST( ValidateCsvParallel, UnterminatedQuoteAcrossSeams )
{
  seamCheck( "aa,bb\n11,\"22\n", rfc() );
}

TEST( ValidateCsvParallel, BackslashRunAcrossSeams )
{
  seamCheck( "aa,\"b\\\"x\",cc\n11,22,33\n", bsl() );
}

TEST( ValidateCsvParallel, DoubledQuoteAcrossSeams )
{
  seamCheck( "aa,\"b\"\"b\",cc\n1,2,3\n", rfc() );
}

// ---------------------------------------------------------------------------
// Randomized fuzz: build varied CSVs (both dialects, plain + quoted fields with
// embedded delimiters/newlines/doubled-or-escaped quotes, occasional ragged
// rows and unterminated quotes) and assert the parallel driver agrees with the
// reference at a random small chunk size.
// ---------------------------------------------------------------------------
namespace {

std::string randToken( std::mt19937& rng )
{
  static const char alnum[] = "abcdefABCDEF0123456789";
  std::string t;
  const int n = 1 + static_cast<int>( rng() % 4 );
  for ( int i = 0; i < n; ++i ) t += alnum[rng() % ( sizeof( alnum ) - 1 )];
  return t;
}

// One field's bytes (no surrounding delimiters). Sometimes quoted with embedded
// specials that must NOT change the field count.
std::string randField( std::mt19937& rng, const CsvDialect& d )
{
  if ( !d.quoting || rng() % 2 == 0 ) return randToken( rng );
  std::string inner = randToken( rng );
  switch ( rng() % 4 ) {
    case 0:
      inner += "," + randToken( rng );  // embedded delimiter
      break;
    case 1:
      inner += "\n" + randToken( rng );  // embedded newline
      break;
    case 2:
      inner += "\"\"" + randToken( rng );  // doubled quote (literal ")
      break;
    case 3:
      if ( d.backslashEsc )
        inner += "\\\"" + randToken( rng );  // escaped quote
      break;
    default:
      break;
  }
  return "\"" + inner + "\"";
}

std::string genRandomCsv( std::mt19937& rng, const CsvDialect& d )
{
  const usize fields = 1 + rng() % 5;
  const usize records = 1 + rng() % 25;
  const bool injectRagged = rng() % 4 == 0;
  const usize raggedAt = rng() % records;
  std::string out;
  for ( usize r = 0; r < records; ++r ) {
    usize fc = fields;
    if ( injectRagged && r == raggedAt )
      fc = ( fields > 1 && rng() % 2 ) ? fields - 1 : fields + 1;
    for ( usize c = 0; c < fc; ++c ) {
      if ( c ) out += ',';
      out += randField( rng, d );
    }
    out += '\n';
  }
  if ( rng() % 5 == 0 && !out.empty() && out.back() == '\n' )
    out.pop_back();  // drop the final newline sometimes
  if ( rng() % 20 == 0 ) out += "\"unterminated";  // rare open quote at EOF
  return out;
}

}  // namespace

TEST( ValidateCsvParallel, RandomFuzzMatchesReference )
{
  std::mt19937 rng( 0xC5C0FFEE );
  const usize bpts[] = { 1, 2, 3, 4, 7, 13, 32 };
  for ( int iter = 0; iter < 1200; ++iter ) {
    const CsvDialect d = ( iter & 1 ) ? bsl() : rfc();
    const std::string s = genRandomCsv( rng, d );
    const usize bpt = bpts[rng() % ( sizeof( bpts ) / sizeof( bpts[0] ) )];
    const CsvVerdict want = refValidateCsv( s, d );
    const CsvVerdict got = viaFile( s, d, bpt );
    ASSERT_EQ( got.valid, want.valid )
        << "iter=" << iter << " bpt=" << bpt << " s=[" << s << "]";
    if ( !want.valid )
      ASSERT_EQ( got.badRow, want.badRow )
          << "iter=" << iter << " bpt=" << bpt << " s=[" << s << "]";
  }
}

// ---------------------------------------------------------------------------
// Large multi-chunk inputs that exercise the SIMD kernel across real chunk
// boundaries (not the byte-by-byte seam stress above). The fully-unquoted case
// is the Phase-1-only fast path -- the dominant real-world workload.
// ---------------------------------------------------------------------------
TEST( ValidateCsvNeon, LargeUnquotedRectangular )
{
  std::string s;
  for ( int r = 0; r < 5000; ++r ) s += "alpha,beta,gamma,delta\n";
  for ( const usize bpt:
        { usize{ 64 }, usize{ 256 }, usize{ 4096 }, usize{ 65536 } } )
    EXPECT_TRUE( viaFile( s, rfc(), bpt ).valid ) << "bpt=" << bpt;
}

TEST( ValidateCsvNeon, LargeUnquotedRaggedRowReported )
{
  std::string s;
  for ( int r = 0; r < 5000; ++r ) s += "a,b,c,d\n";
  s += "a,b,c\n";  // row 5001 has one field too few
  for ( const usize bpt: { usize{ 64 }, usize{ 4096 } } )
    EXPECT_EQ( viaFile( s, rfc(), bpt ).badRow, 5001u ) << "bpt=" << bpt;
}

TEST( ValidateCsvNeon, LargeSprinkledQuotesRectangular )
{
  std::string s;
  for ( int r = 0; r < 4000; ++r )
    s += "id,\"free, text\nwith newline\",tag\n";  // 3 fields, quoted middle
  for ( const usize bpt: { usize{ 32 }, usize{ 4096 }, usize{ 65536 } } )
    EXPECT_TRUE( viaFile( s, rfc(), bpt ).valid ) << "bpt=" << bpt;
}
