#pragma once

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Rescue Team Wonder Mail packer.
// Port of the encoder published by Peter O., The Ultimate Pokemon Center, 2007.

static const char kMailChars[] = "?67NPR89F0+.STXY45MCHJ-K12!%3Q#W";

static const uint8_t kWonderCode[24] = {
    0x0C, 0x06, 0x13, 0x08, 0x04, 0x0D, 0x0F, 0x09, 0x10, 0x02, 0x14, 0x12,
    0x00, 0x15, 0x0B, 0x05, 0x17, 0x03, 0x11, 0x0A, 0x01, 0x0E, 0x16, 0x07};

struct MailBits {
  uint8_t buf[16];
  int ptr;
  int bit;
};

static inline void mailEncodeBits(MailBits *state, const uint8_t *src, int start, int count) {
  int bit = 0;
  int pointer = start;
  for (int i = 0; i < count; ++i) {
    uint8_t next = src[pointer];
    if ((next >> bit) & 1) state->buf[state->ptr] |= (uint8_t)(1u << state->bit);
    if (++bit == 8) {
      pointer++;
      bit = 0;
    }
    if (++state->bit == 8) {
      state->ptr++;
      state->bit = 0;
    }
  }
}

static inline void mailDecodeBits(MailBits *state, uint8_t *dst, int start, int count) {
  int bit = 0;
  int pointer = start;
  for (int i = 0; i < count; ++i) {
    if (bit == 0) dst[pointer] = 0;
    if ((state->buf[state->ptr] >> state->bit) & 1) dst[pointer] |= (uint8_t)(1u << bit);
    if (++bit == 8) {
      pointer++;
      bit = 0;
    }
    if (++state->bit == 8) {
      state->ptr++;
      state->bit = 0;
    }
  }
}

static inline void mailWonderBitsRev(MailBits *state, const uint8_t *pass) {
  memset(state->buf, 0, 15);
  mailEncodeBits(state, pass, 0, 4);
  mailEncodeBits(state, pass, 1, 3);
  mailEncodeBits(state, pass, 2, 4);
  mailEncodeBits(state, pass, 0x0C, 9);
  mailEncodeBits(state, pass, 0x0E, 9);
  mailEncodeBits(state, pass, 0x10, 8);
  mailEncodeBits(state, pass, 0x11, 4);
  mailEncodeBits(state, pass, 0x12, 8);
  mailEncodeBits(state, pass, 0x13, 6);
  mailEncodeBits(state, pass, 0x08, 0x18);
  mailEncodeBits(state, pass, 4, 7);
  mailEncodeBits(state, pass, 5, 7);
}

// Writes 24 password characters and a NUL. pass is the 20-byte mission record.
static inline void wonderEncode(const uint8_t pass[20], char out[25]) {
  MailBits packed;
  packed.ptr = 0;
  packed.bit = 0;
  mailWonderBitsRev(&packed, pass);

  uint8_t sum = 0;
  for (int i = 0; i < 0x0E; ++i) sum = (uint8_t)(sum + packed.buf[i] + (i + 1));

  MailBits framed;
  memset(framed.buf, 0, sizeof(framed.buf));
  framed.ptr = 0;
  framed.bit = 0;
  framed.buf[0] = sum;
  for (int i = 1; i < 0x0F; ++i) framed.buf[i] = packed.buf[i - 1];

  uint8_t codes[24];
  memset(codes, 0, sizeof(codes));
  for (int i = 0; i < 0x18; ++i) mailDecodeBits(&framed, codes, i, 5);

  char shuffled[24];
  for (int i = 0; i < 24; ++i) shuffled[i] = kMailChars[codes[i] & 31];

  for (int i = 0; i < 24; ++i) {
    int offset = 0;
    for (int j = 0; j < 24; ++j) {
      if (kWonderCode[j] == i) {
        offset = j;
        break;
      }
    }
    out[i] = shuffled[offset];
  }
  out[24] = 0;
}

static inline void wonderFormat(const char raw[25], char line1[16], char line2[16]) {
  snprintf(line1, 16, "%.4s %.4s %.4s", raw, raw + 4, raw + 8);
  snprintf(line2, 16, "%.4s %.4s %.4s", raw + 12, raw + 16, raw + 20);
}
