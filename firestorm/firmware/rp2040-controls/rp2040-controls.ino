// rp2040-controls: the pod's throttle, stick or pedals as a USB joystick.
//
// One sketch for three boards (Waveshare RP2040-Zero, or any RP2040); pick
// which with DEVICE below. Each is a USB game controller with two 12-bit
// axes, eight buttons and a hat, named so the shim maps it with no setup
// (shim.ini [joy], src/ddraw.c "USB controllers"):
//
//   DEVICE_THROTTLE  "Firestorm Throttle"  X the lever; button 1 the throttle
//                                          button (reverse while held), 2 panic
//   DEVICE_STICK     "Firestorm Stick"     X twist, Y aim; buttons 1-4 trigger,
//                                          group 2, group 3, button 4; the hat
//   DEVICE_PEDALS    "Firestorm Pedals"    X the left pedal, Y the right
//
// Wiring: README.md. Build with Earle Philhower's arduino-pico core, Tools >
// USB Stack: "Adafruit TinyUSB".
//
// Calibration, kept in flash: plug in holding GP0 to ground (the throttle
// button, the trigger, or on the pedals a button or a jumper), let go, move
// every axis end to end, leave everything at rest - throttle closed, stick
// centred, pedals up - and press GP0 again. Until then the axes read their
// whole electrical range.

#include <Adafruit_TinyUSB.h>
#include <EEPROM.h>

#define DEVICE_THROTTLE 1
#define DEVICE_STICK    2
#define DEVICE_PEDALS   3

#define DEVICE DEVICE_STICK          // <- which board this is

#if DEVICE == DEVICE_THROTTLE
static const char *NAME = "Firestorm Throttle";
static const uint16_t PID = 0x0001;
static const int AXES = 1, AXIS_PIN[2] = { 26, 27 };
static const bool CENTRES[2] = { false, false };
static const int BUTTONS = 2, BUTTON_PIN[8] = { 0, 1 };
static const bool HAS_HAT = false;
#elif DEVICE == DEVICE_STICK
static const char *NAME = "Firestorm Stick";
static const uint16_t PID = 0x0002;
static const int AXES = 2, AXIS_PIN[2] = { 26, 27 };
static const bool CENTRES[2] = { true, true };
static const int BUTTONS = 4, BUTTON_PIN[8] = { 0, 1, 2, 3 };
static const bool HAS_HAT = true;
static const int HAT_PIN[4] = { 4, 5, 6, 7 };   // up (forward), down, left, right
#elif DEVICE == DEVICE_PEDALS
static const char *NAME = "Firestorm Pedals";
static const uint16_t PID = 0x0003;
static const int AXES = 2, AXIS_PIN[2] = { 26, 27 };
static const bool CENTRES[2] = { false, false };
static const int BUTTONS = 0, BUTTON_PIN[8] = { 0 };
static const bool HAS_HAT = false;
#endif
static const uint16_t VID = 0x1209;             // pid.codes; see README.md
static const int CAL_PIN = 0;

// A joystick: 8 buttons, a hat (0-7 clockwise from up, 8 = centred), X and Y 0-4095.
static const uint8_t DESCRIPTOR[] = {
  0x05, 0x01, 0x09, 0x04, 0xA1, 0x01,                          // Generic Desktop, Joystick, Application
  0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01,  //   Buttons 1-8
  0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
  0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,              //   Hat switch, 0-7, null outside
  0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14,                    //   0-315 degrees
  0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
  0x65, 0x00, 0x45, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03,  //   padding
  0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x0F,        //   X, Y 0-4095
  0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
  0xC0,
};

struct __attribute__((packed)) Report { uint8_t buttons, hat; uint16_t x, y; };

struct Cal { uint32_t magic; uint16_t lo[2], mid[2], hi[2]; };
static const uint32_t MAGIC = 0x46534331;        // "FSC1"
static Cal cal;

Adafruit_USBD_HID hid;
static Report last;
static uint32_t last_sent;

// An axis, oversampled and smoothed: 0-4095 raw.
static uint16_t read_axis(int a) {
  static uint32_t smooth[2];
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(AXIS_PIN[a]);
  sum /= 16;
  smooth[a] = smooth[a] ? (smooth[a] * 3 + sum * 16) / 4 : sum * 16;   // x16 fixed point
  return smooth[a] / 16;
}

