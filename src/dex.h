#pragma once

#include <stdint.h>
#include <string.h>

static constexpr uint16_t kTransparent = 0xF81F;
static constexpr int kDexCount = 493;
static constexpr int kArtSide = 216;
static constexpr uint32_t kDexMagic = 0x32584544;

static const uint8_t *gDex = nullptr;
static uint16_t gDexCount = 0;
static uint32_t gNameBytes = 0;

struct DexMon {
  const char *es;
  const char *en;
  uint8_t t1;
  uint8_t t2;
  uint8_t stats[6];
  const uint8_t *jpeg;
  uint32_t jpegLen;
  const uint8_t *mask;
};

static const char *kTypeEs[] = {
    "",        "Normal",    "Fuego",  "Agua",      "Electrico", "Planta",
    "Hielo",   "Lucha",     "Veneno", "Tierra",    "Volador",   "Psiquico",
    "Bicho",   "Roca",      "Fantasma", "Dragon",  "Siniestro", "Acero"};

// Generation 4 chart, attacker x defender. 0 immune, 1 half, 2 normal, 4 double.
// Fairy is absent on purpose: the live PokeAPI chart includes it and would
// mislabel Steel and Dragon for these species.
static uint8_t kChart[18][18];

static void initChart() {
  for (int a = 1; a <= 17; ++a)
    for (int d = 1; d <= 17; ++d) kChart[a][d] = 2;
  const uint8_t rows[][3] = {
      {1, 14, 0},
      {2, 2, 1},  {2, 3, 1},  {2, 5, 4},  {2, 6, 4},  {2, 12, 4}, {2, 13, 1}, {2, 15, 1}, {2, 17, 4},
      {3, 2, 4},  {3, 3, 1},  {3, 5, 1},  {3, 9, 4},  {3, 13, 4}, {3, 15, 1},
      {4, 3, 4},  {4, 4, 1},  {4, 5, 1},  {4, 9, 0},  {4, 10, 4}, {4, 15, 1},
      {5, 2, 1},  {5, 3, 4},  {5, 5, 1},  {5, 8, 1},  {5, 9, 4},  {5, 10, 1}, {5, 12, 1}, {5, 13, 4},
      {5, 15, 1}, {5, 17, 1},
      {6, 2, 1},  {6, 3, 1},  {6, 5, 4},  {6, 6, 1},  {6, 9, 4},  {6, 10, 4}, {6, 15, 4}, {6, 17, 1},
      {7, 1, 4},  {7, 6, 4},  {7, 8, 1},  {7, 10, 1}, {7, 11, 1}, {7, 12, 1}, {7, 13, 4}, {7, 14, 0},
      {7, 16, 4}, {7, 17, 4},
      {8, 5, 4},  {8, 8, 1},  {8, 9, 1},  {8, 13, 1}, {8, 14, 1}, {8, 17, 0},
      {9, 2, 4},  {9, 4, 4},  {9, 5, 1},  {9, 8, 4},  {9, 10, 0}, {9, 12, 1}, {9, 13, 4}, {9, 17, 4},
      {10, 4, 1}, {10, 5, 4}, {10, 7, 4}, {10, 12, 4}, {10, 13, 1}, {10, 17, 1},
      {11, 7, 4}, {11, 8, 4}, {11, 11, 1}, {11, 16, 0}, {11, 17, 1},
      {12, 2, 1}, {12, 5, 4}, {12, 7, 1}, {12, 8, 1}, {12, 10, 1}, {12, 11, 4}, {12, 14, 1}, {12, 16, 4},
      {12, 17, 1},
      {13, 2, 4}, {13, 6, 4}, {13, 7, 1}, {13, 9, 1}, {13, 10, 4}, {13, 12, 4}, {13, 17, 1},
      {14, 1, 0}, {14, 11, 4}, {14, 14, 4}, {14, 16, 1}, {14, 17, 1},
      {15, 15, 4}, {15, 17, 1},
      {16, 7, 1}, {16, 11, 4}, {16, 14, 4}, {16, 16, 1}, {16, 17, 1},
      {17, 2, 1}, {17, 3, 1}, {17, 4, 1}, {17, 6, 4}, {17, 13, 4}, {17, 17, 1},
  };
  for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) kChart[rows[i][0]][rows[i][1]] = rows[i][2];
}

static int takenFrom(int attack, int t1, int t2) {
  int value = kChart[attack][t1];
  value *= t2 ? kChart[attack][t2] : 2;
  return value;
}

static bool dexBind(const uint8_t *start) {
  uint32_t magic = 0;
  uint16_t side = 0;
  memcpy(&magic, start, 4);
  memcpy(&gDexCount, start + 4, 2);
  memcpy(&side, start + 6, 2);
  memcpy(&gNameBytes, start + 8, 4);
  if (magic != kDexMagic || gDexCount != kDexCount || side != kArtSide) return false;
  gDex = start;
  return true;
}

static DexMon dexMon(int id) {
  DexMon mon;
  memset(&mon, 0, sizeof(mon));
  if (!gDex || id < 1 || id > gDexCount) {
    mon.es = mon.en = "?";
    return mon;
  }
  const uint8_t *rec = gDex + 16 + (id - 1) * 12;
  uint16_t esOff = 0;
  uint16_t enOff = 0;
  memcpy(&esOff, rec, 2);
  memcpy(&enOff, rec + 2, 2);
  mon.t1 = rec[4];
  mon.t2 = rec[5];
  memcpy(mon.stats, rec + 6, 6);
  const char *names = (const char *)(gDex + 16 + gDexCount * 12);
  mon.es = names + esOff;
  mon.en = names + enOff;
  const uint8_t *index = (const uint8_t *)names + gNameBytes;
  uint32_t off = 0;
  uint32_t len = 0;
  memcpy(&off, index + (id - 1) * 8, 4);
  memcpy(&len, index + (id - 1) * 8 + 4, 4);
  const uint8_t *art = index + gDexCount * 8;
  mon.jpeg = art + off;
  mon.jpegLen = len;
  mon.mask = mon.jpeg + len;
  return mon;
}
