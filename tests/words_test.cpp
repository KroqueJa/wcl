#include "words.h"

#include <gtest/gtest.h>

#include <random>
#include <string>

#include "iswprint_table.h"
#include "test_util.h"

using qwctest::refWords;
using qwctest::wordsChunked;
using qwctest::wordsStr;

// Word counting, wc semantics: a word is a maximal run of non-separator
// characters containing at least one PRINTABLE character (a run of only
// control bytes / unprintable code points is "barren" and not counted). In the
// C parameterization separators are ASCII whitespace and printables are
// 0x21-0x7E; in the UTF-8 parameterization the separator set adds the unicode
// whitespace GNU wc honours (probe-pinned, see scripts/probe-wc-words.py) and
// printability follows glibc iswprint via the generated table.

namespace {

// Split the scan at byte `at`, continuing one WordScan across both calls with
// the +-3-byte context window processfile provides -- proves any buffer split
// yields the same answer.
usize wordsSplit( const std::string& s, const usize at, const WordsMode m = {} )
{
  WordScan ws;
  const usize ctxEnd = std::min( s.size(), at + 3 );
  words( s.data(), ctxEnd, 0, at, ws, m );
  const usize back = at < 3 ? at : 3;
  words(
      s.data() + at - back, s.size() - ( at - back ), back,
      s.size() - ( at - back ), ws, m
  );
  wordsFlush( ws );
  return ws.words;
}

const WordsMode kUtf8{ true, true };
const WordsMode kUtf8Posix{ true, false };

// Sub-row index helper: ((lead - 0xE0) << 6) | (cont1 & 0x3F). Matches the
// kernel's lookup form and the generator's emission order.
u8 candLead3Cell( u32 lead, u32 c1 )
{
  return kCandLead3[( ( lead - 0xE0u ) << 6 ) | ( c1 & 0x3Fu )];
}

}  // namespace

// ---------------------------------------------------------------------------
// The generated iswprint table reproduces known glibc classifications. These
// pin the table's integrity (and document the surprising ones).
// ---------------------------------------------------------------------------
TEST( IswprintTable, KnownClassifications )
{
  EXPECT_TRUE( qwcIswprint( 'a' ) );
  EXPECT_FALSE( qwcIswprint( 0x01 ) );
  EXPECT_FALSE( qwcIswprint( 0x7F ) );    // DEL
  EXPECT_TRUE( qwcIswprint( 0x00A0 ) );   // NBSP
  EXPECT_TRUE( qwcIswprint( 0x200B ) );   // ZWSP: printable per glibc!
  EXPECT_TRUE( qwcIswprint( 0x4E00 ) );   // CJK
  EXPECT_FALSE( qwcIswprint( 0x03A2 ) );  // unassigned hole in Greek
  EXPECT_TRUE( qwcIswprint( 0x1F600 ) );  // emoji
  EXPECT_FALSE( qwcIswprint( 0xD800 ) );  // surrogate
  EXPECT_FALSE( qwcIswprint( 0x110000 ) );
}