// Raw to 0-4095 through the calibration: ends to ends, a centring axis's rest to 2048.
static uint16_t scale(int a, uint16_t raw) {
  long lo = cal.lo[a], hi = cal.hi[a], mid = cal.mid[a], v;
  if (hi <= lo) return raw;
  if (CENTRES[a] && mid > lo && mid < hi)
    v = raw <= mid ? 2048L * (raw - lo) / (mid - lo) : 2048 + 2047L * (raw - mid) / (hi - mid);
  else
    v = 4095L * (raw - lo) / (hi - lo);
  return v < 0 ? 0 : v > 4095 ? 4095 : v;
}

// Buttons, each steady for 5 ms before it counts.
static uint8_t read_buttons() {
  static uint8_t stable, seen;
  static uint32_t since;
  uint8_t now = 0;
  for (int b = 0; b < BUTTONS; b++) if (digitalRead(BUTTON_PIN[b]) == LOW) now |= 1 << b;
  if (now != seen) { seen = now; since = millis(); }
  else if (millis() - since >= 5) stable = seen;
  return stable;
}

static uint8_t read_hat() {
  if (!HAS_HAT) return 8;
#if DEVICE == DEVICE_STICK
  bool u = digitalRead(HAT_PIN[0]) == LOW, d = digitalRead(HAT_PIN[1]) == LOW;
  bool l = digitalRead(HAT_PIN[2]) == LOW, r = digitalRead(HAT_PIN[3]) == LOW;
  if (u && r) return 1;
  if (d && r) return 3;
  if (d && l) return 5;
  if (u && l) return 7;
  if (u) return 0;
  if (r) return 2;
  if (d) return 4;
  if (l) return 6;
#endif
  return 8;
}

static void calibrate() {
  for (int a = 0; a < 2; a++) { cal.lo[a] = 4095; cal.hi[a] = 0; }
  while (digitalRead(CAL_PIN) == LOW) delay(10);          // let go first
  delay(200);
  while (digitalRead(CAL_PIN) == HIGH) {                   // sweep, then press again
    for (int a = 0; a < AXES; a++) {
      uint16_t v = read_axis(a);
      if (v < cal.lo[a]) cal.lo[a] = v;
      if (v > cal.hi[a]) cal.hi[a] = v;
    }
#ifdef LED_BUILTIN
    digitalWrite(LED_BUILTIN, (millis() / 100) & 1);       // a fast blink, where there is a plain LED
#endif
    delay(2);
  }
  for (int a = 0; a < AXES; a++) cal.mid[a] = read_axis(a);  // the rest position
  cal.magic = MAGIC;
  EEPROM.put(0, cal);
  EEPROM.commit();
  while (digitalRead(CAL_PIN) == LOW) delay(10);
}

void setup() {
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);
  TinyUSBDevice.setID(VID, PID);
  TinyUSBDevice.setManufacturerDescriptor("BattleTech Preservation");
  TinyUSBDevice.setProductDescriptor(NAME);
  hid.setPollInterval(1);
  hid.setReportDescriptor(DESCRIPTOR, sizeof DESCRIPTOR);
  hid.begin();
  if (TinyUSBDevice.mounted()) { TinyUSBDevice.detach(); delay(10); TinyUSBDevice.attach(); }

  analogReadResolution(12);
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  pinMode(CAL_PIN, INPUT_PULLUP);
  for (int b = 0; b < BUTTONS; b++) pinMode(BUTTON_PIN[b], INPUT_PULLUP);
#if DEVICE == DEVICE_STICK
  for (int h = 0; h < 4; h++) pinMode(HAT_PIN[h], INPUT_PULLUP);
#endif

  EEPROM.begin(256);
  EEPROM.get(0, cal);
  if (cal.magic != MAGIC)
    for (int a = 0; a < 2; a++) { cal.lo[a] = 0; cal.hi[a] = 4095; cal.mid[a] = 2048; }
  delay(20);
  if (digitalRead(CAL_PIN) == LOW) calibrate();
}

void loop() {
  Report r;
  r.buttons = read_buttons();
  r.hat = read_hat();
  r.x = AXES > 0 ? scale(0, read_axis(0)) : 2048;
  r.y = AXES > 1 ? scale(1, read_axis(1)) : 2048;
  // Send on a change, and at least every 20 ms; the host polls each 1 ms.
  if (hid.ready() && (memcmp(&r, &last, sizeof r) || millis() - last_sent >= 20)) {
    hid.sendReport(0, &r, sizeof r);
    last = r;
    last_sent = millis();
  }
  delay(1);
}
