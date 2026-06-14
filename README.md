# non magical csi

> no magical radar

Firmware for the M5Stack Cardputer that reads raw WiFi Channel State Information (CSI) from the ESP32-S3 radio and visualises it in real time. Built for studying what CSI actually is. For demo and educational purposes.

## what it is

When your ESP32 receives a WiFi frame, the hardware records how each OFDM subcarrier was attenuated and phase-shifted by the environment - that is the channel state. When something moves through the room, multipath reflections change, and those measurements change too.

This firmware captures those measurements, draws them live on screen, and runs a simple motion detector on top.

## what it is not

- Not radar. There is no angle, no distance, no localisation.
- Not reliable for fine-grained gestures or small movements.
- Not a finished product. Calibration drifts. Environment changes break it.
- Not magic. Every effect you see has a boring RF explanation.

The frame rate depends entirely on ambient WiFi traffic: 1–2 fps passively in a quiet room, up to ~50 fps if a device on the same channel is actively transferring data.

## hardware

**Primary**: M5Stack Cardputer ADV - ESP32-S3, ST7789 240×135, TCA8418 keyboard, ES8311 codec

**Secondary**: M5Stack Cardputer v1/v1.1 - NS4168 amp, 74HC138 keyboard matrix

## views

| key | view |
|-----|------|
| 1 | LOS - calibrated disturbance detector with audio alert |
| 2 | Spectrum - amplitude waterfall per subcarrier |
| 3 | Variance - which subcarriers fluctuate most |
| 4 | Motion - scalar score + sparkline history |
| 5 | Correlation - cross-subcarrier coherence matrix |
| 6 | Console - serial terminal mirrored on screen |

Navigation: arrow keys or WASD. ESC / backspace = back to menu.

## LOS detector

Press **F** to scan for APs -> select one -> stand still for 5 s calibration -> three ascending beeps = armed.

The detector measures how much current frame-to-frame channel differences exceed the calibrated baseline (z-score on differential amplitude). Score 0–100. Audio alert rate scales with score. The threshold is 20/100 - below that, silence.

## why the DC null gap in the spectrum

OFDM WiFi reserves subcarrier 32 as a DC null. Subcarriers 0–3 and 61–63 are guard bands. The gap in the middle of the spectrum view is expected and correct. The firmware marks it explicitly.

## build

Dependencies are vendored in `lib/`. No internet required at build time.

```sh
pio run -e m5-cardputer-adv        # build
pio run -e m5-cardputer-adv -t upload  # build + flash
```

## release

Releases are built via GitHub Actions (Actions -> Build and Release -> Run workflow). Provide a tag and optional notes. Both targets are built and attached as factory-flash binaries.

Flash a release binary:

```sh
esptool.py write_flash 0x0 csi-sense-v1.0.0-m5-cardputer-adv.bin
```

## references

- ESP32 CSI Toolkit - Hernandez & Kubisch (2021)
- espressif/esp-csi - Espressif Systems (2023)
- WiSee: Whole-Home Gesture Recognition - Patel et al., MobiCom 2013
- LLTF subcarrier mapping - ESP-IDF wifi_csi_info_t documentation
