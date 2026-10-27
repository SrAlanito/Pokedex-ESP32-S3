#include <Arduino.h>
#include <TJpg_Decoder.h>
#include <TFT_eSPI.h>
#include <math.h>

#include "dex.h"
#include "mail_codec.h"
#include "mail_data.h"

extern "C" {
extern const uint8_t dex_bin_start[] asm("_binary_data_dex_bin_start");
extern const uint8_t dex_bin_end[] asm("_binary_data_dex_bin_end");
}

static constexpr uint8_t kAxisXPin = 4;
static constexpr uint8_t kAxisYPin = 5;
static constexpr uint8_t kButtonPin = 6;
static constexpr uint8_t kSdCsPin = 7;
static constexpr int kDeadZone = 400;
static constexpr int kGenFirst[] = {1, 152, 252, 387};
static constexpr int kGenLast[] = {151, 251, 386, 493};
static const char *kMenuTitle[] = {"Pokedex", "Quien es ese Pokemon?", "Wonder Mail"};
static const char *kMenuSub[] = {"Consulta cada especie", "Adivina la silueta", "Contrasena de rescate"};
static const char *kGenRows[] = {"Completa", "Kanto", "Johto", "Hoenn", "Sinnoh"};
static const char *kGenSpan[] = {"001  -  493", "001  -  151", "152  -  251", "252  -  386", "387  -  493"};
static const char *kStatShort[] = {"PS", "AT", "DF", "AE", "DE", "VE"};
static constexpr int kMenuCount = 3;
static constexpr int kGenCount = 5;

enum Screen : uint8_t { kMenu, kGens, kDex, kWhoPick, kWho, kMail, kMailPick, kMailOut };

TFT_eSPI tft;
static int gCenterX = 2048;
static int gCenterY = 2048;
static bool gButtonWas = false;
static bool gLeftArmed = true;
static bool gVertIdle = true;
static bool gHorizIdle = true;
static uint32_t gVertHold = 0;
static uint32_t gHorizHold = 0;
static uint32_t gVertLast = 0;
static uint32_t gHorizLast = 0;
static uint32_t gClickLock = 0;
static int gNavDelta = 1;

static constexpr uint16_t kRed = 0xA000;
static constexpr uint16_t kBall = 0xF800;
static constexpr uint16_t kPaper = 0xF79D;
static constexpr uint16_t kInk = 0x2104;
static constexpr uint16_t kMuted = 0x6B4D;
static constexpr uint16_t kLine = 0xDEDB;
static constexpr uint16_t kNight = 0x1084;
static constexpr int kArtX = (240 - kArtSide) / 2;
static constexpr int kArtY = 30;

static Screen gScreen = kMenu;
static int gMenu = 0;
static int gGen = 0;
static bool gNational = true;
static int gSpecies = 1;
static bool gMatch = false;
static int gWho = 1;
static bool gRevealed = false;
static bool gWhoGen[4] = {true, true, true, true};
static int gWhoRow = 0;
static int gMailRow = 0;
static int gMailPickKind = 0;
static int gMailPick = 0;
static int gMailPickTop = 0;
static int gDun = 0;
static int gFloor = 1;
static int gItem = 0;
static constexpr int kMailPickVisible = 5;
static int gClient = 1;
static char gPass[25];
static char gPass1[16];
static char gPass2[16];

static int gCacheId[2] = {0, 0};
static uint16_t *gCachePx[2] = {nullptr, nullptr};
static int gCacheHot = 0;
static uint16_t *gDecodeDst = nullptr;
static uint16_t *gFrame = nullptr;

static int readCenter(uint8_t pin) {
  uint32_t sum = 0;
  for (int i = 0; i < 16; ++i) {
    sum += analogRead(pin);
    delay(2);
  }
  return (int)(sum / 16);
}

static int stickDir(int raw, int center) {
  int delta = raw - center;
  if (abs(delta) < kDeadZone) return 0;
  return delta > 0 ? 1 : -1;
}

static void stepInRange(int *value, int delta, int first, int last) {
  int span = last - first + 1;
  int index = *value - first + delta;
  index %= span;
  if (index < 0) index += span;
  *value = first + index;
}

static int dexFirst() { return gNational ? 1 : kGenFirst[gGen - 1]; }
static int dexLast() { return gNational ? kDexCount : kGenLast[gGen - 1]; }

static bool speciesInWhoPool(int id) {
  if (id < 1 || id > kDexCount) return false;
  for (int g = 0; g < 4; ++g) {
    if (gWhoGen[g] && id >= kGenFirst[g] && id <= kGenLast[g]) return true;
  }
  return false;
}

static int whoPoolCount() {
  int n = 0;
  for (int g = 0; g < 4; ++g) {
    if (gWhoGen[g]) n += kGenLast[g] - kGenFirst[g] + 1;
  }
  return n;
}

