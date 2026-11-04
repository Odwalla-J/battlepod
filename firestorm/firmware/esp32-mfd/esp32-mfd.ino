// esp32-mfd: one of Firestorm's MFDs on an ESP32-S3 RGB display module.
//
// A cockpit module (docs/MODULE_PROTOCOL.md) with a framebuffer: it answers
// the bridge's HELLO? as class "mfd", takes the MFD picture stream
// (docs/MFD_STREAMING_PROTOCOL.md: FRAME parts of RLA rectangles, DEFLATE
// with the ROM's tinfl) into a grey framebuffer the panel is fed from, and says
// FRAME_ACK when a frame is on the screen and SYNC when it needs a whole one.
// tools/mfdstream.py's Decoder is the reference; this is it in C.
//
// The board: Elecrow CrowPanel 7.0" (DIS08070H, v3.0): ESP32-S3-WROOM-1
// N4R8, an 800x480 RGB panel, backlight PWM on GPIO 2, a CH340 on UART0.
// The 640x480 picture is drawn 1:1 in the middle, 80 px of black each side.
//
// Build: arduino-cli, esp32:esp32 core 3.x,
//   --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=4M,CDCOnBoot=default
// The link is the CH340 at LINK_BAUD; the bridge opens it at the same speed
// (bridge.py --serial COM8@2000000).

#include <Arduino.h>
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "miniz.h"                // tinfl, in the ESP32-S3 ROM
#include <Preferences.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"

// ---- this module
// Its name is which MFD it is - "mfd:1" to "mfd:5", the names in
// layouts/firestorm-mfds - kept in flash and set by the host (NAME, below;
// the launcher's MFD boards page). Never named, it is "mfd-" and the end of
// its MAC address, which no layout knows: the bridge leaves it alone.
// Its colour - the launcher's MFD colour, white at first - is kept the same
// way (TINT, below).
#define FIRMWARE    "esp32-mfd 0.6"
#define LINK_BAUD   2000000
#ifndef PANEL_CORE
#define PANEL_CORE  0                 // the core the panel's interrupt runs on: see panel_task
#endif

// ---- the panel (DIS08070H v3.0)
#define LCD_W 800
#define LCD_H 480
#define MFD_W 640
#define MFD_H 480
#define MFD_X ((LCD_W - MFD_W) / 2)
#define BACKLIGHT_PIN 2

// Two framebuffers of the MFD's grey, MFD_W x MFD_H, a byte a pixel, in
// PSRAM: one on the screen, one the next update is decoded into. The panel
// has no framebuffer of its own: the driver's two bounce buffers in internal
// RAM, BOUNCE_LINES each, are filled here (on_bounce_empty) from the grey one on
// the screen, through the tint, as the panel scans. A finished update goes
// on the screen whole - the feed takes the other buffer from the top of the
// next frame, so no half-drawn picture is ever seen (a tear) - and then its
// rectangles are copied into the other buffer, so both hold the frame and
// the next update, a change to it, applies to either.
//
// Before 0.6 the buffers were the panel's own, RGB565, and the driver filled
// the bounce buffers from them: twice the PSRAM traffic, and a whole frame
// decoded and copied took enough of it that a refill came too late - the
// picture slipped, part of its bottom at the top.
static uint8_t *greys[2];
static volatile int front = 0;        // the buffer on the screen
static volatile int next = -1;        // the one to show from the next frame
static uint8_t *fb;                   // where updates are decoded: greys[front ^ 1]
static uint16_t lut[256];             // grey -> RGB565 through the tint
static uint8_t tint[3] = { 255, 255, 255 };   // the MFD's colour, kept in flash (TINT)
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t fb_free;     // given when the feed has taken the next buffer

#ifndef BOUNCE_LINES
#define BOUNCE_LINES 30                // lines a bounce buffer (480 a whole number of them): 1.8 ms to refill one
#endif
#define LINE_US      60                // a line of the panel, blanking and all: 901 clocks at 15 MHz

