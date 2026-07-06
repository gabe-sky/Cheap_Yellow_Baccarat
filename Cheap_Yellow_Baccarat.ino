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
enum State   { ST_BETTING, ST_RESULT, ST_GAMEOVER, ST_SETTINGS };
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

const int CHIPS[] = { 10, 25, 50, 100 };
#define N_CHIPS 4
int chipIdx  = 1;                 // start on $25
int bankroll = 1000;
int betP = 0, betB = 0, betT = 0; // deducted from bankroll as placed

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
const Zone Z_SOUND = {  60,  90, 200, 44 };  // settings screen
const Zone Z_DONE  = {  60, 170, 200, 44 };

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

void drawBetPanel() {
  tft.fillRect(0, PANEL_Y, 320, 240 - PANEL_Y, TFT_BLACK);
  drawBetZone(Z_BETP, "PLAYER", betP, COL_PLAYER);
  drawBetZone(Z_BETT, "TIE 8:1", betT, COL_TIE);
  drawBetZone(Z_BETB, "BANKER", betB, COL_BANKER);
  drawButton(Z_MINUS, "-", TFT_LIGHTGREY, TFT_WHITE);
  drawButton(Z_PLUS,  "+", TFT_LIGHTGREY, TFT_WHITE);
  drawButton(Z_CLEAR, "CLEAR", TFT_LIGHTGREY, TFT_WHITE);
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
  tft.drawString(gameOver ? "BUSTED - tap to restart with $1000"
                          : "Tap to continue", 160, 224, 2);
}

// ------------------------------------------------------- Settings screen ---
void drawSoundButton() {
  uint16_t col = soundOn ? TFT_GREEN : COL_BANKER;
  tft.fillRoundRect(Z_SOUND.x, Z_SOUND.y, Z_SOUND.w, Z_SOUND.h, 5, COL_PANEL);
  tft.drawRoundRect(Z_SOUND.x, Z_SOUND.y, Z_SOUND.w, Z_SOUND.h, 5, col);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(col, COL_PANEL);
  tft.drawString(soundOn ? "SOUND: ON" : "SOUND: OFF",
                 Z_SOUND.x + Z_SOUND.w / 2, Z_SOUND.y + Z_SOUND.h / 2, 4);
}

void drawSettingsScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_GOLD, TFT_BLACK);
  tft.drawString("SETTINGS", 160, 36, 4);
  drawSoundButton();
  tft.fillRoundRect(Z_DONE.x, Z_DONE.y, Z_DONE.w, Z_DONE.h, 5, TFT_DARKGREEN);
  tft.drawRoundRect(Z_DONE.x, Z_DONE.y, Z_DONE.w, Z_DONE.h, 5, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("DONE", Z_DONE.x + Z_DONE.w / 2, Z_DONE.y + Z_DONE.h / 2, 4);
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
  if (pN > 2) { delay(300); revealCard(false, 2, pCards[2]); }
  if (bN > 2) { delay(300); revealCard(true,  2, bCards[2]); }

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
  betP = betB = betT = 0;

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
void handleTapBetting(int x, int y) {
  if (inZone(Z_TITLE, x, y)) {
    sClick();
    state = ST_SETTINGS;
    drawSettingsScreen();
    return;
  }
  int chip = CHIPS[chipIdx];
  if (inZone(Z_BETP, x, y) && bankroll >= chip) {
    betP += chip; bankroll -= chip; sChip();
    drawBetZone(Z_BETP, "PLAYER", betP, COL_PLAYER); drawHeader(); drawDealButton();
  } else if (inZone(Z_BETT, x, y) && bankroll >= chip) {
    betT += chip; bankroll -= chip; sChip();
    drawBetZone(Z_BETT, "TIE 8:1", betT, COL_TIE); drawHeader(); drawDealButton();
  } else if (inZone(Z_BETB, x, y) && bankroll >= chip) {
    betB += chip; bankroll -= chip; sChip();
    drawBetZone(Z_BETB, "BANKER", betB, COL_BANKER); drawHeader(); drawDealButton();
  } else if (inZone(Z_MINUS, x, y)) {
    if (chipIdx > 0) { chipIdx--; sClick(); drawChipValue(); }
  } else if (inZone(Z_PLUS, x, y)) {
    if (chipIdx < N_CHIPS - 1) { chipIdx++; sClick(); drawChipValue(); }
  } else if (inZone(Z_CLEAR, x, y)) {
    if (betP + betB + betT > 0) {
      bankroll += betP + betB + betT;
      betP = betB = betT = 0;
      sClick(); drawBetPanel(); drawHeader();
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
      bankroll = 1000;
      beadCount = 0;
      shuffleShoe();
      state = ST_BETTING;
      drawMainScreen();
      break;
    case ST_SETTINGS:
      if (inZone(Z_SOUND, x, y)) {
        soundOn = !soundOn;
        prefs.putBool("sound", soundOn);
        drawSoundButton();
        sClick();               // audible confirmation only when unmuting
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

  shuffleShoe();
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
