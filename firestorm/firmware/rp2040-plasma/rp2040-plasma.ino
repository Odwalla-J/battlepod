// rp2040-plasma: the pod's plasma sign on a 128x32 HUB75 LED matrix.
//
// A Waveshare RP2040-Zero (or any RP2040) that Windows sees as a USB serial
// port, "Firestorm Plasma". It takes the bytes the game sends its sign -
// text as itself, commands after ESC (docs/RIO.md, *The plasma sign*) - as
// the shim passes them on, every x already moved to the middle of a 128-dot
// sign, and draws them in orange. At power-up it plays a 'Mech's boot
// sequence until the game takes over.
//
// Wiring (README.md): R1 G1 B1 R2 G2 B2 on GP2-GP7, A-D on GP8-GP11, CLK
// GP12, LAT GP13, OE GP14. Build with Earle Philhower's arduino-pico core,
// Tools > USB Stack: "Adafruit TinyUSB".
//
// Core 0 reads the port and draws into dots[][]; core 1 does nothing but
// scan the panel. Orange is red all the time a row is lit and green for part
// of it: each row is sent twice, red+green and then red alone, each shown
// for a number of clock periods of the next row's shifting (GREEN_ON,
// RED_ON, out of 128).

#include <Adafruit_TinyUSB.h>
#include <hardware/gpio.h>
#include <hardware/structs/sio.h>

static const char *NAME = "Firestorm Plasma";
static const uint16_t VID = 0x1209, PID = 0x0004;   // pid.codes; see ../rp2040-controls/README.md

static const int W = 128, H = 32;

static const int PIN_R1 = 2;          // R1 G1 B1 R2 G2 B2 = GP2..GP7
static const int PIN_A = 8;           // A B C D = GP8..GP11
static const int PIN_CLK = 12, PIN_LAT = 13, PIN_OE = 14;

static const uint32_t R1 = 1u << 2, G1 = 1u << 3, R2 = 1u << 5, G2 = 1u << 6;
static const uint32_t DATA_MASK = 0x3Fu << PIN_R1, ADDR_MASK = 0x0Fu << PIN_A;
static const uint32_t CLK = 1u << PIN_CLK, LAT = 1u << PIN_LAT, OE = 1u << PIN_OE;

static const int RED_ON = 128;        // brightness: clocks of 128 a row is lit, red alone
static const int GREEN_ON = 48;       // and red+green; GREEN_ON / (GREEN_ON + RED_ON) is the orange's green
static const int CLK_NOPS = 4;        // slows the clock for long wires; raise it if dots sparkle

static volatile uint8_t dots[H][W];
static volatile int level = 256;      // brightness, of 256: the boot sequence starts dim
static bool replay = true;            // the boot sequence, at power-up and on ESC 'b'

// ------------------------------------------------ the sign's drawing, as the shim's (src/ddraw.c)