static int whoRandom() {
  int pool = whoPoolCount();
  if (pool < 1) return 1;
  int pick = random(0, pool);
  for (int g = 0; g < 4; ++g) {
    if (!gWhoGen[g]) continue;
    int span = kGenLast[g] - kGenFirst[g] + 1;
    if (pick < span) return kGenFirst[g] + pick;
    pick -= span;
  }
  return 1;
}

static int whoStep(int id, int dir) {
  for (int i = 0; i < kDexCount; ++i) {
    id += dir;
    if (id > kDexCount) id = 1;
    if (id < 1) id = kDexCount;
    if (speciesInWhoPool(id)) return id;
  }
  return id;
}

static bool jpgBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *data) {
  if (y < 0 || x < 0 || y >= kArtSide || x >= kArtSide) return true;
  if (x + w > kArtSide) w = kArtSide - x;
  if (y + h > kArtSide) h = kArtSide - y;
  for (uint16_t row = 0; row < h; ++row) {
    memcpy(gDecodeDst + (y + row) * kArtSide + x, data + row * w, w * sizeof(uint16_t));
  }
  return true;
}

static uint16_t *ensurePixels(uint16_t **slot) {
  const int bytes = kArtSide * kArtSide * (int)sizeof(uint16_t);
  if (!*slot) *slot = (uint16_t *)ps_malloc(bytes);
  if (!*slot) *slot = (uint16_t *)malloc(bytes);
  return *slot;
}

static uint16_t *artPixels(int id) {
  if (id < 1 || id > kDexCount) id = 1;
  for (int i = 0; i < 2; ++i) {
    if (gCacheId[i] == id && gCachePx[i]) {
      gCacheHot = i;
      return gCachePx[i];
    }
  }
  int slot = gCacheHot ^ 1;
  if (!ensurePixels(&gCachePx[slot])) return nullptr;
  gDecodeDst = gCachePx[slot];
  uint16_t *p = gDecodeDst;
  uint16_t *end = p + kArtSide * kArtSide;
  while (p < end) *p++ = kPaper;
  DexMon mon = dexMon(id);
  JRESULT rc = TJpgDec.drawJpg(0, 0, mon.jpeg, mon.jpegLen);
  if (rc != JDR_OK) Serial.printf("jpeg %d error %d\n", id, (int)rc);
  gCacheId[slot] = id;
  gCacheHot = slot;
  return gCachePx[slot];
}

static void warmAround(int id, int first, int last) {
  int next = id;
  stepInRange(&next, gNavDelta == 0 ? 1 : gNavDelta, first, last);
  if (next != id) artPixels(next);
}

static void blitArt(int x, int y, int id, bool silhouette, int clipCx, int clipCy, int clipR, uint16_t outside,
                    uint16_t inside) {
  uint16_t *src = artPixels(id);
  if (!src || !ensurePixels(&gFrame)) return;
  const uint8_t *mask = dexMon(id).mask;
  const int n = kArtSide * kArtSide;
  if (!clipR) {
    for (int i = 0; i < n; ++i) {
      bool body = mask[i >> 3] & (0x80 >> (i & 7));
      gFrame[i] = !body ? inside : silhouette ? TFT_BLACK : src[i];
    }
  } else {
    const int limit = clipR * clipR;
    for (int py = 0; py < kArtSide; ++py) {
      for (int px = 0; px < kArtSide; ++px) {
        int i = py * kArtSide + px;
        bool body = mask[i >> 3] & (0x80 >> (i & 7));
        int dx = x + px - clipCx;
        int dy = y + py - clipCy;
        bool in = dx * dx + dy * dy <= limit;
        uint16_t color = src[i];
        if (!body) color = in ? inside : outside;
        else if (silhouette) color = TFT_BLACK;
        gFrame[i] = color;
      }
    }
  }
  tft.setSwapBytes(true);
  tft.pushImage(x, y, kArtSide, kArtSide, gFrame);
}

static void drawPokeball(int cx, int cy, int r, int open) {
  if (r < 4) return;
  const int top = cy - open / 2;
  const int bot = cy + (open - open / 2);
  for (int dy = -r; dy <= r; ++dy) {
    float rest = (float)r * r - (float)dy * dy;
    if (rest < 0) continue;
    int span = (int)sqrtf(rest);
    if (span < 1) continue;
    if (dy < 0) {
      int y = top + dy;
      if ((unsigned)y < 280) tft.drawFastHLine(cx - span, y, span * 2, dy >= -3 ? TFT_BLACK : kBall);
    } else if (dy > 0) {
      int y = bot + dy;
      if ((unsigned)y < 280) tft.drawFastHLine(cx - span, y, span * 2, dy <= 3 ? TFT_BLACK : TFT_WHITE);
    }
  }
  int seam = open ? top : cy;
  if ((unsigned)seam < 280) tft.drawFastHLine(cx - r, seam, r * 2, TFT_BLACK);
  if (open >= 3 && (unsigned)bot < 280) tft.drawFastHLine(cx - r, bot, r * 2, TFT_BLACK);
  tft.drawCircle(cx, top, r, TFT_BLACK);
  if (open >= 3) tft.drawCircle(cx, bot, r, TFT_BLACK);
  if (open < 3) {
    int br = r / 4;
    if (br < 3) br = 3;
    tft.fillCircle(cx, cy, br, TFT_BLACK);
    tft.fillCircle(cx, cy, br - 2, TFT_WHITE);
    if (br > 6) tft.fillCircle(cx, cy, br / 3, TFT_BLACK);
  } else {
    tft.fillCircle(cx, cy, 5, TFT_WHITE);
  }
}

