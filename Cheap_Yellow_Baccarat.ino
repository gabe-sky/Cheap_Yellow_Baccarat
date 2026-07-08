// Cheap Yellow Baccarat — punto banco for the Cheap Yellow Display
// (ESP32-2432S028R, 2-USB variant, ILI9341, no PSRAM)
//
// Full casino rules: 8-deck shoe, standard third-card tableau, Banker pays
// 19:20 (5% commission, rounded down), Tie pays 8:1 and pushes Player/Banker
// bets. Bet with touch chips, deal, watch the reveal, repeat until rich or
// busted.
//
// Turn-based, so everything draws directly to the TFT — no sprite needed.
// Libraries: TFT_eSPI (configured via Setup_CYD.h) + XPT2046_Touchscreen.
// Sound: core tone()/noTone() on the speaker amp (GPIO 26).
// Board: ESP32 Dev Module, default partition scheme, Serial 115200.

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>

#define SPEAKER_PIN 26

#define LED_R 4      // onboard RGB LED, active LOW
#define LED_G 16
#define LED_B 17

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33
#define RAW_X_MIN 200
#define RAW_X_MAX 3700
#define RAW_Y_MIN 240
#define RAW_Y_MAX 3800

SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();

// ---------------------------------------------------------------- Colors ---
#define COL_FELT   0x0304   // dark casino green
#define COL_PANEL  0x18E3   // dark grey control panel
#define COL_GOLD   0xFE40
#define COL_PLAYER 0x34DF   // blue
#define COL_BANKER 0xF186   // red
#define COL_TIE    0x2E8B   // green
#define COL_CARDBK 0x9084   // card-back maroon

// ------------------------------------------------------------------ Types ---
// These must come before the first function definition: the Arduino IDE
// inserts its auto-generated prototypes right above the first function, and
// prototypes referencing Zone/Outcome fail if the types are declared later.
enum Outcome { WIN_PLAYER, WIN_BANKER, WIN_TIE };
enum State   { ST_BETTING, ST_RESULT, ST_GAMEOVER, ST_BUSTED, ST_SETTINGS,
               ST_SCORES, ST_NAME, ST_SCORES_VIEW };
struct Zone  { int x, y, w, h; };

// ------------------------------------------------------------------ Shoe ---
// Cards are 0..51: rank = c % 13 (0=A .. 12=K), suit = c / 13.
#define DECKS      8
#define SHOE_SIZE  (DECKS * 52)
#define CUT_CARD   16       // reshuffle before the hand when fewer remain

uint8_t shoe[SHOE_SIZE];
int shoePos = 0;

void shuffleShoe() {
  for (int i = 0; i < SHOE_SIZE; i++) shoe[i] = i % 52;
  for (int i = SHOE_SIZE - 1; i > 0; i--) {   // Fisher-Yates, hardware RNG
    int j = esp_random() % (i + 1);
    uint8_t t = shoe[i]; shoe[i] = shoe[j]; shoe[j] = t;
  }
  shoePos = 0;
}

uint8_t dealOne() { return shoe[shoePos++]; }

// ------------------------------------------------------------ Game rules ---
uint8_t cardPoints(uint8_t c) {
  int r = c % 13;
  if (r == 0) return 1;        // ace
  if (r <= 8) return r + 1;    // 2..9
  return 0;                    // 10/J/Q/K
}

uint8_t handValue(const uint8_t* cards, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) v += cardPoints(cards[i]);
  return v % 10;
}

uint8_t pCards[3], bCards[3];
int pN = 0, bN = 0;
bool natural = false;

// Deals a complete hand into pCards/bCards following the punto banco tableau.
// Player: draws on 0-5, stands on 6-7. Banker: if Player stood, draws on 0-5;
// otherwise depends on Banker total and the POINT VALUE of Player's third card.
void playHand() {
  pN = bN = 0;
  natural = false;
  pCards[pN++] = dealOne(); bCards[bN++] = dealOne();
  pCards[pN++] = dealOne(); bCards[bN++] = dealOne();
  uint8_t pv = handValue(pCards, 2), bv = handValue(bCards, 2);
  if (pv >= 8 || bv >= 8) { natural = true; return; }

  int p3 = -1;                              // -1 = Player stood
  if (pv <= 5) { pCards[pN++] = dealOne(); p3 = cardPoints(pCards[2]); }

  bool bankerDraws;
  if (p3 < 0) bankerDraws = (bv <= 5);
  else switch (bv) {
    case 0: case 1: case 2: bankerDraws = true;                  break;
    case 3: bankerDraws = (p3 != 8);                             break;
    case 4: bankerDraws = (p3 >= 2 && p3 <= 7);                  break;
    case 5: bankerDraws = (p3 >= 4 && p3 <= 7);                  break;
    case 6: bankerDraws = (p3 == 6 || p3 == 7);                  break;
    default: bankerDraws = false;
  }
  if (bankerDraws) bCards[bN++] = dealOne();
}

// --------------------------------------------------------------- Betting ---
State state = ST_BETTING;

const int CHIPS[] = { 1, 10, 25, 50, 100, 500 };
#define N_CHIPS 6
int chipIdx  = 2;                 // start on $25
int bankroll = 1000;
int betP = 0, betB = 0, betT = 0; // deducted from bankroll as placed
int lastBetP = 0, lastBetB = 0, lastBetT = 0; // previous hand's bet, for REBET

