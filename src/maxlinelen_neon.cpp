/*
 * Copyright Ji Krochmal 2026
 */
#include <arm_neon.h>

#include "maxlinelen.h"

// NEON longest-line scanner -- the vectorized counterpart of maxlinelen_scalar.
//
// Unlike line/word/char counting, the longest line is a segmented-max over the
// gaps between newlines, which carries state (the open run length) and branches
// on newline positions -- not a clean reduction. But the key observation makes
// it vectorize well: a 16-byte block that contains NO newline can neither end
// the current line nor start a new one, so its entire counted length simply
// extends the open run. That bulk case -- the common one for long lines, which
// is exactly when -L is interesting -- is handled with a single compare per 16
// bytes. Only blocks that actually contain a newline drop to the per-byte state
// machine, where the segment boundaries (and prefix/maxComplete updates) live.

// One byte through the scalar longest-line state machine. Shared by the
// alignment prologue, the scalar epilogue, and the per-byte slow path for
// 16-byte blocks that contain a newline.
static inline void scalarStep(
    const unsigned char c, LineScan& s, const bool countChars
)
{
  if ( c == '\n' ) {
    // The first line of the range is its prefix: it may join a line left open
    // by the previous chunk, so the merge needs its length on its own.
    if ( !s.hasNewline ) {
      s.prefixLen = s.cur;
      s.hasNewline = true;
    }
    if ( s.cur > s.maxComplete ) s.maxComplete = s.cur;
    s.cur = 0;  // the newline is not counted; start the next line
  } else if ( countChars && ( c & 0xC0 ) == 0x80 ) {
    // A UTF-8 continuation byte continues the current character; in character
    // mode it does not advance the line length.
  } else {
    ++s.cur;
  }
}

void maxLineLen(
    const char* buffer, const usize length, LineScan& s, const bool countChars
)
{
  const u8* tmp = reinterpret_cast<const u8*>( buffer );
  usize processed = 0;

  // Scalar prologue to a 16-byte boundary, threading the carry state so the
  // vector loop starts aligned.
  while ( processed < length && reinterpret_cast<usize>( tmp ) % 16 != 0 ) {
    scalarStep( *tmp, s, countChars );
    ++tmp;
    ++processed;
  }

  const uint8x16_t newline = vdupq_n_u8( static_cast<u8>( '\n' ) );
  const uint8x16_t contMask = vdupq_n_u8( 0xC0 );
  const uint8x16_t contTag = vdupq_n_u8( 0x80 );

  // Character mode counts the continuation bytes of a run of newline-free
  // blocks into byte lanes (the same overflow-safe trick as chars_neon /
  // count_neon) and folds the run into `cur` only when it ends -- at a newline
  // block, every 255 blocks (before a lane could overflow), or the loop's end.
  // This keeps the hot path free of the per-block horizontal reduction (and the
  // per-block scalar arithmetic it feeds) that the byte path never pays: a run
  // of newline-free blocks all belongs to the same open line, so `cur` only
  // needs the total.
  uint8x16_t contAcc = vdupq_n_u8( 0 );
  usize pendingBytes = 0;
  u32 pendingBlocks = 0;
  const auto flushRun = [&]() {
    if ( pendingBytes != 0 ) {
      s.cur += pendingBytes - vaddlvq_u8( contAcc );  // non-continuation bytes
      contAcc = vdupq_n_u8( 0 );
      pendingBytes = 0;
      pendingBlocks = 0;
    }
  };

  // Main loop: 16 bytes per iteration.
  while ( processed + 16 <= length ) {
    const uint8x16_t v = vld1q_u8( tmp );

    if ( vmaxvq_u8( vceqq_u8( v, newline ) ) != 0 ) {
      // A newline splits this block into segments. Fold any pending
      // newline-free run into `cur` first (the per-byte pass reads it for
      // prefix/maxComplete), then hand the block to the scalar state machine.
      if ( countChars ) flushRun();
      for ( i32 i = 0; i < 16; ++i ) scalarStep( tmp[i], s, countChars );
    } else if ( countChars ) {
      // Newline-free block: add its continuation bytes to the running lanes
      // (vceqq yields 0xFF == -1 per hit, so vsubq adds 1). No reduction here.
      contAcc =
          vsubq_u8( contAcc, vceqq_u8( vandq_u8( v, contMask ), contTag ) );
      pendingBytes += 16;
      if ( ++pendingBlocks == 255 ) flushRun();
    } else {
      s.cur += 16;  // byte mode: the whole block extends the open line
    }

    tmp += 16;
    processed += 16;
  }
  if ( countChars )
    flushRun();  // fold the final run before the scalar epilogue

  // Scalar epilogue for the remaining < 16 bytes.
  while ( processed < length ) {
    scalarStep( *tmp, s, countChars );
    ++tmp;
    ++processed;
  }
}