// ---------------------------------------------------------------------------
// kCandLead3: known (lead, cont1) sub-row classifications. Pins the table's
// integrity end-to-end with the generator's contract.
// ---------------------------------------------------------------------------
TEST( CandLead3Table, KnownCells )
{
  // CJK Unified Ideographs: mostly CLEAN. U+4E00 (CJK) lives at E4 cont1=0xB8.
  EXPECT_EQ( candLead3Cell( 0xE4, 0xB8 ), 0u ) << "E4 B8 (CJK U+4E00) clean";
  // Hangul Syllables block: ED cont1=0x80 is CLEAN. 0x9E/0x9F hold unassigned
  // holes (U+D7A4..D7AF), making them DIRTY -- pin a still-CLEAN value.
  EXPECT_EQ( candLead3Cell( 0xED, 0x80 ), 0u ) << "ED 80 (Hangul U+D000) clean";
  EXPECT_EQ( candLead3Cell( 0xED, 0x9D ), 0u ) << "ED 9D (Hangul U+D740) clean";
  EXPECT_EQ( candLead3Cell( 0xED, 0x9E ), 1u ) << "ED 9E (Hangul holes) dirty";
  // Surrogate sub-row: ED cont1=0xA0..0xBF is DIRTY.
  EXPECT_EQ( candLead3Cell( 0xED, 0xA0 ), 1u ) << "ED A0 (surrogates) dirty";
  // Overlong: E0 cont1=0x80..0x9F is DIRTY.
  EXPECT_EQ( candLead3Cell( 0xE0, 0x80 ), 1u ) << "E0 80 (overlong) dirty";
  EXPECT_EQ( candLead3Cell( 0xE0, 0x9F ), 1u ) << "E0 9F (overlong) dirty";
  // Separator-bearing sub-rows are DIRTY in BOTH nbspace modes.
  EXPECT_EQ( candLead3Cell( 0xE3, 0x80 ), 1u ) << "E3 80 (U+3000) dirty";
  EXPECT_EQ( candLead3Cell( 0xE2, 0x80 ), 1u ) << "E2 80 (U+2000 area) dirty";
  EXPECT_EQ( candLead3Cell( 0xE2, 0x81 ), 1u ) << "E2 81 (U+205F area) dirty";
  EXPECT_EQ( candLead3Cell( 0xE1, 0x9A ), 1u ) << "E1 9A (U+1680 ogham) dirty";
  // Devanagari: E0 cont1=0xA4..0xA5 is CLEAN. Pins the test corpus below.
  EXPECT_EQ( candLead3Cell( 0xE0, 0xA4 ), 0u ) << "E0 A4 (Devanagari) clean";
  EXPECT_EQ( candLead3Cell( 0xE0, 0xA5 ), 0u ) << "E0 A5 (Devanagari) clean";
}

// ---------------------------------------------------------------------------
// C parameterization
// ---------------------------------------------------------------------------
TEST( WordsC, Basics )
{
  EXPECT_EQ( wordsStr( "" ), 0u );
  EXPECT_EQ( wordsStr( "hello" ), 1u );
  EXPECT_EQ( wordsStr( "  the quick brown   fox\n" ), 4u );
  EXPECT_EQ( wordsStr( " \t\n\v\f\r " ), 0u );
}

TEST( WordsC, PrintabilityRule )
{
  EXPECT_EQ( wordsStr( " \x01 " ), 0u );   // barren: ctrl only
  EXPECT_EQ( wordsStr( " \x01g " ), 1u );  // rescued by 'g'
  EXPECT_EQ( wordsStr( " \x7F " ), 0u );   // DEL is not printable
  EXPECT_EQ( wordsStr( " \xFF " ), 0u );   // high byte: not printable in C
  EXPECT_EQ(
      wordsStr(
          "a\xFF"
          "b"
      ),
      1u
  );  // ...but word-constituent
}

// ---------------------------------------------------------------------------
// C-mode 16-byte block geometry. These craft inputs that exercise the
// boundaries the nibble kernel introduces (block-edge runs, a separator at the
// top nibble (byte 15), barren runs, carried-in runs). Correctness holds on
// the baseline build too, so they pass with QWC_NEON_NIBBLE off or on.
// ---------------------------------------------------------------------------
TEST( WordsCNibble, BlockGeometryEdges )
{
  const std::string cases[] = {
    std::string( 16, ' ' ),            // full separator block
    std::string( 16, 'a' ),            // full word block (run open at edge)
    std::string( 16, 'a' ) + " b",     // run crosses the 16-byte edge
    std::string( 15, 'a' ) + " b",     // separator exactly at byte 15
    std::string( 17, 'a' ) + " ",      // run ends one byte into block 1
    "ab cd ef gh ij kl mn op",         // many words spanning blocks
    std::string( "\x01\x01\x01", 3 ),  // barren run only -> 0 words
    std::string( "a\x01\x01 b", 5 ),   // run with Other + printable
    std::string( 16, 'a' ) + std::string( 3, '\x01' ) +
        " z",                                    // word|barren|word
    "   leading spaces then words here too   ",  // leading + trailing runs
  };
  const std::size_t chunks[] = { 1, 7, 16, 17, 31, 33 };
  for ( const auto& s: cases ) {
    EXPECT_EQ( wordsStr( s ), refWords( s ) ) << "one-shot: [" << s << "]";
    for ( const std::size_t chunk: chunks )
      EXPECT_EQ( wordsChunked( s, chunk ), refWords( s ) )
          << "chunk=" << chunk << " [" << s << "]";
  }
}