// Death chart: bankroll after every hand of the current life, drawn on the
// bust screen. Within-run only — each bust earns its own fresh shame.
#define RUN_MAX 128
int runHist[RUN_MAX];
int runLen = 0;
int handsThisRun = 0;      // true hand count (survives decimation)
int runPeak = 1000;

void recordRunPoint(int v) {
  if (runLen == RUN_MAX) {   // full: drop every other point, keep the arc
    for (int i = 0; i < RUN_MAX / 2; i++) runHist[i] = runHist[i * 2];
    runLen = RUN_MAX / 2;
  }
  runHist[runLen++] = v;
}

void resetRunStats() {
  runLen = 0;
  handsThisRun = 0;
  runPeak = bankroll;
  recordRunPoint(bankroll);
}

// Bead road: last results, blue=Player red=Banker green=Tie.
#define MAX_BEADS 31
uint8_t beads[MAX_BEADS];
int beadCount = 0;

void pushBead(uint8_t o) {
  if (beadCount == MAX_BEADS) {
    memmove(beads, beads + 1, MAX_BEADS - 1);
    beads[MAX_BEADS - 1] = o;
  } else beads[beadCount++] = o;
}

// -------------------------------------------------------------- Settings ---
Preferences prefs;        // NVS namespace "baccarat", survives reboots
bool soundOn = true;
bool teachOn = false;     // tutor mode: pause and explain each drawing rule

// ------------------------------------------------------------ High scores ---
// Top five cash-outs of all time, persisted in NVS (keys hs0n/hs0v..hs4n/hs4v).
#define NAME_MAX 16       // fits font 4 on the entry line and font 2 list rows
char hsName[5][NAME_MAX + 1];
int  hsVal[5] = { 0, 0, 0, 0, 0 };
int  hsNew = -1;          // freshly inserted row, highlighted on the view screen
int  cashPending = 0;     // amount being cashed out while the name is entered
char nameBuf[NAME_MAX + 1];
int  nameLen = 0;

void loadScores() {
  char k[8];
  for (int i = 0; i < 5; i++) {
    snprintf(k, sizeof k, "hs%dv", i);
    hsVal[i] = prefs.getInt(k, 0);
    snprintf(k, sizeof k, "hs%dn", i);
    String s = prefs.getString(k, "");
    strncpy(hsName[i], s.c_str(), NAME_MAX);
    hsName[i][NAME_MAX] = 0;
  }
}

void saveScores() {
  char k[8];
  for (int i = 0; i < 5; i++) {
    snprintf(k, sizeof k, "hs%dv", i);
    prefs.putInt(k, hsVal[i]);
    snprintf(k, sizeof k, "hs%dn", i);
    prefs.putString(k, hsName[i]);
  }
}

// Returns the row the score landed in, or -1 if it doesn't make the table.
int insertScore(const char* nm, int v) {
  int pos = -1;
  for (int i = 0; i < 5; i++) if (v > hsVal[i]) { pos = i; break; }
  if (pos < 0) return -1;
  for (int i = 4; i > pos; i--) {
    hsVal[i] = hsVal[i - 1];
    strcpy(hsName[i], hsName[i - 1]);
  }
  hsVal[pos] = v;
  strncpy(hsName[pos], nm, NAME_MAX);
  hsName[pos][NAME_MAX] = 0;
  return pos;
}

// ---------------------------------------------------------------- Layout ---
#define TABLE_Y  32
#define TABLE_H  122          // felt: y 32..153
#define PANEL_Y  156
#define CARD_W   40
#define CARD_H   56
#define CARD_Y   54

int cardX(bool banker, int i) { return (banker ? 174 : 14) + i * 46; }
int sideCX(bool banker)       { return banker ? 240 : 80; }

const Zone Z_BETP  = {   2, 158, 100, 40 };
const Zone Z_BETT  = { 110, 158, 100, 40 };
const Zone Z_BETB  = { 218, 158, 100, 40 };
const Zone Z_MINUS = {   2, 202,  30, 34 };
const Zone Z_PLUS  = {  88, 202,  30, 34 };
const Zone Z_CLEAR = { 128, 202,  80, 34 };
const Zone Z_DEAL  = { 216, 202, 102, 34 };
const Zone Z_TITLE = {   0,   0, 110, 18 };  // hidden: tap "BACCARAT" for settings
const Zone Z_SOUND = {  60,  66, 200, 40 };  // settings screen
const Zone Z_TEACH = {  60, 116, 200, 40 };
const Zone Z_DONE  = {  60, 186, 200, 40 };
const Zone Z_BANK  = { 230,   0,  90, 18 };  // bankroll tap -> high scores
const Zone Z_CASH  = {  30, 186, 120, 40 };  // high-score screen buttons
const Zone Z_CANCL = { 170, 186, 120, 40 };

// Name-entry keyboard: 7x4 grid, cells 0-25 = A-Z, 26 = DEL, 27 = OK.
#define KB_X0 6
#define KB_Y0 64
#define KB_CW 44
#define KB_CH 34

bool inZone(const Zone& z, int x, int y) {
  return x >= z.x && x < z.x + z.w && y >= z.y && y < z.y + z.h;
}

// ------------------------------------------------------------------- LED ---
void ledSet(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}
void ledOff() { ledSet(false, false, false); }

// ----------------------------------------------------------------- Sound ---
void sChip()  { if (soundOn) tone(SPEAKER_PIN, 1600, 20); }
void sClick() { if (soundOn) tone(SPEAKER_PIN, 1000, 15); }