static bool buttonDown() { return digitalRead(kButtonPin) == LOW; }

static void drawDevice(const char *title) {
  tft.fillScreen(kPaper);
  tft.fillRect(0, 0, 240, 34, kRed);
  tft.fillRect(0, 272, 240, 8, kRed);
  drawPokeball(18, 17, 9, 0);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, kRed);
  tft.drawString(title && title[0] ? title : "POKEDEX", 34, 9, 2);
}

static void drawMenu() {
  drawDevice("");
  if (gMenu == 0) {
    drawPokeball(120, 100, 46, 0);
  } else if (gMenu == 1) {
    tft.fillCircle(120, 100, 46, kInk);
    tft.drawCircle(120, 100, 50, kRed);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(kPaper, kInk);
    tft.drawString("?", 120, 100, 4);
  } else {
    tft.fillRoundRect(70, 74, 100, 58, 8, TFT_WHITE);
    tft.drawRoundRect(70, 74, 100, 58, 8, kInk);
    tft.drawLine(70, 74, 120, 104, kRed);
    tft.drawLine(170, 74, 120, 104, kRed);
  }
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(kInk, kPaper);
  int titleFont = (gMenu == 1) ? 2 : 4;
  tft.drawString(kMenuTitle[gMenu], 120, gMenu == 1 ? 160 : 168, titleFont);
  tft.setTextColor(kMuted, kPaper);
  tft.drawString(kMenuSub[gMenu], 120, 196, 2);
  int dot0 = 120 - (kMenuCount - 1) * 8;
  for (int i = 0; i < kMenuCount; ++i) {
    tft.fillCircle(dot0 + i * 16, 236, i == gMenu ? 4 : 2, i == gMenu ? kRed : kLine);
  }
  tft.setTextColor(kMuted, kPaper);
  tft.drawString("izq  der      click entra", 120, 258, 1);
}

static void drawGens() {
  drawDevice("Pokedex");
  for (int i = 0; i < kGenCount; ++i) {
    int y = 42 + i * 42;
    bool on = i == gGen;
    uint16_t bg = on ? kRed : kPaper;
    if (on) tft.fillRoundRect(16, y, 208, 38, 8, kRed);
    else tft.drawRoundRect(16, y, 208, 38, 8, kLine);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(on ? TFT_WHITE : kInk, bg);
    tft.drawString(kGenRows[i], 32, y + 10, 2);
    tft.setTextColor(on ? kPaper : kMuted, bg);
    tft.drawString(kGenSpan[i], 32, y + 24, 1);
  }
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(kMuted, kPaper);
  tft.drawString("click abre      izq vuelve", 120, 262, 1);
}

static void drawWhoPick() {
  drawDevice("Quien es ese?");
  for (int i = 0; i < 4; ++i) {
    int y = 44 + i * 40;
    bool on = i == gWhoRow;
    uint16_t bg = on ? TFT_WHITE : kPaper;
    if (on) {
      tft.fillRoundRect(14, y, 212, 36, 8, TFT_WHITE);
      tft.fillRect(14, y + 6, 4, 24, kRed);
    } else {
      tft.drawRoundRect(14, y, 212, 36, 8, kLine);
    }
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(gWhoGen[i] ? kRed : kMuted, bg);
    tft.drawString(gWhoGen[i] ? "[x]" : "[ ]", 28, y + 18, 2);
    tft.setTextColor(kInk, bg);
    tft.drawString(kGenRows[i + 1], 68, y + 12, 2);
    tft.setTextColor(kMuted, bg);
    tft.drawString(kGenSpan[i + 1], 68, y + 26, 1);
  }
  int y = 44 + 4 * 40;
  bool on = gWhoRow == 4;
  uint16_t bg = on ? kRed : kPaper;
  if (on) tft.fillRoundRect(14, y, 212, 36, 8, kRed);
  else tft.drawRoundRect(14, y, 212, 36, 8, kLine);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(on ? TFT_WHITE : kInk, bg);
  tft.drawString("Empezar", 120, y + 18, 2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(kMuted, kPaper);
  tft.drawString("click marca     izq vuelve", 120, 268, 1);
}

static uint16_t typeColor(int type) {
  static const uint16_t colors[] = {TFT_DARKGREY, 0xAD55, 0xF288, 0x4B7F, 0xF6E0, 0x5E48, 0x9DFF, 0xC208,
                                     0xA85A,       0xE4C4, 0x9DDF, 0xF473, 0xAFE5, 0xB4A0, 0x6B7F, 0x7A7B,
                                     0x630C,       0xB596};
  if (type < 1 || type > 17) return TFT_DARKGREY;
  return colors[type];
}

static int drawTypeChip(int x, int y, int type) {
  if (type < 1) return 0;
  const char *name = kTypeEs[type];
  int w = (int)strlen(name) * 6 + 10;
  uint16_t color = typeColor(type);
  tft.fillRoundRect(x, y, w, 14, 3, color);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, color);
  tft.drawString(name, x + 5, y + 3, 1);
  return w;
}