// Whether the feed keeps up: a band filled later than a band's time after
// the panel wanted it was sent unfilled (a slip). Told with the link's errors.
static volatile uint32_t pv_vsync, pv_frames, pv_slips, pv_worst, pv_base;

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t p, const esp_lcd_rgb_panel_event_data_t *e, void *ctx) {
  pv_vsync = (uint32_t)esp_timer_get_time();
  pv_frames++;
  return false;
}

static bool IRAM_ATTR on_bounce_empty(esp_lcd_panel_handle_t p, void *buf, int pos_px, int len_bytes, void *ctx) {
  BaseType_t woke = pdFALSE;
  if (pos_px == 0 && next >= 0) {               // the top of a frame: the new picture from here
    front = next;
    next = -1;
    xSemaphoreGiveFromISR(fb_free, &woke);
  }
  const uint8_t *g = greys[front];
  uint16_t *o = (uint16_t *)buf;
  int y = pos_px / LCD_W, lines = len_bytes / (LCD_W * 2);
  for (int r = 0; r < lines; r++, y++) {
    memset(o, 0, MFD_X * 2);
    o += MFD_X;
    const uint32_t *s = (const uint32_t *)(g + (size_t)y * MFD_W);
    for (int i = 0; i < MFD_W / 4; i++, o += 4) {
      uint32_t v = s[i];
      o[0] = lut[v & 0xFF];
      o[1] = lut[v >> 8 & 0xFF];
      o[2] = lut[v >> 16 & 0xFF];
      o[3] = lut[v >> 24];
    }
    memset(o, 0, (LCD_W - MFD_X - MFD_W) * 2);
    o += LCD_W - MFD_X - MFD_W;
  }
  // Past the first two bands (filled ahead, round the VSYNC), each band is
  // filled a fixed time after the VSYNC plus its lines.
  int line = pos_px / LCD_W;
  if (line >= 2 * BOUNCE_LINES && pv_frames > 32) {
    int32_t d = (int32_t)((uint32_t)esp_timer_get_time() - pv_vsync) - line * LINE_US;
    if (!pv_base || d < (int32_t)pv_base) pv_base = d > 0 ? d : 1;
    uint32_t late = d - pv_base;
    if (late > pv_worst) pv_worst = late;
    if (late > BOUNCE_LINES * LINE_US) pv_slips++;
  }
  return woke == pdTRUE;
}

static void panel_start() {
  esp_lcd_rgb_panel_config_t c = {};
  c.clk_src = LCD_CLK_SRC_DEFAULT;
  c.timings.pclk_hz = 15000000;
  c.timings.h_res = LCD_W;
  c.timings.v_res = LCD_H;
  c.timings.hsync_front_porch = 40;
  c.timings.hsync_pulse_width = 48;
  c.timings.hsync_back_porch = 13;
  c.timings.vsync_front_porch = 1;
  c.timings.vsync_pulse_width = 31;
  c.timings.vsync_back_porch = 13;
  c.timings.flags.pclk_active_neg = 1;
  c.data_width = 16;
  c.flags.no_fb = 1;                             // fed by on_bounce_empty, from the grey
  c.bounce_buffer_size_px = LCD_W * BOUNCE_LINES;
  c.hsync_gpio_num = 39;
  c.vsync_gpio_num = 40;
  c.de_gpio_num = 41;
  c.pclk_gpio_num = 0;
  c.disp_gpio_num = -1;
  const int data[16] = { 15, 7, 6, 5, 4,  9, 46, 3, 8, 16, 1,  14, 21, 47, 48, 45 };  // B0-4 G0-5 R0-4
  for (int i = 0; i < 16; i++) c.data_gpio_nums[i] = data[i];
  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&c, &panel));
  fb_free = xSemaphoreCreateBinary();
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_bounce_empty = on_bounce_empty;
  cbs.on_vsync = on_vsync;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, NULL));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ledcAttach(BACKLIGHT_PIN, 1220, 8);
  ledcWrite(BACKLIGHT_PIN, 255);
}