// Delays run even when muted so hand pacing doesn't change with the setting.
void playSeq(const int* f, int n, int ms) {
  for (int i = 0; i < n; i++) {
    if (soundOn) tone(SPEAKER_PIN, f[i], ms - 20);
    delay(ms);
  }
  noTone(SPEAKER_PIN);
}
void sWin()  { const int f[] = { 523, 659, 784, 1047 }; playSeq(f, 4, 120); }
void sLose() { const int f[] = { 330, 262, 196 };       playSeq(f, 3, 160); }
void sTie()  { const int f[] = { 440, 440 };            playSeq(f, 2, 140); }
// Sad trombone for the bust screen: three descending womps, then a long
// final note with a mournful wobble. Uses continuous tone() for the slides.
void sBust() {
  if (!soundOn) return;
  const int womp[] = { 294, 277, 262, 233 };
  for (int n = 0; n < 4; n++) {
    bool last = (n == 3);
    for (int f = womp[n] + 25; f >= womp[n]; f -= 5) {  // slide down into it
      tone(SPEAKER_PIN, f);
      delay(18);
    }
    delay(last ? 100 : 160);
    if (last) {
      for (int i = 0; i < 6; i++) {                     // womppppp...
        tone(SPEAKER_PIN, womp[n] - 8); delay(70);
        tone(SPEAKER_PIN, womp[n]);     delay(70);
      }
    }
    noTone(SPEAKER_PIN);
    delay(60);
  }
}

void sShuffle() {
  for (int i = 0; i < 8; i++) {
    if (soundOn) tone(SPEAKER_PIN, 700 + (esp_random() % 600), 18);
    delay(35);
  }
}

// --------------------------------------------------------- Card graphics ---
const char* RANKS[13] = { "A","2","3","4","5","6","7","8","9","10","J","Q","K" };

// suit: 0=spades 1=hearts 2=clubs 3=diamonds
void drawSuitGlyph(int cx, int cy, int suit, uint16_t col) {
  switch (suit) {
    case 3:  // diamond
      tft.fillTriangle(cx, cy - 8, cx - 6, cy, cx + 6, cy, col);
      tft.fillTriangle(cx, cy + 8, cx - 6, cy, cx + 6, cy, col);
      break;
    case 1:  // heart
      tft.fillCircle(cx - 3, cy - 3, 4, col);
      tft.fillCircle(cx + 3, cy - 3, 4, col);
      tft.fillTriangle(cx - 7, cy - 1, cx + 7, cy - 1, cx, cy + 8, col);
      break;
    case 0:  // spade
      tft.fillTriangle(cx - 7, cy + 1, cx + 7, cy + 1, cx, cy - 8, col);
      tft.fillCircle(cx - 3, cy + 2, 4, col);
      tft.fillCircle(cx + 3, cy + 2, 4, col);
      tft.fillRect(cx - 1, cy + 2, 3, 7, col);
      break;
    default: // club
      tft.fillCircle(cx, cy - 4, 4, col);
      tft.fillCircle(cx - 4, cy + 2, 4, col);
      tft.fillCircle(cx + 4, cy + 2, 4, col);
      tft.fillRect(cx - 1, cy + 2, 3, 7, col);
  }
}

void drawCardFace(int x, int y, uint8_t c) {
  int r = c % 13, s = c / 13;
  uint16_t col = (s == 1 || s == 3) ? TFT_RED : TFT_BLACK;
  tft.fillRoundRect(x, y, CARD_W, CARD_H, 4, TFT_WHITE);
  tft.drawRoundRect(x, y, CARD_W, CARD_H, 4, TFT_DARKGREY);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(col, TFT_WHITE);
  tft.drawString(RANKS[r], x + 4, y + 3, 2);
  drawSuitGlyph(x + CARD_W / 2, y + 38, s, col);
}

void drawCardBack(int x, int y) {
  tft.fillRoundRect(x, y, CARD_W, CARD_H, 4, TFT_WHITE);
  tft.fillRoundRect(x + 3, y + 3, CARD_W - 6, CARD_H - 6, 3, COL_CARDBK);
  tft.drawRoundRect(x, y, CARD_W, CARD_H, 4, TFT_DARKGREY);
}

// ------------------------------------------------------------- Chrome UI ---
void drawHeader() {
  tft.fillRect(0, 0, 320, 17, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_GOLD, TFT_BLACK);
  tft.drawString("BACCARAT", 2, 1, 2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Shoe " + String(SHOE_SIZE - shoePos), 160, 1, 2);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("$" + String(bankroll), 318, 1, 2);
}

void drawBeadRoad() {
  tft.fillRect(0, 18, 320, TABLE_Y - 18, TFT_BLACK);
  for (int i = 0; i < beadCount; i++) {
    uint16_t c = beads[i] == WIN_PLAYER ? COL_PLAYER
               : beads[i] == WIN_BANKER ? COL_BANKER : COL_TIE;
    tft.fillCircle(6 + i * 10, 24, 4, c);
  }
}

// n = cards revealed so far, NOT pN/bN — the whole hand is dealt in
// playHand() before the reveal starts, so the live counts would leak the
// final total on the first flip.
void drawTotal(bool banker, int n) {
  int cx = sideCX(banker);
  tft.fillRect(cx - 32, 116, 64, 34, COL_FELT);
  if (n == 0) return;
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, COL_FELT);
  tft.drawString(String(handValue(banker ? bCards : pCards, n)), cx, 132, 4);
}