// ---------------------------------------------------------------------------
// UTF-8 parameterization. Non-ASCII characters appear as escaped UTF-8 byte
// sequences so the test data is visible and editor-proof.
// ---------------------------------------------------------------------------
TEST( WordsUtf8, UnicodeSeparators )
{
  EXPECT_EQ(
      wordsStr(
          "a\xC2\xA0"  // U+00A0 NBSP
          "b",
          kUtf8
      ),
      2u
  );
  EXPECT_EQ(
      wordsStr(
          "a\xE3\x80\x80"  // U+3000 ideographic space
          "b",
          kUtf8
      ),
      2u
  );
  EXPECT_EQ(
      wordsStr(
          "a\xE2\x80\x83"  // U+2003 em space
          "b\xE1\x9A\x80"  // U+1680 ogham space
          "c",
          kUtf8
      ),
      3u
  );
  EXPECT_EQ(
      wordsStr(
          "a\xE2\x80\x8B"  // U+200B ZWSP: NOT a separator
          "b",
          kUtf8
      ),
      1u
  );
  EXPECT_EQ(
      wordsStr(
          "a\xE2\x80\xA8"  // U+2028 LS: NOT a separator
          "b",
          kUtf8
      ),
      1u
  );
  EXPECT_EQ(
      wordsStr(
          "a\xC2\x85"  // U+0085 NEL: NOT a separator
          "b",
          kUtf8
      ),
      1u
  );
}

TEST( WordsUtf8, PosixlyCorrectDropsNbspace )
{
  // The probe-pinned nbspace four stop separating: A0, 2007, 202F, 2060.
  EXPECT_EQ(
      wordsStr(
          "a\xC2\xA0"
          "b",
          kUtf8Posix
      ),
      1u
  );
  EXPECT_EQ(
      wordsStr(
          "a\xE2\x80\x87"
          "b",
          kUtf8Posix
      ),
      1u
  );
  EXPECT_EQ(
      wordsStr(
          "a\xE2\x80\xAF"
          "b",
          kUtf8Posix
      ),
      1u
  );
  EXPECT_EQ(
      wordsStr(
          "a\xE2\x81\xA0"
          "b",
          kUtf8Posix
      ),
      1u
  );
  // The base (iswspace) set survives POSIXLY_CORRECT.
  EXPECT_EQ(
      wordsStr(
          "a\xE2\x80\x82"  // U+2002 en space
          "b",
          kUtf8Posix
      ),
      2u
  );
  EXPECT_EQ(
      wordsStr(
          "a\xE3\x80\x80"
          "b",
          kUtf8Posix
      ),
      2u
  );
}

TEST( WordsUtf8, BarrenAndRescuedRuns )
{
  // ZWSP is printable per glibc (probe-confirmed: wc counts it), so a
  // ZWSP-only run IS a word.
  EXPECT_EQ( wordsStr( " \xE2\x80\x8B ", kUtf8 ), 1u );
  EXPECT_EQ( wordsStr( " \x01\x02 ", kUtf8 ), 0u );      // ctrl: barren
  EXPECT_EQ( wordsStr( " \x01x ", kUtf8 ), 1u );         // rescued
  EXPECT_EQ( wordsStr( " \xE4\xB8\x80 ", kUtf8 ), 1u );  // U+4E00 CJK
  EXPECT_EQ( wordsStr( " \xCE\xA2 ", kUtf8 ), 0u );      // U+03A2 unassigned
  EXPECT_EQ( wordsStr( " \xCE\xA2\xCE\xB1 ", kUtf8 ), 1u );  // rescued by alpha
}

TEST( WordsUtf8, InvalidBytesAreWordStuffButNotPrintable )
{
  EXPECT_EQ( wordsStr( " \xFF ", kUtf8 ), 0u );  // invalid-only run: barren
  EXPECT_EQ(
      wordsStr(
          "a\xFF"
          "b",
          kUtf8
      ),
      1u
  );
  EXPECT_EQ( wordsStr( "a\xFF b", kUtf8 ), 2u );
  // Truncated lead at end of input: word-constituent, not printable.
  EXPECT_EQ( wordsStr( " \xE3\x80", kUtf8 ), 0u );
}

TEST( WordsUtf8, MatchesReferenceOnMixedText )
{
  const std::string s =
      "caf\xC3\xA9 au\xE3\x80\x80lait \xE2\x80\x8B \x01 "
      "\xE4\xB8\xAD\xE6\x96\x87 ok\n";
  EXPECT_EQ( wordsStr( s, kUtf8 ), refWords( s, true ) );
}

