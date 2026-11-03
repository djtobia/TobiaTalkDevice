# LED ring box (idea, not built yet)

A small box between the wall charger and the board. Charger -> box -> board's USB-C. The box powers an LED ring
from the same 5 V and sends it colour data over the USB cable's **D+ wire**, so the family needs no extra cables.

## What the schematic says (Waveshare `ESP32-S3-Touch-ePaper-1.54-Schematic.pdf`, read 2026-10-29)
- USB-C D+ goes to **GPIO20**, D- to **GPIO19** (nets `U_P` / `U_N`). Straight to the chip.
- **CC1 and CC2 each have 5.1 k to ground**: the board is a plain power *sink*. Any USB-C source that presents Rp
  on CC will power it. The SBU pins are not connected.
- VBUS -> diode -> VSYS -> charger chip. Nothing else is on the USB connector.
- The 2 x 6 header also exposes IO19, IO20, IO1, IO2, IO3 (free), IO43/IO44, IO47/IO48 (I2C, in use), 3V3, GND, VSYS,
  if a wire to the board is ever preferred.

## Parts
| Part | Notes |
|---|---|
| WS2812B/NeoPixel ring, 16 LEDs | 12 also fine. 16 at full white is ~1 A, so the firmware caps brightness (~25%, ~250 mA). |
| 2 x USB-C female breakout (16-pin, power + D+/D- broken out) | Input one **with 5.1 k CC resistors** (or add two 5.1 k to ground on CC1, CC2). |
| 2 x 22 k resistors | Output port: CC1 -> 5 V and CC2 -> 5 V (Rp, "up to 1.5 A"). Makes the box a power source. |
| 1 x 330 ohm resistor | In series with the data wire. |
| 1 x 470-1000 uF capacitor, 6.3 V+ | Across the ring's 5 V / GND, at the ring. |
| 1 x 1N4001 (or 1N4007) diode | In series with the ring's 5 V so the ring sees ~4.2-4.4 V and accepts 3.3 V data reliably. Use an ordinary silicon diode, NOT a Schottky (1N5819 drops only 0.3 V, too little). Band end points toward the ring. |
| Data-capable USB-C cable | Must carry D+. Many cheap cables are charge-only; ship a tested cable with each box. |
| 5 V >= 2 A USB charger | |

## Wiring
```
 charger -> [IN port]                         [OUT port] -> USB-C cable -> board
   VBUS ----------------+------------------------- VBUS
   GND  ----------------+------------------------- GND
   CC1, CC2: 5.1k each to GND   (input is a sink)        CC1, CC2: 22k each to VBUS   (output is a source)
   D+/D-: not connected                                  D+ (tie A6 + B6) --330R--> ring DIN
                                                         D-: not connected
   VBUS ---1N4001---+---- ring 5V        (470-1000uF across ring 5V and GND)
   GND  ------------+---- ring GND
```
The output port's D+ goes **only** to the ring. Never connect the input port's data pins (wall chargers short D+/D-).

## Firmware plan
- Setup-page option "LED ring" (off by default). On: release the USB pins, drive GPIO20 as the data line,
  GPIO19 as a plain input. USB serial is lost while on (BOOT-button download mode still flashes; OTA works).
- Each board picks a colour in setup; the colour travels with every message; ring shows the sender's colour for a few seconds.
- Brightness cap in firmware.

## First test (before any soldering)
Breadboard: a ring on a 5 V supply, data from GPIO1 (free header pin) first, then move to GPIO20.