void drawTable() {
  tft.fillRect(0, TABLE_Y, 320, TABLE_H, COL_FELT);
  tft.drawFastVLine(160, TABLE_Y + 4, TABLE_H - 8, COL_GOLD);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_PLAYER, COL_FELT);
  tft.drawString("PLAYER", 80, 42, 2);
  tft.setTextColor(COL_BANKER, COL_FELT);
  tft.drawString("BANKER", 240, 42, 2);
}

void highlightWinner(Outcome o) {
  if (o == WIN_TIE) {
    tft.drawRoundRect(sideCX(false) - 32, 116, 64, 34, 5, COL_GOLD);
    tft.drawRoundRect(sideCX(true) - 32, 116, 64, 34, 5, COL_GOLD);
  } else {
    tft.drawRoundRect(sideCX(o == WIN_BANKER) - 32, 116, 64, 34, 5, COL_GOLD);
  }
}

// ----------------------------------------------------------- Bet panel UI ---
void drawButton(const Zone& z, const char* label, uint16_t border, uint16_t txt) {
  tft.fillRoundRect(z.x, z.y, z.w, z.h, 5, COL_PANEL);
  tft.drawRoundRect(z.x, z.y, z.w, z.h, 5, border);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(txt, COL_PANEL);
  tft.drawString(label, z.x + z.w / 2, z.y + z.h / 2, 2);
}

void drawBetZone(const Zone& z, const char* name, int amount, uint16_t col) {
  tft.fillRoundRect(z.x, z.y, z.w, z.h, 5, COL_PANEL);
  tft.drawRoundRect(z.x, z.y, z.w, z.h, 5, col);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(col, COL_PANEL);
  tft.drawString(name, z.x + z.w / 2, z.y + 11, 2);
  tft.setTextColor(amount ? TFT_WHITE : TFT_DARKGREY, COL_PANEL);
  tft.drawString("$" + String(amount), z.x + z.w / 2, z.y + 29, 2);
}

void drawChipValue() {
  tft.fillRect(34, 202, 52, 34, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_GOLD, TFT_BLACK);
  tft.drawString("$" + String(CHIPS[chipIdx]), 60, 219, 2);
}