// Pure-CJK / Hangul / Devanagari word counting: in-block clean 3-byte
// sequences. Equality with refWords proves the vector path stays bit-identical
// to scalar after Task 5; before Task 5 the kernel punts these blocks to
// scalarUtf8 and the equality holds trivially.
TEST( WordsUtf8, PureCjkIdeographs )
{
  // Japanese: 日本語 (U+65E5 U+672C U+8A9E), repeated with ASCII separators.
  // Exercises clean E6/E7/E8 lead sub-rows.
  const std::string s =
      "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E "
      "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E "
      "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
  EXPECT_EQ( wordsStr( s, kUtf8 ), 3u );
  EXPECT_EQ( wordsStr( s, kUtf8 ), refWords( s, true ) );
}

TEST( WordsUtf8, PureHangulSyllables )
{
  // Korean: 안녕하세요 (U+C548 U+B155 U+D558 U+C138 U+C694), repeated.
  // Exercises EC/EB plus the ED cont1<0x9E (Hangul main) sub-rows.
  const std::string s =
      "\xEC\x95\x88\xEB\x85\x95\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94 "
      "\xEC\x95\x88\xEB\x85\x95\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94";
  EXPECT_EQ( wordsStr( s, kUtf8 ), 2u );
  EXPECT_EQ( wordsStr( s, kUtf8 ), refWords( s, true ) );
}

TEST( WordsUtf8, Devanagari )
{
  // नमस्ते (U+0928 U+092E U+0938 U+094D U+0924 U+0947), repeated. Exercises
  // E0 cont1=0xA4..0xA5 sub-rows (Devanagari block).
  const std::string s =
      "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D"
      "\xE0\xA4\xA4\xE0\xA5\x87 "
      "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8";
  EXPECT_EQ( wordsStr( s, kUtf8 ), refWords( s, true ) );
}

TEST( WordsUtf8, OverlongPuntsToScalar )
{
  // E0 80 80 would decode to U+0000; the vector cleanness gate rejects via the
  // E0 cont1=0x80..0x9F overlong sub-row. Output must equal the scalar
  // reference. Padded to >= 64 bytes so the vector loop is entered.
  const std::string s =
      "a\xE0\x80\x80"
      "b c d";
  EXPECT_EQ( wordsStr( s, kUtf8 ), refWords( s, true ) );
  const std::string padded =
      std::string( 40, 'a' ) + s + std::string( 40, 'b' );
  EXPECT_EQ( wordsStr( padded, kUtf8 ), refWords( padded, true ) );
}

TEST( WordsUtf8, SurrogateLeadPuntsToScalar )
{
  // ED A0 80 would encode U+D800. The ED cont1=0xA0..0xBF sub-row is DIRTY.
  const std::string s =
      "a\xED\xA0\x80"
      "b c d";
  EXPECT_EQ( wordsStr( s, kUtf8 ), refWords( s, true ) );
  const std::string padded =
      std::string( 40, 'a' ) + s + std::string( 40, 'b' );
  EXPECT_EQ( wordsStr( padded, kUtf8 ), refWords( padded, true ) );
}

TEST( WordsUtf8, IdeographicSpacePuntsToScalar )
{
  // U+3000 (E3 80 80) lives in a DIRTY sub-row. Padded to force the vector
  // loop; output must equal scalar reference.
  const std::string s =
      std::string( 40, 'a' ) + "\xE3\x80\x80word" + std::string( 40, 'b' );
  EXPECT_EQ( wordsStr( s, kUtf8 ), refWords( s, true ) );
}