static int drawWrapped(const char *text, int y, uint16_t color) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, kPaper);
  char line[40];
  int len = 0;
  line[0] = 0;
  const char *p = text;
  while (*p && y < tft.height() - 12) {
    while (*p == ' ') ++p;
    if (!*p) break;
    const char *start = p;
    while (*p && *p != ' ') ++p;
    int word = (int)(p - start);
    if (len && len + 1 + word > 34) {
      tft.drawString(line, 12, y, 1);
      y += 12;
      len = 0;
      line[0] = 0;
    }
    if (word > 34) word = 34;
    if (len) line[len++] = ' ';
    memcpy(line + len, start, word);
    len += word;
    line[len] = 0;
  }
  if (len && y < tft.height() - 12) {
    tft.drawString(line, 12, y, 1);
    y += 12;
  }
  return y;
}

static int matchupLine(const char *label, int kind, const DexMon &mon, int y) {
  if (y > tft.height() - 24) return y;
  char body[180];
  int n = 0;
  body[0] = 0;
  for (int attack = 1; attack <= 17; ++attack) {
    int taken = takenFrom(attack, mon.t1, mon.t2);
    bool want = (kind == 0 && taken == 0) || (kind == 1 && taken > 0 && taken < 4) || (kind == 2 && taken > 4);
    if (!want) continue;
    int wrote = snprintf(body + n, sizeof(body) - n, n ? ", %s" : "%s", kTypeEs[attack]);
    if (wrote < 0 || n + wrote >= (int)sizeof(body)) break;
    n += wrote;
  }
  if (!body[0]) snprintf(body, sizeof(body), "-");
  tft.setTextColor(kRed, kPaper);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(label, 12, y, 1);
  return drawWrapped(body, y + 11, kInk) + 2;
}

static void drawDex() {
  DexMon mon = dexMon(gSpecies);
  tft.fillRect(0, 0, 240, kArtY, kRed);
  tft.fillRect(0, kArtY, 240, kArtSide, kPaper);
  tft.fillRect(0, kArtY + kArtSide, 240, 280 - kArtY - kArtSide, kPaper);

  char head[40];
  snprintf(head, sizeof(head), "#%03d  %s", gSpecies, mon.es);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, kRed);
  tft.drawString(head, 8, 7, 2);
  char place[16];
  int first = dexFirst();
  int last = dexLast();
  int count = last - first + 1;
  snprintf(place, sizeof(place), "%d/%d", gSpecies - first + 1, count);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(kPaper, kRed);
  tft.drawString(place, 232, 10, 1);

  blitArt(kArtX, kArtY, gSpecies, false, 0, 0, 0, kPaper, kPaper);

  if (gMatch) {
    tft.fillRect(0, 150, 240, 130, kPaper);
    tft.fillRect(0, 150, 240, 3, kRed);
    int y = 158;
    y = matchupLine("Debil a", 2, mon, y);
    y = matchupLine("Resiste", 1, mon, y);
    matchupLine("Inmune", 0, mon, y);
    tft.setTextDatum(BC_DATUM);
    tft.setTextColor(kMuted, kPaper);
    tft.drawString("click cierra", 120, 278, 1);
    warmAround(gSpecies, first, last);
    return;
  }

  int chipX = 8 + drawTypeChip(8, 250, mon.t1);
  if (mon.t2) drawTypeChip(chipX + 6, 250, mon.t2);
  for (int i = 0; i < 6; ++i) {
    int x = 6 + i * 39;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(kMuted, kPaper);
    tft.drawString(kStatShort[i], x, 268, 1);
    int bar = mon.stats[i] * 22 / 255;
    if (bar < 1) bar = 1;
    uint16_t fill = mon.stats[i] >= 80 ? 0x3A48 : mon.stats[i] >= 50 ? 0xC560 : kRed;
    tft.fillRect(x + 14, 270, 22, 5, kLine);
    tft.fillRect(x + 14, 270, bar, 5, fill);
  }
  warmAround(gSpecies, first, last);
}

