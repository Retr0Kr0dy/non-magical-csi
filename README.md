# non magical csi

> no magical radar

Firmware for the M5Stack Cardputer that reads raw WiFi Channel State Information (CSI) from the ESP32-S3 radio and visualises it in real time. Built for studying what CSI actually is. For demo and educational purposes.

<p align="center"><sub><i>Knowledge should be free 🏴‍☠️</i></sub></p>

## what it is

When your ESP32 receives a WiFi frame, the hardware records how each OFDM subcarrier was attenuated and phase-shifted by the environment - that is the channel state. When something moves through the room, multipath reflections change, and those measurements change too.

This firmware captures those measurements, draws them live on screen, and runs a simple motion detector on top.

## what it is not

- Not radar. There is no angle, no distance, no localisation.
- Not reliable for fine-grained gestures or small movements.
- Not a finished product. Calibration drifts. Environment changes break it.
- Not magic. Every effect you see has a boring RF explanation.

**Passive mode** frame rate depends on ambient WiFi traffic: 1–2 fps in a quiet room, up to ~50 fps near an active device.

**Active injection mode** sends 802.11 Probe Request frames at ~100 Hz with per-frame SA MAC rotation, causing the target AP to reply with Probe Responses. This bypasses the AP's per-source-address rate limit and raises CSI collection to 70–300 fps depending on AP behaviour. Frame injection uses a libnet80211 symbol-weakening patch (wsl_bypasser technique) applied at build time via `scripts/patch_libnet80211.py`.

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

In LOS mode: **F** = scan APs, **P** = toggle active/passive injection, **R** = recalibrate, **C** = cycle channel, **Q** = quit.

## LOS detector

Press **F** to scan for APs → select one → stand still during calibration → three ascending beeps = armed.

The detector measures how much frame-to-frame channel differences exceed the calibrated baseline (z-score on differential amplitude). Score 0–100.

**Calibration** collects a baseline in passive mode (80 frames, ~40–80 s) or active injection mode (500 frames, ~10 s at 50+ fps).

**Active injection** is on by default. Press **P** in LOS mode to toggle between active (fast calibration, higher fps) and passive (AP traffic only).

**Audio alerts** use a Geiger-counter style: beep rate scales continuously with score. Below 20 = silence.

| Score | Beep rate | Tone |
|-------|-----------|------|
| 20–35 | 2 Hz | 800 Hz |
| 35–50 | 5 Hz | 950 Hz |
| 50–65 | 11 Hz | 1200 Hz |
| 65–80 | 22 Hz | 1500 Hz |
| >80 | 22 Hz | 2000 Hz |

## why the DC null gap in the spectrum

OFDM WiFi reserves subcarrier 32 as a DC null. Subcarriers 0–3 and 61–63 are guard bands. The gap in the middle of the spectrum view is expected and correct. The firmware marks it explicitly.

## build

Dependencies are vendored in `lib/`. No internet required at build time.

```sh
pio run -e m5-cardputer-adv              # build
pio run -e m5-cardputer-adv -t upload   # build + flash
```

The pre-build script `scripts/patch_libnet80211.py` weakens the `ieee80211_raw_frame_sanity_check` symbol in the ESP32 WiFi library so that `esp_wifi_80211_tx()` accepts all management frame subtypes. The patch is idempotent (sentinel file) and survives incremental builds.

## serial commands

```
csi start [ch]          start CSI on channel (default 6)
csi stop                stop
csi scan                scan for nearby APs
csi ap <idx>            lock onto scanned AP (sets BSSID filter)
csi ch <N>              change channel
csi active start        start active probe injection
csi active stop         stop injection
csi info                frame count, fps, per-subcarrier stats
los start               begin LOS sequence (countdown → calibrate → scan)
los stop                stop
los recal               recalibrate baseline
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
