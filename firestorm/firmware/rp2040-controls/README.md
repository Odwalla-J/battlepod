# rp2040-controls

The pod's throttle, stick and pedals, each an RP2040 board (a Waveshare
RP2040-Zero, or any RP2040) that Windows sees as a USB game controller. The
shim maps them by name with no setup; the launcher's Controls page shows them
live and can map anything else.

| board | USB name | USB ids | what it reports |
|---|---|---|---|
| throttle | Firestorm Throttle | 1209:0001 | X the lever; button 1 the throttle button, 2 the panic button |
| stick | Firestorm Stick | 1209:0002 | X twist, Y aim; buttons 1-4; the hat |
| pedals | Firestorm Pedals | 1209:0003 | X the left pedal, Y the right |

What the game does with them is in `../../docs/RIO.md` and the README's
*Flying it*: the throttle button reverses while held; the trigger and buttons
2 and 3 fire weapon groups 1-3 (all three, everything); button 4 does nothing
in this game; the hat looks round while held - forward down, back behind,
left and right.

## Building

1. Arduino IDE 2, with Earle Philhower's RP2040 core (Boards Manager URL
   `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`).
2. Board: *Waveshare RP2040 Zero* (or yours). **Tools > USB Stack: Adafruit
   TinyUSB.**
3. In `rp2040-controls.ino` set `#define DEVICE` to `DEVICE_THROTTLE`,
   `DEVICE_STICK` or `DEVICE_PEDALS`, and upload. Do each board in turn.

Each board needs its own product id: Windows keeps one name per USB id, and
the shim finds the boards by name. 1209 is pid.codes' open vendor id and
0001-0003 are placeholders for a build of your own, not registered numbers:
fine on your PC, but change them (all three different) if something else of
yours already uses them, and register real ones before giving boards away.

*Not yet built against real hardware: compile and test one board first.*

## Wiring

Everything to ground; the board's pull-ups do the rest. A potentiometer's
ends go to 3V3 and GND (the board's own, *not* 5V) and its wiper to the axis
pin. Hall-effect sensors with a 0-3.3 V output wire the same way.

| | GP26 | GP27 | GP0 | GP1 | GP2 | GP3 | GP4 | GP5 | GP6 | GP7 |
|---|---|---|---|---|---|---|---|---|---|---|
| throttle | lever | - | throttle button | panic | | | | | | |
| stick | X (twist) | Y (aim) | trigger | button 2 | button 3 | button 4 | hat forward | hat back | hat left | hat right |
| pedals | left pedal | right pedal | calibrate (a button or a jumper) | | | | | | | |

The pod's own stick: trigger 0x40, then 0x47, 0x46 and 0x45 on the
manual's joystick board; the hat 0x42 forward, 0x41 back, 0x44 left, 0x43
right.

## Calibrating

A lever or pedal seldom turns its potentiometer through the whole of its
range, so each board learns its own, once, and keeps it in flash:

1. Hold GP0 to ground (the throttle button, the trigger, or the pedals'
   calibrate button) while plugging the board in; let go.
2. Move every axis from end to end: the lever closed to open, the stick
   round its corners, each pedal up and down.
3. Leave everything at rest - throttle closed, stick centred, pedals up -
   and press GP0 once more. Saved.

The stick's rest becomes its centre; small differences either side are taken
up by the shim's dead zone (Controls, *USB controllers*). If an axis runs
the wrong way, reverse it there (*Learn*, or a "-" in front) rather than in
the firmware.

## Notes

- The game calibrates too, from what it sees each mission; the shim shows it
  the throttle closed and the pedals up for half a second whenever it starts
  over, so a lever left open does not become "closed".
- Two 12-bit axes are plenty: the game takes the throttle in 800 steps, the
  stick in 75 each way.