static void drawWho() {
  DexMon mon = dexMon(gWho);
  const int cx = 120;
  const int cy = kArtY + kArtSide / 2;
  tft.fillScreen(kNight);
  tft.fillCircle(cx, cy, 100, TFT_WHITE);
  blitArt(kArtX, kArtY, gWho, !gRevealed, cx, cy, 100, kNight, TFT_WHITE);
  tft.drawCircle(cx, cy, 100, 0x5D7F);
  tft.drawCircle(cx, cy, 101, 0x5D7F);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, kNight);
  if (!gRevealed) {
    tft.drawString("Quien es ese Pokemon?", 120, 6, 2);
    tft.setTextColor(0xBDD7, kNight);
    tft.drawString("click revela     arriba otro", 120, 256, 1);
  } else {
    char title[40];
    snprintf(title, sizeof(title), "#%03d  %s", gWho, mon.es);
    tft.drawString(title, 120, 6, 2);
    tft.setTextColor(0xBDD7, kNight);
    tft.drawString("click: otro", 120, 256, 1);
  }
  int warm = whoStep(gWho, gNavDelta == 0 ? 1 : gNavDelta);
  if (warm != gWho) artPixels(warm);
}

static const char *mailLabel(int row) {
  if (row == 0) return "Mazmorra";
  if (row == 1) return "Piso";
  if (row == 2) return "Recompensa";
  return "Generar";
}

static void mailValue(int row, char *buf, int n) {
  if (row == 0) snprintf(buf, n, "%s", kMailDungeons[gDun].name);
  else if (row == 1) snprintf(buf, n, "Piso %d", gFloor);
  else if (row == 2) snprintf(buf, n, "%s", kMailItems[gItem].name);
  else snprintf(buf, n, "Crear contrasena");
}