static void make_lut() {
  for (int v = 0; v < 256; v++) {
    int r = v * tint[0] / 255, g = v * tint[1] / 255, b = v * tint[2] / 255;
    lut[v] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  }
}

// Until the first frame: black. Before the panel starts.
static void make_buffers() {
  for (int i = 0; i < 2; i++) {
    greys[i] = (uint8_t *)heap_caps_malloc(MFD_W * MFD_H, MALLOC_CAP_SPIRAM);
    memset(greys[i], 0, MFD_W * MFD_H);
  }
  front = 0;
  fb = greys[1];
}

// The rectangles of the update being decoded, to copy across once it is shown.
#define MAX_RECTS 256
static struct { uint16_t x, y, w, h; } rects[MAX_RECTS];
static int n_rects;
static bool rects_all;                // too many to list: copy the whole picture

static void note_rect(int x, int y, int w, int h) {
  if (n_rects < MAX_RECTS) { rects[n_rects].x = x; rects[n_rects].y = y; rects[n_rects].w = w; rects[n_rects].h = h; n_rects++; }
  else rects_all = true;
}

// Put the decoded buffer on the screen at the next frame, wait until the
// one it replaces is no longer read, and bring that one up to date.
static void present() {
  int back = front ^ 1;
  xSemaphoreTake(fb_free, 0);
  next = back;
  if (xSemaphoreTake(fb_free, pdMS_TO_TICKS(100)) != pdTRUE) {   // a frame and a bit (31.7 Hz)
    front = back;                                // the panel is not running: no one to wait for
    next = -1;
  }
  const uint8_t *shown = greys[front];
  fb = greys[front ^ 1];
  if (rects_all) {
    memcpy(fb, shown, MFD_W * MFD_H);
  } else {
    for (int i = 0; i < n_rects; i++)
      for (int y = rects[i].y; y < rects[i].y + rects[i].h; y++) {
        size_t o = (size_t)y * MFD_W + rects[i].x;
        memcpy(fb + o, shown + o, rects[i].w);
      }
  }
  n_rects = 0;
  rects_all = false;
}