void drawDealButton() {
  bool on = (betP + betB + betT) > 0;
  tft.fillRoundRect(Z_DEAL.x, Z_DEAL.y, Z_DEAL.w, Z_DEAL.h, 5,
                    on ? TFT_DARKGREEN : COL_PANEL);
  tft.drawRoundRect(Z_DEAL.x, Z_DEAL.y, Z_DEAL.w, Z_DEAL.h, 5,
                    on ? TFT_GREEN : TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(on ? TFT_WHITE : TFT_DARKGREY, on ? TFT_DARKGREEN : COL_PANEL);
  tft.drawString("DEAL", Z_DEAL.x + Z_DEAL.w / 2, Z_DEAL.y + Z_DEAL.h / 2, 4);
}

// One button, two jobs: CLEAR while chips are down, REBET when the table is
// empty and last hand's bet can be repeated (greyed if unaffordable).
void drawClearButton() {
  int total = betP + betB + betT;
  int lastTotal = lastBetP + lastBetB + lastBetT;
  if (total == 0 && lastTotal > 0) {
    bool ok = bankroll >= lastTotal;
    drawButton(Z_CLEAR, "REBET", ok ? COL_GOLD : TFT_DARKGREY,
               ok ? COL_GOLD : TFT_DARKGREY);
  } else {
    drawButton(Z_CLEAR, "CLEAR", total ? TFT_LIGHTGREY : TFT_DARKGREY,
               total ? TFT_WHITE : TFT_DARKGREY);
  }
}

void drawBetPanel() {
  tft.fillRect(0, PANEL_Y, 320, 240 - PANEL_Y, TFT_BLACK);
  drawBetZone(Z_BETP, "PLAYER", betP, COL_PLAYER);
  drawBetZone(Z_BETT, "TIE 8:1", betT, COL_TIE);
  drawBetZone(Z_BETB, "BANKER", betB, COL_BANKER);
  drawButton(Z_MINUS, "-", TFT_LIGHTGREY, TFT_WHITE);
  drawButton(Z_PLUS,  "+", TFT_LIGHTGREY, TFT_WHITE);
  drawClearButton();
  drawChipValue();
  drawDealButton();
}

void drawResultPanel(const char* headline, uint16_t col, int net, bool gameOver) {
  tft.fillRect(0, PANEL_Y, 320, 240 - PANEL_Y, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(col, TFT_BLACK);
  tft.drawString(headline, 160, 172, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String line = net > 0 ? "You win $" + String(net)
              : net < 0 ? "You lose $" + String(-net) : "Push";
  tft.drawString(line, 160, 200, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(gameOver ? "Out of chips - tap to continue"
                          : "Tap to continue", 160, 224, 2);
}

// ------------------------------------------------------- Settings screen ---
void drawToggle(const Zone& z, const char* name, bool on) {
  uint16_t col = on ? TFT_GREEN : COL_BANKER;
  tft.fillRoundRect(z.x, z.y, z.w, z.h, 5, COL_PANEL);
  tft.drawRoundRect(z.x, z.y, z.w, z.h, 5, col);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(col, COL_PANEL);
  tft.drawString(String(name) + (on ? ": ON" : ": OFF"),
                 z.x + z.w / 2, z.y + z.h / 2, 4);
}

void drawSettingsScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_GOLD, TFT_BLACK);
  tft.drawString("SETTINGS", 160, 30, 4);
  drawToggle(Z_SOUND, "SOUND", soundOn);
  drawToggle(Z_TEACH, "TUTOR", teachOn);
  tft.fillRoundRect(Z_DONE.x, Z_DONE.y, Z_DONE.w, Z_DONE.h, 5, TFT_DARKGREEN);
  tft.drawRoundRect(Z_DONE.x, Z_DONE.y, Z_DONE.w, Z_DONE.h, 5, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("DONE", Z_DONE.x + Z_DONE.w / 2, Z_DONE.y + Z_DONE.h / 2, 4);
}

// One informative line about the run that just ended, most notable fact first.
void runSummary(char* buf, size_t n) {
  if (hsVal[0] > 0 && runPeak > hsVal[0])
    snprintf(buf, n, "Peak $%d was past the high score.", runPeak);
  else if (hsVal[4] > 0 && runPeak > hsVal[4])
    snprintf(buf, n, "Peak $%d would have made the top five.", runPeak);
  else if (runPeak >= 2500)
    snprintf(buf, n, "Peak bankroll this run: $%d.", runPeak);
  else if (handsThisRun == 1)
    snprintf(buf, n, "Busted on the first hand.");
  else
    snprintf(buf, n, "This run lasted %d hands.", handsThisRun);
}

// Death-chart plot area, shared by drawBustScreen and its threshold lines.
#define CHART_X 30
#define CHART_Y 48
#define CHART_W 260
#define CHART_H 100

// Dashed score line across the chart, skipped if above the plot ceiling.
// label may be nullptr for an unlabeled context line.
void drawChartLevel(int v, const char* label, uint16_t col, bool labelLeft,
                    int maxV) {
  int ry = CHART_Y + CHART_H - (long)v * CHART_H / maxV;
  if (ry <= CHART_Y + 2) return;
  for (int x = CHART_X; x < CHART_X + CHART_W; x += 8)
    tft.drawFastHLine(x, ry, 4, col);
  if (!label) return;
  tft.setTextDatum(labelLeft ? ML_DATUM : MR_DATUM);
  tft.setTextColor(col, TFT_BLACK);
  tft.drawString(String(label) + " $" + String(v),
                 labelLeft ? CHART_X : CHART_X + CHART_W,
                 ry < CHART_Y + 14 ? ry + 10 : ry - 9, 2);
}

void drawBustScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_GOLD, TFT_BLACK);
  tft.drawString("OUT OF CHIPS", 160, 18, 4);

  // The death chart: the whole run, $0-terminated.
  const int cx0 = CHART_X, cy0 = CHART_Y, cw = CHART_W, ch = CHART_H;
  tft.drawFastHLine(cx0, cy0 + ch, cw, TFT_DARKGREY);   // the floor: $0
  int peakI = 0;
  for (int i = 1; i < runLen; i++) if (runHist[i] > runHist[peakI]) peakI = i;
  int maxV = runHist[peakI] > 1000 ? runHist[peakI] : 1000;
  int px = -1, py = -1;
  for (int i = 0; i < runLen; i++) {
    int x = cx0 + (runLen > 1 ? (long)i * cw / (runLen - 1) : 0);
    int y = cy0 + ch - (long)runHist[i] * ch / maxV;
    if (px >= 0) {
      tft.drawLine(px, py, x, y, COL_GOLD);
      tft.drawLine(px, py + 1, x, y + 1, COL_GOLD);     // 2px for visibility
    }
    px = x; py = y;
  }
  tft.fillCircle(px, py, 3, COL_BANKER);                // the moment of death

  // Leaderboard altitudes the run climbed past: 2nd-4th as unlabeled grey
  // context (drawn first, so labeled lines paint over shared values), then
  // the labeled high score and fifth-place lines.
  for (int i = 1; i <= 3; i++)
    if (hsVal[i] > 0)
      drawChartLevel(hsVal[i], nullptr, TFT_LIGHTGREY, false, maxV);
  if (hsVal[0] > 0 && runPeak > hsVal[0])
    drawChartLevel(hsVal[0], "high score", COL_PLAYER, false, maxV);
  if (hsVal[4] > 0 && runPeak > hsVal[4] && hsVal[4] != hsVal[0])
    drawChartLevel(hsVal[4], "fifth", COL_TIE, true, maxV);

  if (runHist[peakI] > 1000) {                          // mark peak glory
    int peakX = cx0 + (runLen > 1 ? (long)peakI * cw / (runLen - 1) : 0);
    tft.fillCircle(peakX, cy0, 3, TFT_WHITE);
    tft.setTextDatum(peakX > 240 ? MR_DATUM : ML_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("$" + String(runHist[peakI]),
                   peakX > 240 ? peakX - 8 : peakX + 8, cy0 + 2, 2);
  }

  char line[48];
  runSummary(line, sizeof line);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(line, 160, 188, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Tap to rebuy for $1000", 160, 222, 2);
}

// Full redraw of the betting screen (boot, leaving settings, bust restart).
void drawMainScreen() {
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  drawBeadRoad();
  drawTable();
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, COL_FELT);
  tft.drawString("Place your bets", 160, 90, 2);
  drawBetPanel();
}

// ------------------------------------------------ Cash out / high scores ---
void drawScoresScreen(bool viewOnly) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_GOLD, TFT_BLACK);
  tft.drawString("HIGH SCORES", 160, 24, 4);
  for (int i = 0; i < 5; i++) {
    int y = 56 + i * 22;
    uint16_t col = (viewOnly && i == hsNew) ? COL_GOLD
                 : hsName[i][0] ? TFT_WHITE : TFT_DARKGREY;
    tft.setTextColor(col, TFT_BLACK);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(String(i + 1) + ". " + (hsName[i][0] ? hsName[i] : "-------"),
                   44, y, 2);
    tft.setTextDatum(MR_DATUM);
    if (hsName[i][0]) tft.drawString("$" + String(hsVal[i]), 276, y, 2);
  }
  tft.setTextDatum(MC_DATUM);
  if (viewOnly) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Tap to continue", 160, 216, 2);
  } else {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Cash out $" + String(bankroll + betP + betB + betT) + "?",
                   160, 168, 2);
    tft.fillRoundRect(Z_CASH.x, Z_CASH.y, Z_CASH.w, Z_CASH.h, 5, TFT_DARKGREEN);
    tft.drawRoundRect(Z_CASH.x, Z_CASH.y, Z_CASH.w, Z_CASH.h, 5, TFT_GREEN);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.drawString("CASH OUT", Z_CASH.x + Z_CASH.w / 2, Z_CASH.y + Z_CASH.h / 2, 2);
    tft.fillRoundRect(Z_CANCL.x, Z_CANCL.y, Z_CANCL.w, Z_CANCL.h, 5, COL_PANEL);
    tft.drawRoundRect(Z_CANCL.x, Z_CANCL.y, Z_CANCL.w, Z_CANCL.h, 5, TFT_LIGHTGREY);
    tft.setTextColor(TFT_WHITE, COL_PANEL);
    tft.drawString("CANCEL", Z_CANCL.x + Z_CANCL.w / 2, Z_CANCL.y + Z_CANCL.h / 2, 2);
  }
}