static void paintMailRow(int row) {
  int y = 44 + row * 52;
  tft.fillRect(8, y - 2, 224, 52, kPaper);
  bool on = row == gMailRow;
  uint16_t bg = on ? TFT_WHITE : kPaper;
  if (on) {
    tft.fillRoundRect(14, y, 212, 46, 8, TFT_WHITE);
    tft.fillRect(14, y + 8, 4, 30, kRed);
  } else {
    tft.drawRoundRect(14, y, 212, 46, 8, kLine);
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(on ? kRed : kMuted, bg);
  tft.drawString(mailLabel(row), 28, y + 6, 1);
  char value[48];
  mailValue(row, value, sizeof(value));
  tft.setTextColor(kInk, bg);
  tft.drawString(value, 28, y + 20, strlen(value) > 20 ? 1 : 2);
}

static void clampFloor() {
  int maxFloor = kMailDungeons[gDun].floors - 1;
  if (maxFloor < 1) maxFloor = 1;
  if (gFloor < 1) gFloor = 1;
  if (gFloor > maxFloor) gFloor = maxFloor;
}

static int mailPickCount() {
  if (gMailPickKind == 0) return kMailDungeonCount;
  if (gMailPickKind == 1) {
    int maxFloor = kMailDungeons[gDun].floors - 1;
    return maxFloor < 1 ? 1 : maxFloor;
  }
  return kMailItemCount;
}

static void mailPickLabel(int index, char *buf, int n) {
  if (gMailPickKind == 0) snprintf(buf, n, "%s", kMailDungeons[index].name);
  else if (gMailPickKind == 1) snprintf(buf, n, "Piso %d", index + 1);
  else snprintf(buf, n, "%s", kMailItems[index].name);
}

static void ensureMailPickVisible() {
  int count = mailPickCount();
  if (gMailPick < 0) gMailPick = 0;
  if (gMailPick >= count) gMailPick = count - 1;
  if (gMailPick < gMailPickTop) gMailPickTop = gMailPick;
  if (gMailPick >= gMailPickTop + kMailPickVisible) gMailPickTop = gMailPick - kMailPickVisible + 1;
  if (gMailPickTop < 0) gMailPickTop = 0;
  int maxTop = count - kMailPickVisible;
  if (maxTop < 0) maxTop = 0;
  if (gMailPickTop > maxTop) gMailPickTop = maxTop;
}

static void openMailPick(int kind) {
  gMailPickKind = kind;
  if (kind == 0) gMailPick = gDun;
  else if (kind == 1) gMailPick = gFloor - 1;
  else gMailPick = gItem;
  gMailPickTop = 0;
  ensureMailPickVisible();
  gScreen = kMailPick;
}

static void drawMail() {
  drawDevice("Wonder Mail");
  for (int i = 0; i < 4; ++i) paintMailRow(i);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(kMuted, kPaper);
  tft.drawString("click abre     izq vuelve", 120, 256, 1);
}

static void drawMailPick() {
  const char *title = mailLabel(gMailPickKind);
  drawDevice(title);
  int count = mailPickCount();
  ensureMailPickVisible();
  for (int i = 0; i < kMailPickVisible; ++i) {
    int index = gMailPickTop + i;
    int y = 44 + i * 40;
    if (index >= count) {
      tft.fillRect(8, y, 224, 40, kPaper);
      continue;
    }
    bool on = index == gMailPick;
    uint16_t bg = on ? kRed : kPaper;
    if (on) tft.fillRoundRect(14, y, 212, 36, 8, kRed);
    else {
      tft.fillRect(8, y, 224, 40, kPaper);
      tft.drawRoundRect(14, y, 212, 36, 8, kLine);
    }
    char text[48];
    mailPickLabel(index, text, sizeof(text));
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(on ? TFT_WHITE : kInk, bg);
    tft.drawString(text, 28, y + 18, strlen(text) > 22 ? 1 : 2);
  }
  char place[24];
  snprintf(place, sizeof(place), "%d / %d", gMailPick + 1, count);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(kMuted, kPaper);
  tft.drawString(place, 120, 250, 1);
  tft.drawString("click elige     izq vuelve", 120, 266, 1);
}

static void makeMail() {
  gClient = kMailClients[random(0, kMailClientCount)].id;
  const char *name = kMailClients[0].name;
  for (int i = 0; i < kMailClientCount; ++i) {
    if (kMailClients[i].id == gClient) {
      name = kMailClients[i].name;
      break;
    }
  }
  int dungeon = kMailDungeons[gDun].id;
  int title = random(0, 15);
  uint8_t options[24];
  int count = 0;
  for (int i = 0; i < 256 && count < 24; ++i) {
    if ((((i + dungeon) & 0xFF) % 15) == title) options[count++] = (uint8_t)i;
  }
  if (!count) options[count++] = 0;

  uint8_t pass[20];
  memset(pass, 0, sizeof(pass));
  pass[0] = 5;
  pass[1] = 0;
  pass[2] = 0;
  pass[4] = (uint8_t)dungeon;
  pass[5] = (uint8_t)gFloor;
  pass[8] = options[random(0, count)];
  pass[9] = (uint8_t)random(0, 256);
  pass[10] = 0xFF;
  pass[12] = (uint8_t)(gClient & 0xFF);
  pass[13] = (uint8_t)((gClient >> 8) & 0xFF);
  pass[14] = pass[12];
  pass[15] = pass[13];
  pass[16] = 9;
  pass[17] = 8;
  pass[18] = (uint8_t)(kMailItems[gItem].id & 0xFF);
  wonderEncode(pass, gPass);
  wonderFormat(gPass, gPass1, gPass2);

  Serial.println();
  Serial.printf("Cliente %s\n", name);
  Serial.printf("%s  piso %d\n", kMailDungeons[gDun].name, gFloor);
  Serial.printf("Recompensa %s + ?\n", kMailItems[gItem].name);
  Serial.println(gPass1);
  Serial.println(gPass2);
  Serial.println(gPass);
}

static const char *clientName() {
  for (int i = 0; i < kMailClientCount; ++i) {
    if (kMailClients[i].id == gClient) return kMailClients[i].name;
  }
  return "?";
}

static void drawMailOut() {
  drawDevice("Wonder Mail");
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(kMuted, kPaper);
  tft.drawString("Cliente", 20, 48, 1);
  tft.setTextColor(kInk, kPaper);
  tft.drawString(clientName(), 20, 62, 2);

  char place[48];
  snprintf(place, sizeof(place), "%s  %d", kMailDungeons[gDun].name, gFloor);
  tft.setTextColor(kMuted, kPaper);
  tft.drawString("Lugar", 20, 96, 1);
  tft.setTextColor(kInk, kPaper);
  tft.drawString(place, 20, 110, 1);

  char reward[48];
  snprintf(reward, sizeof(reward), "%s + ?", kMailItems[gItem].name);
  tft.setTextColor(kMuted, kPaper);
  tft.drawString("Recompensa", 20, 136, 1);
  tft.setTextColor(kRed, kPaper);
  tft.drawString(reward, 20, 150, 1);

  tft.fillRoundRect(16, 178, 208, 52, 8, TFT_WHITE);
  tft.setTextColor(kInk, TFT_WHITE);
  tft.drawString(gPass1, 28, 186, 2);
  tft.drawString(gPass2, 28, 206, 2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(kMuted, kPaper);
  tft.drawString("click otra     izq volver", 120, 248, 1);
}

static void releaseButton() {
  while (buttonDown()) delay(10);
  delay(40);
  gButtonWas = false;
  gClickLock = millis();
}

static bool playIntro() {
  tft.fillScreen(TFT_BLACK);
  for (int y = -30; y <= 112; y += 14) {
    tft.fillScreen(TFT_BLACK);
    drawPokeball(120, y, 40, 0);
    if (buttonDown()) return true;
    delay(18);
  }
  for (int bounce = 10; bounce >= 0; bounce -= 5) {
    tft.fillScreen(TFT_BLACK);
    drawPokeball(120, 112 - bounce, 40, 0);
    if (buttonDown()) return true;
    delay(30);
  }
  delay(160);
  if (buttonDown()) return true;
  for (int open = 0; open <= 36; open += 6) {
    tft.fillScreen(TFT_BLACK);
    drawPokeball(120, 112, 40, open);
    if (buttonDown()) return true;
    delay(28);
  }
  tft.fillScreen(TFT_WHITE);
  delay(70);
  if (buttonDown()) return true;
  tft.fillScreen(TFT_BLACK);
  drawPokeball(120, 74, 32, 0);
  tft.drawFastHLine(36, 118, 168, TFT_WHITE);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("POKEDEX", 120, 132, 4);
  tft.setTextColor(0xBDD7, TFT_BLACK);
  tft.drawString("Kanto    Johto    Hoenn    Sinnoh", 120, 172, 1);
  tft.setTextColor(kMuted, TFT_BLACK);
  tft.drawString("Arte  PokeAPI", 120, 196, 1);
  uint32_t start = millis();
  artPixels(1);
  while (millis() - start < 1500) {
    if (buttonDown()) return true;
    delay(20);
  }
  return false;
}

static void showScreen() {
  if (gScreen == kMenu) drawMenu();
  else if (gScreen == kGens) drawGens();
  else if (gScreen == kDex) drawDex();
  else if (gScreen == kWhoPick) drawWhoPick();
  else if (gScreen == kWho) drawWho();
  else if (gScreen == kMail) drawMail();
  else if (gScreen == kMailPick) drawMailPick();
  else drawMailOut();
}

static int navStep(uint32_t held) {
  if (held > 900) return 5;
  if (held > 400) return 2;
  return 1;
}

static void onClick() {
  gClickLock = millis();
  if (gScreen == kMenu) {
    if (gMenu == 0) {
      gGen = 0;
      gScreen = kGens;
    } else if (gMenu == 1) {
      gWhoRow = 0;
      gScreen = kWhoPick;
    } else {
      gMailRow = 0;
      gScreen = kMail;
    }
    showScreen();
    return;
  }
  if (gScreen == kGens) {
    gNational = (gGen == 0);
    gSpecies = dexFirst();
    gMatch = false;
    gScreen = kDex;
    showScreen();
    return;
  }
  if (gScreen == kDex) {
    gMatch = !gMatch;
    showScreen();
    return;
  }
  if (gScreen == kWhoPick) {
    if (gWhoRow < 4) {
      gWhoGen[gWhoRow] = !gWhoGen[gWhoRow];
      if (whoPoolCount() < 1) gWhoGen[gWhoRow] = true;
      drawWhoPick();
    } else if (whoPoolCount() > 0) {
      gWho = whoRandom();
      gRevealed = false;
      gNavDelta = 1;
      gScreen = kWho;
      showScreen();
    }
    return;
  }
  if (gScreen == kWho) {
    if (!gRevealed) gRevealed = true;
    else {
      int next = whoRandom();
      if (next == gWho && whoPoolCount() > 1) next = whoStep(gWho, 1);
      gWho = next;
      gRevealed = false;
    }
    showScreen();
    return;
  }
  if (gScreen == kMail) {
    if (gMailRow == 3) {
      makeMail();
      gScreen = kMailOut;
      showScreen();
    } else {
      openMailPick(gMailRow);
      showScreen();
    }
    return;
  }
  if (gScreen == kMailPick) {
    if (gMailPickKind == 0) {
      gDun = gMailPick;
      clampFloor();
    } else if (gMailPickKind == 1) {
      gFloor = gMailPick + 1;
    } else {
      gItem = gMailPick;
    }
    gScreen = kMail;
    showScreen();
    return;
  }
  if (gScreen == kMailOut) {
    makeMail();
    showScreen();
  }
}

static void onLeft() {
  if (gScreen == kMenu) return;
  if (gScreen == kGens || gScreen == kWhoPick || gScreen == kMail) {
    gScreen = kMenu;
    showScreen();
    return;
  }
  if (gScreen == kMailPick) {
    gScreen = kMail;
    showScreen();
    return;
  }
  if (gScreen == kDex) {
    if (gMatch) gMatch = false;
    else gScreen = kGens;
    showScreen();
    return;
  }
  if (gScreen == kWho) {
    gScreen = kWhoPick;
    showScreen();
    return;
  }
  if (gScreen == kMailOut) {
    gScreen = kMail;
    showScreen();
  }
}

static void onVert(int dir, uint32_t held) {
  int step = navStep(held);
  if (gScreen == kGens) {
    stepInRange(&gGen, dir, 0, kGenCount - 1);
    drawGens();
  } else if (gScreen == kWhoPick) {
    stepInRange(&gWhoRow, dir, 0, 4);
    drawWhoPick();
  } else if (gScreen == kDex) {
    gNavDelta = dir * step;
    int before = gSpecies;
    stepInRange(&gSpecies, gNavDelta, dexFirst(), dexLast());
    if (gSpecies != before) {
      gMatch = false;
      drawDex();
    }
  } else if (gScreen == kWho) {
    gNavDelta = dir > 0 ? 1 : -1;
    gWho = whoStep(gWho, gNavDelta);
    gRevealed = false;
    drawWho();
  } else if (gScreen == kMail) {
    int next = gMailRow + dir;
    if (next < 0) next = 3;
    if (next > 3) next = 0;
    if (next != gMailRow) {
      int prev = gMailRow;
      gMailRow = next;
      paintMailRow(prev);
      paintMailRow(gMailRow);
    }
  } else if (gScreen == kMailPick) {
    int before = gMailPick;
    stepInRange(&gMailPick, dir * step, 0, mailPickCount() - 1);
    if (gMailPick != before) drawMailPick();
  }
}

static void onHoriz(int dir, uint32_t held) {
  if (gScreen == kMenu) {
    stepInRange(&gMenu, dir, 0, kMenuCount - 1);
    drawMenu();
    return;
  }
  if (gScreen == kDex && dir > 0) {
    int jump = held > 900 ? 30 : held > 400 ? 15 : 10;
    gNavDelta = jump;
    int before = gSpecies;
    stepInRange(&gSpecies, jump, dexFirst(), dexLast());
    if (gSpecies != before) {
      gMatch = false;
      drawDex();
    }
    return;
  }
  if (gScreen == kWho && dir > 0) {
    if (held > 280) return;
    int next = whoRandom();
    if (next == gWho && whoPoolCount() > 1) next = whoStep(gWho, 1);
    gWho = next;
    gRevealed = false;
    gNavDelta = 1;
    drawWho();
    return;
  }
  if (gScreen == kMailPick && dir > 0) {
    int jump = held > 900 ? 20 : held > 400 ? 8 : 5;
    int before = gMailPick;
    stepInRange(&gMailPick, jump, 0, mailPickCount() - 1);
    if (gMailPick != before) drawMailPick();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  pinMode(kSdCsPin, OUTPUT);
  digitalWrite(kSdCsPin, HIGH);
  pinMode(kButtonPin, INPUT_PULLUP);
  analogRead(kAxisXPin);
  analogRead(kAxisYPin);
  analogSetPinAttenuation(kAxisXPin, ADC_11db);
  analogSetPinAttenuation(kAxisYPin, ADC_11db);

  tft.init();
  tft.setRotation(0);
  tft.setTextWrap(false);
  initChart();
  randomSeed(esp_random());
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(jpgBlock);

  if (!dexBind(dex_bin_start)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("dex.bin invalido", 8, 40, 2);
    while (true) delay(1000);
  }

  gCenterX = readCenter(kAxisXPin);
  gCenterY = readCenter(kAxisYPin);
  playIntro();
  releaseButton();
  showScreen();
  Serial.println("Pokedex lista");
}

void loop() {
  uint32_t now = millis();
  bool down = buttonDown();
  int dx = stickDir(analogRead(kAxisXPin), gCenterX);
  int dy = stickDir(analogRead(kAxisYPin), gCenterY);

  if (down && !gButtonWas) onClick();
  gButtonWas = down;
  if (now - gClickLock < 180) {
    dx = 0;
    dy = 0;
  }

  if (gScreen != kMenu) {
    if (dx < 0 && gLeftArmed) onLeft();
    if (dx >= 0) gLeftArmed = true;
    else gLeftArmed = false;
  } else {
    gLeftArmed = true;
  }

  if (dy == 0) {
    gVertIdle = true;
    gVertHold = now;
  } else {
    uint32_t held = now - gVertHold;
    uint32_t gap = held > 900 ? 90 : held > 400 ? 120 : 180;
    if (gVertIdle || now - gVertLast >= gap) {
      gVertIdle = false;
      gVertLast = now;
      onVert(dy, held);
    }
  }

  if (dx == 0) {
    gHorizIdle = true;
    gHorizHold = now;
  } else if (gScreen == kMenu || dx > 0) {
    uint32_t held = now - gHorizHold;
    uint32_t gap = held > 900 ? 90 : held > 400 ? 120 : 180;
    if (gHorizIdle || now - gHorizLast >= gap) {
      gHorizIdle = false;
      gHorizLast = now;
      onHoriz(dx, held);
    }
  }

  delay(10);
}