// ---------------------------------------------------------------------------
// UTF-8 16-byte block geometry (exercises the nibble kernel's UTF-8 path:
// clean 2-/3-byte sequences and punts straddling the 16-byte edge, hitting
// carryLead2/3a/3b, carryS, carryN, and the i+16/i+17 edge-byte validation).
// Correctness holds on the baseline build too.
// ---------------------------------------------------------------------------
TEST( WordsUtf8Nibble, BlockEdgeSequences )
{
  const std::string seqs[] = {
    "\xC3\xA9",          // U+00E9 e-acute (clean 2-byte)
    "\xD0\xB0",          // U+0430 Cyrillic a (clean 2-byte)
    "\xE4\xB8\x80",      // U+4E00 CJK (clean 3-byte)
    "\xC2\xA0",          // U+00A0 NBSP (2-byte separator -> carryS)
    "\xC2\x80",          // U+0080 C1 control (non-printable -> carryN)
    "\xF0\x9F\x98\x80",  // U+1F600 emoji (4-byte -> punts)
    "\xE3\x80\x80",      // U+3000 ideographic space (dirty 3-byte -> punts)
  };
  const std::size_t chunks[] = { 1, 13, 16, 17, 32 };
  for ( const auto& seq: seqs )
    for ( std::size_t pre = 10; pre <= 18; ++pre ) {
      const std::string s = std::string( pre, 'a' ) + seq + "bc def";
      EXPECT_EQ( wordsStr( s, kUtf8 ), refWords( s, true ) )
          << "pre=" << pre << " one-shot";
      for ( const std::size_t chunk: chunks )
        EXPECT_EQ( wordsChunked( s, chunk, kUtf8 ), refWords( s, true ) )
            << "pre=" << pre << " chunk=" << chunk;
    }
}

TEST( WordsUtf8Nibble, PureMultibyteBlocks )
{
  std::string cjk, cyr;
  for ( int k = 0; k < 40; ++k ) {
    cjk += "\xE4\xB8\x80";  // U+4E00
    cyr += "\xD0\xB0";      // U+0430
  }
  const std::string cases[] = { cjk, cyr, cjk + " " + cyr,
                                "word " + cjk + " word " + cyr + " end" };
  const std::size_t chunks[] = { 1, 16, 17, 48 };
  for ( const auto& s: cases ) {
    EXPECT_EQ( wordsStr( s, kUtf8 ), refWords( s, true ) );
    for ( const std::size_t chunk: chunks )
      EXPECT_EQ( wordsChunked( s, chunk, kUtf8 ), refWords( s, true ) )
          << "chunk=" << chunk;
  }
}

// ---------------------------------------------------------------------------
// Split-independence: any buffer boundary (even mid-sequence) must agree.
// ---------------------------------------------------------------------------
TEST( WordsSplit, EveryBoundaryAgrees )
{
  const std::string s =
      "a\xE3\x80\x80"
      "b \xE2\x80\x8B c\xC2\xA0也 d \x02 caf\xC3\xA9";
  const usize whole = wordsStr( s, kUtf8 );
  EXPECT_EQ( whole, refWords( s, true ) );
  for ( usize at = 0; at <= s.size(); ++at )
    EXPECT_EQ( wordsSplit( s, at, kUtf8 ), whole ) << "split at " << at;
}

TEST( WordsSplit, ChunkedFeedAgreesAcrossSizes )
{
  std::string s;
  std::mt19937 rng( 42 );
  const char* toks[] = { "word", "\xE3\x80\x80", " ",    "\xC2\xA0",
                         "x",    "\xE2\x80\x8B", "\x01", "\xE4\xB8\x80",
                         "\t",   "\xCE\xA2" };
  for ( int i = 0; i < 2000; ++i ) s += toks[rng() % 10];
  const usize whole = wordsStr( s, kUtf8 );
  EXPECT_EQ( whole, refWords( s, true ) );
  for ( usize chunk: { usize( 1 ), usize( 2 ), usize( 3 ), usize( 7 ),
                       usize( 64 ), usize( 4096 ) } )
    EXPECT_EQ( wordsChunked( s, chunk, kUtf8 ), whole ) << "chunk=" << chunk;
}

TEST( WordsSplit, CModeChunkedAgrees )
{
  std::string s;
  std::mt19937 rng( 7 );
  const char* toks[] = { "alpha", " ", "\x01", "\t", "b", "\xFF" };
  for ( int i = 0; i < 3000; ++i ) s += toks[rng() % 6];
  const usize whole = wordsStr( s );
  EXPECT_EQ( whole, refWords( s ) );
  for ( usize chunk: { usize( 1 ), usize( 3 ), usize( 64 ), usize( 1024 ) } )
    EXPECT_EQ( wordsChunked( s, chunk ), whole ) << "chunk=" << chunk;
}