// One byte through the fused state machine for `-L -m`: it maintains the
// character-mode longest line (like scalarStep with countChars) and, at the
// same time, the running character total. The newline is itself a character but
// does not extend the line; a continuation byte is neither.
static inline void stepBoth(
    const unsigned char c, LineScan& s, usize& charCount
)
{
  if ( c == '\n' ) {
    ++charCount;
    if ( !s.hasNewline ) {
      s.prefixLen = s.cur;
      s.hasNewline = true;
    }
    if ( s.cur > s.maxComplete ) s.maxComplete = s.cur;
    s.cur = 0;
  } else if ( ( c & 0xC0 ) == 0x80 ) {
    // UTF-8 continuation byte: not a character, not a length advance.
  } else {
    ++charCount;
    ++s.cur;
  }
}

void maxLineLenChars(
    const char* buffer, const usize length, LineScan& s, usize& charCount
)
{
  const u8* tmp = reinterpret_cast<const u8*>( buffer );
  usize processed = 0;

  // Scalar prologue to a 16-byte boundary.
  while ( processed < length && reinterpret_cast<usize>( tmp ) % 16 != 0 ) {
    stepBoth( *tmp, s, charCount );
    ++tmp;
    ++processed;
  }

  const uint8x16_t newline = vdupq_n_u8( static_cast<u8>( '\n' ) );
  const uint8x16_t contMask = vdupq_n_u8( 0xC0 );
  const uint8x16_t contTag = vdupq_n_u8( 0x80 );

  // A run of newline-free blocks is one continuous stretch of the open line, so
  // its non-continuation bytes contribute equally to the line length (`cur`)
  // and to the character total. Count continuation bytes into byte lanes and
  // fold the run into BOTH at run end -- one vector pass yields both answers,
  // with no per-block reduction and no second traversal for chars().
  uint8x16_t contAcc = vdupq_n_u8( 0 );
  usize pendingBytes = 0;
  u32 pendingBlocks = 0;
  const auto flushRun = [&]() {
    if ( pendingBytes != 0 ) {
      const usize nonCont = pendingBytes - vaddlvq_u8( contAcc );
      s.cur += nonCont;
      charCount += nonCont;
      contAcc = vdupq_n_u8( 0 );
      pendingBytes = 0;
      pendingBlocks = 0;
    }
  };

  while ( processed + 16 <= length ) {
    const uint8x16_t v = vld1q_u8( tmp );

    if ( vmaxvq_u8( vceqq_u8( v, newline ) ) != 0 ) {
      flushRun();  // realize the run (into cur and charCount) before the
                   // newline
      for ( i32 i = 0; i < 16; ++i ) stepBoth( tmp[i], s, charCount );
    } else {
      contAcc =
          vsubq_u8( contAcc, vceqq_u8( vandq_u8( v, contMask ), contTag ) );
      pendingBytes += 16;
      if ( ++pendingBlocks == 255 ) flushRun();
    }

    tmp += 16;
    processed += 16;
  }
  flushRun();

  // Scalar epilogue for the remaining < 16 bytes.
  while ( processed < length ) {
    stepBoth( *tmp, s, charCount );
    ++tmp;
    ++processed;
  }
}