void drawNameKey(int idx) {
  int x = KB_X0 + (idx % 7) * KB_CW, y = KB_Y0 + (idx / 7) * KB_CH;
  char one[2] = { (char)('A' + idx), 0 };
  const char* lbl = idx < 26 ? one : idx == 26 ? "DEL" : "OK";
  uint16_t col = idx < 26 ? TFT_LIGHTGREY : idx == 26 ? COL_BANKER : TFT_GREEN;
  tft.fillRoundRect(x, y, KB_CW - 4, KB_CH - 4, 4, COL_PANEL);
  tft.drawRoundRect(x, y, KB_CW - 4, KB_CH - 4, 4, col);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(idx < 26 ? TFT_WHITE : col, COL_PANEL);
  tft.drawString(lbl, x + (KB_CW - 4) / 2, y + (KB_CH - 4) / 2, 2);
}

void drawNameText() {
  tft.fillRect(0, 24, 320, 32, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String s = String(nameBuf);
  if (nameLen < NAME_MAX) s += "_";
  tft.drawString(s, 160, 40, 4);
}

void drawNameEntry() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_GOLD, TFT_BLACK);
  tft.drawString("NEW HIGH SCORE - enter your name", 160, 10, 2);
  drawNameText();
  for (int i = 0; i < 28; i++) drawNameKey(i);
}

// ------------------------------------------------------------- Deal flow ---
void revealCard(bool banker, int i, uint8_t c) {
  int x = cardX(banker, i);
  drawCardBack(x, CARD_Y);
  sClick();
  delay(160);
  drawCardFace(x, CARD_Y, c);
  drawTotal(banker, i + 1);
  delay(340);
}