// The classic 5x7 font, ' ' to '~': five columns, bit 0 at the top.
static const uint8_t font5x7[95][5] = {
  {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
  {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
  {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
  {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
  {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
  {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
  {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
  {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
  {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
  {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
  {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
  {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
  {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
  {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
  {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
  {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
  {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
  {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
  {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
  {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08},
};

static void dot(int x, int y, int on)
{
  if (x >= 0 && x < W && y >= 0 && y < H) dots[y][x] = (uint8_t)on;
}

static void rect(int x0, int y0, int x1, int y1, bool fill, int on)
{
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      if (fill || y == y0 || y == y1 || x == x0 || x == x1) dot(x, y, on);
}

// One character at (x, y): 5x7, or for the large fonts the same at 10x14
// less a column, so letters keep a gap. Its whole cell is drawn, unlit dots
// too. Returns how far the next one goes.
static int glyph(int x, int y, int ch, bool big)
{
  static const int col_x[5] = { 0, 2, 4, 5, 7 }, col_w[5] = { 2, 2, 1, 2, 2 };
  rect(x, y, x + (big ? 9 : 5), y + (big ? 13 : 6), true, 0);
  if (ch < 0x20 || ch > 0x7E) ch = '?';
  const uint8_t *g = font5x7[ch - 0x20];
  for (int c = 0; c < 5; c++)
    for (int r = 0; r < 7; r++) {
      if (!(g[c] >> r & 1)) continue;
      if (!big) dot(x + c, y + r, 1);
      else
        for (int i = 0; i < col_w[c]; i++)
          for (int j = 0; j < 2; j++) dot(x + col_x[c] + i, y + 2 * r + j, 1);
    }
  return big ? 10 : 6;
}

static int cx, cy, font;

// The bytes as they come, a command at a time.
static void feed(uint8_t c)
{
  static uint8_t cmd[8];
  static int have, need;

  if (have) {
    cmd[have++] = c;
    if (have == 2)
      switch (c) {
      case 0x40: need = 2; break;
      case 0x52: case 0x51: case 0x4B: case 0x48: case 0x47: need = 3; break;
      case 0x58: need = 6; break;
      case 0x78: need = 7; break;
      default: need = 2;
      }
    if (have < need) return;
    switch (cmd[1]) {
    case 0x40: rect(0, 0, W - 1, H - 1, true, 0); cx = cy = 0; break;
    case 0x52: cx = cmd[2]; break;
    case 0x51: cy = cmd[2]; break;
    case 0x4B: font = cmd[2]; break;
    case 0x58: rect(cmd[2], cmd[3], cmd[4], cmd[5], false, 1); break;
    case 0x78: rect(cmd[3], cmd[4], cmd[5], cmd[6], true, cmd[2] != 0); break;
    case 0x62: replay = true; break;    // ours: the boot sequence again
    }
    have = 0;
  } else if (c == 0x1B) {
    have = 1;
    need = 2;
  } else
    cx += glyph(cx, cy, c, font == 4 || font == 5);
}

static void feed(const char *s)
{
  while (*s) feed((uint8_t)*s++);
}

// ------------------------------------------------ the boot sequence

// At power-up, and on ESC 'b' (not one of the game's), the sign comes up as
// a 'Mech does: a terminal of 21 columns by 4 lines, each system typed out
// and its dots ticking over unevenly, a spinner turning, until it is
// ONLINE. It starts dim, on emergency power, and the reactor brings it up
// to full. Anything on the port stops it and clears the sign: the game
// always wins.

static int spin_col = -1, spin_line;  // where the spinner turns while a system loads

static void put(int col, int line, char ch) { glyph(1 + 6 * col, 8 * line, ch, false); }
static void put(int col, int line, const char *s) { while (*s) put(col++, line, *s++); }
static uint32_t rnd(uint32_t lo, uint32_t hi) { return lo + rp2040.hwrand32() % (hi - lo + 1); }

// Waits ms, turning the spinner; false as soon as the port has something.
static bool pause(uint32_t ms)
{
  static const char spin[] = "|/-\\";
  static int turn;
  uint32_t start = millis(), last = start;
  while (millis() - start < ms) {
    if (Serial.available()) return false;
    if (spin_col >= 0 && millis() - last >= 90) {
      last = millis();
      put(spin_col, spin_line, spin[turn++ & 3]);
    }
  }
  return true;
}

#define STEP(x) do { if (!(x)) return false; } while (0)

static bool type(int col, int line, const char *s, uint32_t lo, uint32_t hi)
{
  for (; *s; s++) {
    put(col++, line, *s);
    STEP(pause(rnd(lo, hi)));
  }
  return true;
}

// One system: its name, dots to column 13, ONLINE at 15-20. `weight` (4 is
// ordinary) stretches its loading; the reactor's flickers the sign's
// brightness as it climbs.
static bool system_up(int line, const char *name, int weight, bool reactor)
{
  STEP(type(0, line, name, 30, 70));
  spin_col = 20;
  spin_line = line;
  for (int col = strlen(name); col < 14; col++) {
    uint32_t ms = rnd(80, 220) * weight / 4;
    if (rnd(0, 3) == 0) ms += rnd(400, 900) * weight / 4;   // a stall
    if (reactor) level = 48 + 8 * col + rnd(0, 32);
    STEP(pause(ms));
    put(col, line, '.');
  }
  STEP(pause(rnd(200, 500)));
  spin_col = -1;
  put(15, line, "ONLINE");
  if (reactor)
    while (level < 256) {
      level = min(256, level + 6);
      STEP(pause(12));
    }
  STEP(pause(120));
  put(15, line, "      ");
  STEP(pause(80));
  put(15, line, "ONLINE");
  return pause(rnd(300, 500));
}

// Every dot goes dark, a few at a time in a random order, over about ms.
static bool dissolve(uint32_t ms)
{
  static uint16_t order[W * H];
  const int n = W * H, steps = ms / 15;
  for (int i = 0; i < n; i++) order[i] = i;
  for (int i = n - 1; i > 0; i--) {
    int j = rnd(0, i);
    uint16_t t = order[i];
    order[i] = order[j];
    order[j] = t;
  }
  for (int s = 0, i = 0; s < steps; s++) {
    for (int end = (s + 1) * n / steps; i < end; i++) dots[order[i] / W][order[i] % W] = 0;
    STEP(pause(15));
  }
  return true;
}

static bool boot_steps()
{
  STEP(pause(400));
  for (int i = 0; i < 3; i++) {         // a cursor, waiting
    put(0, 0, '_');
    STEP(pause(250));
    put(0, 0, ' ');
    STEP(pause(250));
  }
  STEP(system_up(0, "REACTOR", 6, true));
  STEP(system_up(1, "SENSORS", 4, false));
  STEP(system_up(2, "WEAPONS", 5, false));
  STEP(pause(600));
  STEP(type(1, 3, "ALL SYSTEMS NOMINAL", 25, 45));
  for (int i = 0; i < 3; i++) {
    STEP(pause(350));
    rect(0, 24, W - 1, H - 1, true, 0);
    STEP(pause(200));
    put(1, 3, "ALL SYSTEMS NOMINAL");
  }
  STEP(pause(15000));
  return dissolve(1500);
}

static void boot()
{
  feed("\x1B@");
  level = 48;
  bool done = boot_steps();
  spin_col = -1;
  level = 256;
  if (!done) feed("\x1B@");
}

// ------------------------------------------------ core 0: the port

void setup()
{
  TinyUSBDevice.setID(VID, PID);
  TinyUSBDevice.setManufacturerDescriptor("BattleTech Preservation");
  TinyUSBDevice.setProductDescriptor(NAME);
  Serial.begin(9600);
}

void loop()
{
  if (replay) {
    replay = false;
    boot();
  }
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    feed((uint8_t)c);
  }
}

// ------------------------------------------------ core 1: the panel

static inline void wait_nops(void)
{
  for (int i = 0; i < CLK_NOPS; i++) __asm volatile("nop");
}

void setup1()
{
  uint32_t all = DATA_MASK | ADDR_MASK | CLK | LAT | OE;
  gpio_init_mask(all);
  gpio_set_dir_out_masked(all);
  for (int p = PIN_R1; p <= PIN_OE; p++) gpio_set_drive_strength(p, GPIO_DRIVE_STRENGTH_8MA);
  sio_hw->gpio_clr = all;
  sio_hw->gpio_set = OE;                // OE is active low: dark
}

// Shift one row pair (rows y and y + 16), red+green or red alone, while the
// one latched before is lit for `lit` clocks.
static void shift(int y, bool green, int lit)
{
  const volatile uint8_t *top = dots[y], *bot = dots[y + H / 2];
  uint32_t t = green ? R1 | G1 : R1, b = green ? R2 | G2 : R2;
  for (int x = 0; x < W; x++) {
    uint32_t v = (top[x] ? t : 0) | (bot[x] ? b : 0);
    sio_hw->gpio_togl = (sio_hw->gpio_out ^ v) & (DATA_MASK | CLK);   // data, CLK low
    if (x == lit) sio_hw->gpio_set = OE;
    wait_nops();
    sio_hw->gpio_set = CLK;
    wait_nops();
  }
  sio_hw->gpio_clr = CLK;
  sio_hw->gpio_set = OE;
}

static void show(int y)
{
  sio_hw->gpio_togl = (sio_hw->gpio_out ^ ((uint32_t)y << PIN_A)) & ADDR_MASK;
  wait_nops();
  sio_hw->gpio_set = LAT;
  wait_nops();
  sio_hw->gpio_clr = LAT;
  sio_hw->gpio_clr = OE;
}

void loop1()
{
  static int lit = 0;                   // how long the row latched last is lit
  for (int y = 0; y < H / 2; y++) {
    shift(y, true, lit);
    show(y);
    lit = GREEN_ON * level >> 8;
    shift(y, false, lit);
    show(y);
    lit = RED_ON * level >> 8;
  }
}