TEST( WordsSplit, Lead3StraddlesBit30 )
{
  // 3-byte CJK lead at offset 30: lead at bit 30, cont1 at bit 31 (in-block),
  // cont2 at bit 32 (lookahead). Exercises the carryLead3a path.
  std::string s( 30, 'a' );
  s += "\xE6\x97\xA5";  // 日 (U+65E5) at offsets 30, 31, 32
  s += std::string( 30, 'b' );
  s += "\xE6\x9C\xAC";  // 本 (no special alignment, sanity check)
  s += " ok";
  const usize whole = wordsStr( s, kUtf8 );
  EXPECT_EQ( whole, refWords( s, true ) );
  for ( usize at = 0; at <= s.size(); ++at )
    EXPECT_EQ( wordsSplit( s, at, kUtf8 ), whole ) << "split at " << at;
}

TEST( WordsSplit, Lead3StraddlesBit31 )
{
  // 3-byte CJK lead at offset 31: lead at bit 31, cont1 at bit 32, cont2 at
  // bit 33. Exercises carryLead3b and the widened i+34<=len lookahead bound.
  std::string s( 31, 'a' );
  s += "\xE6\x97\xA5";  // bytes at offsets 31, 32, 33
  s += std::string( 30, 'b' );
  s += "\xE6\x9C\xAC ok";
  const usize whole = wordsStr( s, kUtf8 );
  EXPECT_EQ( whole, refWords( s, true ) );
  for ( usize at = 0; at <= s.size(); ++at )
    EXPECT_EQ( wordsSplit( s, at, kUtf8 ), whole ) << "split at " << at;
}

TEST( WordsSplit, MixedLead2AndLead3Carries )
{
  // Lead2 at bit 31 alongside Lead3 elsewhere: ensures the widened cont
  // expression (lead2<<1 | lead3<<1 | lead3<<2 | carryC1 | carryC2) doesn't
  // double-account a continuation. NBSP (C2 A0) at offset 31, CJK earlier in
  // the same block.
  std::string s( 10, 'a' );
  s += "\xE6\x97\xA5";          // CJK at offsets 10..12
  s += std::string( 18, 'a' );  // through offset 30
  s += "\xC2\xA0";              // NBSP lead at offset 31, cont at offset 32
  s += "word more";
  const usize whole = wordsStr( s, kUtf8 );
  EXPECT_EQ( whole, refWords( s, true ) );
  for ( usize at = 0; at <= s.size(); ++at )
    EXPECT_EQ( wordsSplit( s, at, kUtf8 ), whole ) << "split at " << at;
}

// ---------------------------------------------------------------------------
// Seam-merge facts reported for the parallel-chunk merge.
// ---------------------------------------------------------------------------
TEST( WordsSeamFacts, LeadingAndTrailingRuns )
{
  WordScan ws;
  const std::string s = "xx yy";
  words( s.data(), s.size(), 0, s.size(), ws, {} );
  EXPECT_TRUE( ws.sawByte );
  EXPECT_TRUE( ws.startsInWord );
  EXPECT_TRUE( ws.leadingEnded );
  EXPECT_TRUE( ws.leadingHasPrintable );
  EXPECT_TRUE( ws.sawSeparator );
  EXPECT_TRUE( ws.inWord );  // "yy" still open
  EXPECT_TRUE( ws.runHasPrintable );
  EXPECT_EQ( ws.words, 1u );  // only "xx" ended
}

TEST( WordsSeamFacts, BarrenLeadingRun )
{
  WordScan ws;
  const std::string s = "\x01\x01 ok";
  words( s.data(), s.size(), 0, s.size(), ws, {} );
  EXPECT_TRUE( ws.startsInWord );
  EXPECT_TRUE( ws.leadingEnded );
  EXPECT_FALSE( ws.leadingHasPrintable );  // the merge may rescue it
  EXPECT_EQ( ws.words, 0u );               // barren run not counted here
}

TEST( WordsSeamFacts, SingleRunSpansWholeRange )
{
  WordScan ws;
  const std::string s = "abcdef";
  words( s.data(), s.size(), 0, s.size(), ws, {} );
  EXPECT_TRUE( ws.startsInWord );
  EXPECT_FALSE( ws.sawSeparator );
  EXPECT_FALSE( ws.leadingEnded );
  EXPECT_TRUE( ws.inWord );
  EXPECT_EQ( ws.words, 0u );
}

TEST( WordsSeamFacts, EmptyRangeReportsNothing )
{
  WordScan ws;
  words( "x", 1, 0, 0, ws, {} );
  EXPECT_FALSE( ws.sawByte );
  EXPECT_EQ( ws.words, 0u );
}