// ---- framing: COBS, then a zero; type, seq, payload, CRC-16/CCITT-FALSE
static uint16_t crc16(const uint8_t *p, size_t n, uint16_t crc = 0xFFFF) {
  while (n--) {
    crc ^= (uint16_t)(*p++) << 8;
    for (int i = 0; i < 8; i++) crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

static uint8_t tx_seq;
static void send(uint8_t type, const uint8_t *payload, size_t n) {
  static uint8_t raw[300], out[320];
  if (n > sizeof raw - 4) return;
  raw[0] = type;
  raw[1] = tx_seq++;
  memcpy(raw + 2, payload, n);
  uint16_t c = crc16(raw, n + 2);
  raw[n + 2] = c >> 8;
  raw[n + 3] = c & 0xFF;
  size_t len = n + 4, o = 1, code_at = 0;
  uint8_t code = 1;
  for (size_t i = 0; i < len; i++) {        // COBS, as modproto.cobs_encode
    if (raw[i] == 0) {
      out[code_at] = code; code_at = o++; code = 1;
    } else {
      out[o++] = raw[i];
      if (++code == 255) { out[code_at] = code; code_at = o++; code = 1; }
    }
  }
  out[code_at] = code;
  out[o++] = 0;
  Serial.write(out, o);
}

static size_t cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t room) {
  size_t i = 0, o = 0;
  while (i < n) {
    uint8_t code = in[i++];
    if (!code || i + code - 1 > n || o + code > room) return 0;
    memcpy(out + o, in + i, code - 1);
    o += code - 1;
    i += code - 1;
    if (code < 255 && i < n) out[o++] = 0;
  }
  return o;
}

static void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static char name[33];
static Preferences prefs;

static void load_name() {
  prefs.begin("esp32-mfd", false);
  String v = prefs.getString("name", "");
  if (v.length() && v.length() < sizeof name) {
    strcpy(name, v.c_str());
  } else {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(name, sizeof name, "mfd-%02X%02X%02X", (int)(mac >> 24 & 0xFF), (int)(mac >> 32 & 0xFF),
             (int)(mac >> 40 & 0xFF));
  }
  if (prefs.getBytesLength("tint") == 3) prefs.getBytes("tint", tint, 3);
}

static void send_hello() {
  uint8_t p[96];
  size_t n = 0;
  p[n++] = 0;                                    // protocol version
  const char *s[3] = { "mfd", name, FIRMWARE };
  for (int i = 0; i < 3; i++) { size_t l = strlen(s[i]); p[n++] = l; memcpy(p + n, s[i], l); n += l; }
  p[n++] = 8;                                    // keys
  p[n++] = 8;                                    // LED channels
  p[n++] = 8;                                    // PWM bits
  put_be16(p + n, MFD_W); n += 2;
  put_be16(p + n, MFD_H); n += 2;
  p[n++] = 3;                                    // an 8-bit grey framebuffer
  p[n++] = 1;                                    // capabilities: DEFLATE
  put_be16(p + n, 4096); n += 2;                 // the largest FRAME body
  memcpy(p + n, tint, 3); n += 3;                // its colour: R, G, B
  send(0x02, p, n);
}

// ---- the picture stream: tools/mfdstream.py's Decoder
enum { SYNC_NEW, SYNC_GAP, SYNC_BAD, SYNC_SIZE, SYNC_STREAM };
static int display = 0;                          // which MFD: taken from the host's key frames
static bool synced = false, building = false, b_key;
static uint32_t stream_id, frame_no, b_stream, b_frame;
static uint16_t b_part;
static int why = SYNC_NEW;
static uint32_t last_sync_ms;

static void send_sync() {
  if (millis() - last_sync_ms < 250) return;     // at most four a second
  last_sync_ms = millis();
  uint8_t p[6] = { (uint8_t)display, (uint8_t)why };
  put_be32(p + 2, frame_no);
  send(0x23, p, 6);
}

static bool lost(int reason) {
  synced = building = false;
  // what was decoded is wrong; a key frame will rewrite all of it
  n_rects = 0;
  rects_all = true;
  why = reason;
  send_sync();
  return false;
}

// One RLA rectangle into the framebuffer (grey through the table), as
// mfdstream.rla_decode; false if it does not fill the rectangle exactly.
static bool rla(const uint8_t *d, size_t len, int x, int y, int w, int h) {
  size_t i = 0;
  long n = (long)w * h, at = 0;
  uint8_t prev = 0;
  while (at < n) {
    if (i >= len) return false;
    uint8_t b = d[i++];
    int kind = b >> 6;
    long m = b & 0x1F;
    if (b & 0x20) { if (i >= len) return false; m = m << 8 | d[i++]; }
    m += 1;
    if (kind == 3 || at + m > n) return false;
    while (m) {
      int r = at / w, c = at % w;
      long k = min(m, (long)(w - c));
      uint8_t *o = fb + (size_t)(y + r) * MFD_W + x + c;
      if (kind == 0) {
        if (i + k > len) return false;
        memcpy(o, d + i, k);
        i += k;
      } else if (kind == 1) {
        memset(o, prev, k);
      } else {
        if (y + r == 0) return false;
        memcpy(o, o - MFD_W, k);
      }
      prev = o[k - 1];
      at += k;
      m -= k;
    }
  }
  return i == len;
}

static bool apply(const uint8_t *body, size_t len) {
  size_t at = 0;
  while (at < len) {
    if (at + 11 > len) return false;
    int x = be16(body + at), y = be16(body + at + 2), w = be16(body + at + 4), h = be16(body + at + 6);
    int codec = body[at + 8];
    size_t n = be16(body + at + 9);
    at += 11;
    if (at + n > len || !w || !h || x + w > MFD_W || y + h > MFD_H) return false;
    const uint8_t *d = body + at;
    at += n;
    note_rect(x, y, w, h);
    if (codec == 0) {
      if (n != (size_t)w * h) return false;
      for (int r = 0; r < h; r++) memcpy(fb + (size_t)(y + r) * MFD_W + x, d + r * w, w);
    } else if (codec == 1) {
      if (!rla(d, n, x, y, w, h)) return false;
    } else return false;
  }
  return true;
}

static tinfl_decompressor inflator;
static uint8_t inflated[4096];

static void on_frame(const uint8_t *p, size_t n) {
  uint32_t t0 = micros();
  if (n < 22) { lost(SYNC_BAD); return; }
  int ver = p[0], disp = p[1], flags = p[2];
  uint32_t sid = be32(p + 4), fno = be32(p + 8);
  uint16_t part = be16(p + 12), w = be16(p + 14), h = be16(p + 16);
  bool key = flags & 1;
  if (ver != 1) return;
  if (disp != display) {
    if (!(key && part == 0)) return;
    display = disp;                              // the host moves me: take its key frame
    synced = building = false;
  }
  if (w != MFD_W || h != MFD_H) { lost(SYNC_SIZE); return; }
  if (building && b_stream == sid && b_frame == fno && b_part == part && b_key == key) {
  } else if (part == 0 && key) {
  } else if (part == 0 && synced && sid == stream_id && fno == frame_no + 1) {
  } else if (!key && synced && sid == stream_id && fno == frame_no && !building) {
    return;                                      // one I have
  } else if (!synced && !building) {
    send_sync();                                 // still waiting for a key frame
    return;
  } else {
    lost(sid != stream_id && !key ? SYNC_STREAM : SYNC_GAP);
    return;
  }
  const uint8_t *body = p + 22;
  size_t blen = n - 22;
  if (flags & 4) {
    tinfl_init(&inflator);
    size_t in_n = blen, out_n = sizeof inflated;
    tinfl_status s = tinfl_decompress(&inflator, body, &in_n, inflated, inflated, &out_n,
                                      TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (s != TINFL_STATUS_DONE) { lost(SYNC_BAD); return; }
    body = inflated;
    blen = out_n;
  } else if (blen > 4096) { lost(SYNC_BAD); return; }
  if (!apply(body, blen)) { lost(SYNC_BAD); return; }
  if (flags & 2) {                               // END: the frame is whole: on the screen with it
    building = false;
    synced = true;
    stream_id = sid;
    frame_no = fno;
    // Acknowledged as soon as it is decoded - it is on the screen at the
    // next frame. Sent after the wait instead, the acknowledgement met the
    // host's next update on the CH340, which then lost bytes coming back.
    uint8_t a[9] = { (uint8_t)display };
    put_be32(a + 1, fno);
    put_be32(a + 5, micros() - t0);
    send(0x24, a, 9);
    present();
    return;
  }
  building = true;
  b_stream = sid; b_frame = fno; b_part = part + 1; b_key = key;
  if (key) synced = false;                       // until the key frame is whole
}

// ---- what goes wrong on the link, told to the host as ERROR (the bridge logs it)
static volatile uint32_t err_fifo, err_full, err_frame, err_other;
static uint32_t bad_packets, long_packets, told_at;
static uint32_t told[6], told_slips = ~0u;       // the panel told once, then on a change

static void on_rx_error(hardwareSerial_error_t e) {
  if (e == UART_FIFO_OVF_ERROR) err_fifo++;
  else if (e == UART_BUFFER_FULL_ERROR) err_full++;
  else if (e == UART_FRAME_ERROR || e == UART_PARITY_ERROR) err_frame++;
  else err_other++;
}

static void report_errors() {
  if (millis() - told_at < 5000) return;
  told_at = millis();
  if (pv_slips != told_slips) {
    told_slips = pv_slips;
    char t[120];
    int n = snprintf(t + 1, sizeof t - 1, "panel: %lu slips in %lu frames, worst refill %lu us late",
                     (unsigned long)pv_slips, (unsigned long)pv_frames, (unsigned long)pv_worst);
    t[0] = 1;
    send(0x22, (const uint8_t *)t, 1 + n);
  }
  uint32_t now[6] = { bad_packets, long_packets, err_fifo, err_full, err_frame, err_other };
  if (!memcmp(now, told, sizeof now)) return;
  memcpy(told, now, sizeof now);
  char t[160];
  int n = snprintf(t + 1, sizeof t - 1, "link: %lu bad packets, %lu too long; UART %lu FIFO overflows, "
                   "%lu buffer full, %lu framing, %lu other",
                   (unsigned long)now[0], (unsigned long)now[1], (unsigned long)now[2],
                   (unsigned long)now[3], (unsigned long)now[4], (unsigned long)now[5]);
  t[0] = 1;
  send(0x22, (const uint8_t *)t, 1 + n);
}

// ---- messages from the host
static void on_message(const uint8_t *m, size_t n) {
  if (n < 4 || crc16(m, n - 2) != be16(m + n - 2)) { bad_packets++; return; }
  uint8_t type = m[0];
  const uint8_t *p = m + 2;
  size_t pn = n - 4;
  switch (type) {
  case 0x01: send_hello(); break;                                // HELLO?
  case 0x15: { uint8_t t[4]; put_be32(t, millis()); send(0x21, t, 4); break; }   // PING -> PONG
  case 0x16: on_frame(p, pn); break;                             // FRAME
  case 0x17:                                                     // NAME: which MFD I am, kept
    if (pn >= 1 && pn < sizeof name) {
      char was[sizeof name];
      strcpy(was, name);
      memcpy(name, p, pn);
      name[pn] = 0;
      if (strcmp(was, name)) {
        // Writing flash stops the cache, and with it the panel's refill from
        // PSRAM: restart its transfer at the next frame, in case it slipped.
        prefs.putString("name", name);
        esp_lcd_rgb_panel_restart(panel);
      }
      send_hello();
    }
    break;
  case 0x18:                                                     // TINT: my colour, R G B, kept
    if (pn == 3) {
      if (memcmp(tint, p, 3)) {
        memcpy(tint, p, 3);
        make_lut();                              // the feed colours as it scans: from the next frame
        prefs.putBytes("tint", tint, 3);
        esp_lcd_rgb_panel_restart(panel);        // as for NAME
      }
      send_hello();
    }
    break;
  default: break;                                                // lamps and text: no keys yet
  }
}

static uint8_t rx[12288], msg[12288];
static size_t rx_n;

// The panel's bounce buffers are refilled from PSRAM in an interrupt, on
// the core that made the panel: core 0, so it never holds off the UART's
// interrupt (core 1, with the decoder) - at 2 Mbaud the UART's 128-byte FIFO
// lasts 0.64 ms.
static void panel_task(void *done) {
  panel_start();
  xSemaphoreGive((SemaphoreHandle_t)done);
  vTaskDelete(NULL);
}

void setup() {
  Serial.setRxBufferSize(32768);
  Serial.begin(LINK_BAUD);
  Serial.setRxFIFOFull(64);                      // drain the FIFO half full, not nearly full
  Serial.onReceiveError(on_rx_error);
  load_name();
  make_lut();
  make_buffers();
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(panel_task, "panel", 4096, done, 5, NULL, PANEL_CORE);
  xSemaphoreTake(done, portMAX_DELAY);
  send_hello();
}

void loop() {
  uint8_t buf[512];
  report_errors();
  int got = Serial.available();
  if (got <= 0) { delay(1); return; }
  got = Serial.readBytes(buf, min(got, (int)sizeof buf));
  for (int i = 0; i < got; i++) {
    if (buf[i] != 0) {
      if (rx_n < sizeof rx) rx[rx_n++] = buf[i];
      else { if (rx_n == sizeof rx) long_packets++; rx_n = sizeof rx + 1; }   // too long: dropped at its zero
      continue;
    }
    if (rx_n && rx_n <= sizeof rx) {
      size_t n = cobs_decode(rx, rx_n, msg, sizeof msg);
      if (n) on_message(msg, n);
      else bad_packets++;
    }
    rx_n = 0;
  }
}