// --------------------------------------------------------------- Tutor ---
void showInstruction(const char* l1, const char* l2, const char* l3) {
  tft.fillRect(0, PANEL_Y, 320, 240 - PANEL_Y, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(l1, 160, 166, 2);
  tft.drawString(l2, 160, 184, 2);
  tft.setTextColor(COL_GOLD, TFT_BLACK);
  tft.drawString(l3, 160, 202, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Tap to continue", 160, 226, 2);
}

// Blocks until a fresh tap: waits out any held press, then a press+release.
void waitForTap() {
  while (ts.touched())  delay(20);
  while (!ts.touched()) delay(20);
  while (ts.touched())  delay(20);
}

// Walks the third-card tableau step by step. The hand is already fully dealt
// by playHand(), so each explanation is derived from what actually happened.
void teachHand() {
  uint8_t pv = handValue(pCards, 2), bv = handValue(bCards, 2);
  char l1[44], l2[44];

  if (natural) {
    if (pv >= 8 && bv >= 8) strcpy(l1, "Both hands are naturals!");
    else snprintf(l1, sizeof l1, "%s has a natural %d!",
                  pv >= 8 ? "Player" : "Banker", pv >= 8 ? pv : bv);
    showInstruction(l1, "An 8 or 9 in two cards ends the hand.",
                    "-> No third cards.");
    waitForTap();
    return;
  }

  snprintf(l1, sizeof l1, "Player has %d.", pv);
  showInstruction(l1, "Rule: Player draws on 0-5, stands on 6-7.",
                  pN > 2 ? "-> Player draws a card." : "-> Player stands.");
  waitForTap();
  if (pN > 2) revealCard(false, 2, pCards[2]);

  if (pN == 2) {
    snprintf(l1, sizeof l1, "Banker has %d. Player stood.", bv);
    strcpy(l2, "Rule: draw on 0-5, stand on 6-7.");
  } else {
    snprintf(l1, sizeof l1, "Banker has %d. 3rd card was worth %d.",
             bv, cardPoints(pCards[2]));
    if      (bv <= 2) strcpy(l2, "Rule: with 0-2 always draw.");
    else if (bv == 3) strcpy(l2, "Rule: with 3, draw unless it was an 8.");
    else if (bv == 4) strcpy(l2, "Rule: with 4, draw only vs 2-7.");
    else if (bv == 5) strcpy(l2, "Rule: with 5, draw only vs 4-7.");
    else if (bv == 6) strcpy(l2, "Rule: with 6, draw only vs 6-7.");
    else              strcpy(l2, "Rule: with 7 always stand.");
  }
  showInstruction(l1, l2, bN > 2 ? "-> Banker draws." : "-> Banker stands.");
  waitForTap();
  if (bN > 2) revealCard(true, 2, bCards[2]);
}

void runDeal() {
  if (SHOE_SIZE - shoePos < CUT_CARD) {
    tft.fillRect(0, PANEL_Y, 320, 240 - PANEL_Y, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_GOLD, TFT_BLACK);
    tft.drawString("Shuffling shoe...", 160, 190, 4);
    sShuffle();
    shuffleShoe();
    delay(500);
  }

  playHand();      // logic first; then replay the reveal on screen

  drawTable();
  tft.fillRect(0, PANEL_Y, 320, 240 - PANEL_Y, TFT_BLACK);
  drawHeader();

  // Casino order: Player, Banker, Player, Banker, then third cards.
  revealCard(false, 0, pCards[0]);
  revealCard(true,  0, bCards[0]);
  revealCard(false, 1, pCards[1]);
  revealCard(true,  1, bCards[1]);
  if (teachOn) {
    teachHand();
  } else {
    if (pN > 2) { delay(300); revealCard(false, 2, pCards[2]); }
    if (bN > 2) { delay(300); revealCard(true,  2, bCards[2]); }
  }

  uint8_t pv = handValue(pCards, pN), bv = handValue(bCards, bN);
  Outcome res = pv > bv ? WIN_PLAYER : bv > pv ? WIN_BANKER : WIN_TIE;

  // Settle: Player 1:1, Banker 19:20, Tie 8:1 (and P/B bets push on a tie).
  int stake = betP + betB + betT;
  int winnings = 0;
  if      (res == WIN_PLAYER) winnings = betP * 2;
  else if (res == WIN_BANKER) winnings = betB + (betB * 95) / 100;
  else                        winnings = betT * 9 + betP + betB;
  bankroll += winnings;
  int net = winnings - stake;
  lastBetP = betP; lastBetB = betB; lastBetT = betT;   // remembered for REBET
  betP = betB = betT = 0;

  handsThisRun++;
  recordRunPoint(bankroll);
  if (bankroll > runPeak) runPeak = bankroll;

  pushBead(res);
  highlightWinner(res);
  drawHeader();
  drawBeadRoad();

  Serial.printf("P:%d(%d cards) B:%d(%d cards) %s%s  net %+d  bankroll %d\n",
                pv, pN, bv, bN,
                res == WIN_PLAYER ? "PLAYER" : res == WIN_BANKER ? "BANKER" : "TIE",
                natural ? " (natural)" : "", net, bankroll);

  const char* headline;
  uint16_t col;
  if      (res == WIN_PLAYER) { headline = natural ? "PLAYER NATURAL!" : "PLAYER WINS!"; col = COL_PLAYER; }
  else if (res == WIN_BANKER) { headline = natural ? "BANKER NATURAL!" : "BANKER WINS!"; col = COL_BANKER; }
  else                        { headline = "TIE!"; col = COL_TIE; }

  // Green = you profited, red = you lost money, blue = push. Pulses with the
  // jingle, then stays lit until the result screen is dismissed.
  bool lr = net < 0, lg = net > 0, lb = net == 0;
  ledSet(lr, lg, lb);
  if      (net > 0) sWin();
  else if (net < 0) sLose();
  else              sTie();
  for (int i = 0; i < 2; i++) {
    ledOff(); delay(90);
    ledSet(lr, lg, lb); delay(90);
  }

  bool busted = bankroll < CHIPS[0];
  drawResultPanel(headline, col, net, busted);
  state = busted ? ST_GAMEOVER : ST_RESULT;
}

// ------------------------------------------------------------ Touch flow ---
void resetGame() {
  bankroll = 1000;
  betP = betB = betT = 0;
  lastBetP = lastBetB = lastBetT = 0;   // no rebets across lifetimes
  beadCount = 0;
  resetRunStats();
  shuffleShoe();
  state = ST_BETTING;
  drawMainScreen();
}

void handleTapScores(int x, int y) {
  if (inZone(Z_CANCL, x, y)) {
    sClick();
    state = ST_BETTING;
    drawMainScreen();       // bets were left on the table; panel still shows them
  } else if (inZone(Z_CASH, x, y)) {
    sClick();
    cashPending = bankroll + betP + betB + betT;   // pick your chips back up too
    Serial.printf("Cash out: $%d\n", cashPending);
    if (cashPending > hsVal[4]) {
      nameLen = 0;
      nameBuf[0] = 0;
      state = ST_NAME;
      drawNameEntry();
    } else {
      resetGame();          // no glory, straight back to a fresh $1000
    }
  }
}

void handleTapName(int x, int y) {
  if (x < KB_X0 || y < KB_Y0) return;
  int col = (x - KB_X0) / KB_CW, row = (y - KB_Y0) / KB_CH;
  if (col > 6 || row > 3) return;
  int idx = row * 7 + col;
  if (idx < 26) {
    if (nameLen < NAME_MAX) {
      nameBuf[nameLen++] = 'A' + idx;
      nameBuf[nameLen] = 0;
      sClick();
      drawNameText();
    }
  } else if (idx == 26) {
    if (nameLen > 0) {
      nameBuf[--nameLen] = 0;
      sClick();
      drawNameText();
    }
  } else if (nameLen > 0) {  // OK needs at least one letter
    hsNew = insertScore(nameBuf, cashPending);
    saveScores();
    sWin();
    state = ST_SCORES_VIEW;
    drawScoresScreen(true);
  }
}

void handleTapBetting(int x, int y) {
  if (inZone(Z_TITLE, x, y)) {
    sClick();
    state = ST_SETTINGS;
    drawSettingsScreen();
    return;
  }
  if (inZone(Z_BANK, x, y)) {
    sClick();
    state = ST_SCORES;
    drawScoresScreen(false);
    return;
  }
  int chip = CHIPS[chipIdx];
  if (inZone(Z_BETP, x, y) && bankroll >= chip) {
    betP += chip; bankroll -= chip; sChip();
    drawBetZone(Z_BETP, "PLAYER", betP, COL_PLAYER);
    drawHeader(); drawDealButton(); drawClearButton();
  } else if (inZone(Z_BETT, x, y) && bankroll >= chip) {
    betT += chip; bankroll -= chip; sChip();
    drawBetZone(Z_BETT, "TIE 8:1", betT, COL_TIE);
    drawHeader(); drawDealButton(); drawClearButton();
  } else if (inZone(Z_BETB, x, y) && bankroll >= chip) {
    betB += chip; bankroll -= chip; sChip();
    drawBetZone(Z_BETB, "BANKER", betB, COL_BANKER);
    drawHeader(); drawDealButton(); drawClearButton();
  } else if (inZone(Z_MINUS, x, y)) {
    if (chipIdx > 0) { chipIdx--; sClick(); drawChipValue(); }
  } else if (inZone(Z_PLUS, x, y)) {
    if (chipIdx < N_CHIPS - 1) { chipIdx++; sClick(); drawChipValue(); }
  } else if (inZone(Z_CLEAR, x, y)) {
    int total = betP + betB + betT;
    int lastTotal = lastBetP + lastBetB + lastBetT;
    if (total > 0) {                                    // CLEAR
      bankroll += total;
      betP = betB = betT = 0;
      sClick(); drawBetPanel(); drawHeader();
    } else if (lastTotal > 0 && bankroll >= lastTotal) { // REBET
      betP = lastBetP; betB = lastBetB; betT = lastBetT;
      bankroll -= lastTotal;
      sChip(); drawBetPanel(); drawHeader();
    }
  } else if (inZone(Z_DEAL, x, y)) {
    if (betP + betB + betT > 0) runDeal();
  }
}

void handleTap(int x, int y) {
  switch (state) {
    case ST_BETTING:  handleTapBetting(x, y); break;
    case ST_RESULT:
      ledOff();
      state = ST_BETTING;
      drawBetPanel();
      break;
    case ST_GAMEOVER:
      ledOff();
      state = ST_BUSTED;
      drawBustScreen();
      sBust();
      break;
    case ST_BUSTED:
      resetGame();
      break;
    case ST_SCORES:
      handleTapScores(x, y);
      break;
    case ST_NAME:
      handleTapName(x, y);
      break;
    case ST_SCORES_VIEW:
      hsNew = -1;
      resetGame();
      break;
    case ST_SETTINGS:
      if (inZone(Z_SOUND, x, y)) {
        soundOn = !soundOn;
        prefs.putBool("sound", soundOn);
        drawToggle(Z_SOUND, "SOUND", soundOn);
        sClick();               // audible confirmation only when unmuting
      } else if (inZone(Z_TEACH, x, y)) {
        teachOn = !teachOn;
        prefs.putBool("teach", teachOn);
        drawToggle(Z_TEACH, "TUTOR", teachOn);
        sClick();
      } else if (inZone(Z_DONE, x, y)) {
        sClick();
        state = ST_BETTING;
        drawMainScreen();
      }
      break;
  }
}

// ----------------------------------------------------------------- Setup ---
void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  ledOff();

  tft.init();
  tft.setRotation(1);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  prefs.begin("baccarat", false);
  soundOn = prefs.getBool("sound", true);
  teachOn = prefs.getBool("teach", false);
  loadScores();

  shuffleShoe();
  resetRunStats();
  drawMainScreen();

  Serial.printf("Cheap Yellow Baccarat ready. Sound %s.\n", soundOn ? "on" : "off");
}

bool wasTouched = false;

void loop() {
  bool t = ts.touched();
  if (t && !wasTouched) {
    TS_Point p = ts.getPoint();
    int x = map(p.x, RAW_X_MIN, RAW_X_MAX, 0, tft.width());
    int y = map(p.y, RAW_Y_MIN, RAW_Y_MAX, 0, tft.height());
    handleTap(x, y);
  }
  wasTouched = t;
  delay(20);
}
