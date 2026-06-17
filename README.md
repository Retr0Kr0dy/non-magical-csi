# non magical csi - v2.1

> no magical radar

Firmware for the M5Stack Cardputer that reads raw WiFi Channel State Information (CSI) from the ESP32-S3 radio, visualises it in real time, collects labelled training data for on-device ML models, and manages those files on-device. Built for studying what CSI actually is. For demo and educational purposes.

<p align="center"><sub><i>Knowledge should be free</i></sub></p>

## what it is

When your ESP32 receives a WiFi frame, the hardware records how each OFDM subcarrier was attenuated and phase-shifted by the environment - that is the channel state. When something moves through the room, multipath reflections change, and those measurements change too.

This firmware captures those measurements, draws them live on screen, runs a motion detector on top, guides you through structured data-collection sessions to build ML training datasets, and lets you browse and manage the collected files directly on the device.

## what it is not

- Not radar. There is no angle, no distance, no localisation.
- Not reliable for fine-grained gestures with a single AP.
- Not a finished product. Calibration drifts. Environment changes break it.
- Not magic. Every effect you see has a boring RF explanation.

**Passive mode** frame rate depends on ambient WiFi traffic: beacons give ~10 fps per AP on the channel, data traffic adds more. Up to ~50 fps near an active device.

**Active injection mode** sends 802.11 Probe Request frames at 10 Hz with per-frame SA MAC rotation, causing the target AP to reply with Probe Responses. Rate is intentionally kept at 10 Hz to avoid exhausting the AP's per-source-address rate-limit bucket. Frame injection uses a libnet80211 symbol-weakening patch (wsl_bypasser technique) applied at build time via `scripts/patch_libnet80211.py`.

## hardware

**Primary**: M5Stack Cardputer ADV - ESP32-S3, ST7789 240x135, TCA8418 keyboard, ES8311 codec

**Secondary**: M5Stack Cardputer v1/v1.1 - NS4168 amp, 74HC138 keyboard matrix

## views

| key | view |
|-----|------|
| 1 | LOS - calibrated disturbance detector with audio alert |
| 2 | Spectrum - amplitude waterfall per subcarrier |
| 3 | Variance - which subcarriers fluctuate most |
| 4 | Motion - scalar score + sparkline history |
| 5 | Correlation - cross-subcarrier coherence |
| 6 | ChanOcc - passive per-channel frame-rate survey |
| 7 | Console - serial terminal mirrored on screen |
| 8 | Training - guided ML data-collection session |
| 9 | Files - SD card file browser |

Navigation: arrow keys or WASD. ESC / backspace = back to menu.

WiFi is stopped automatically when in the menu or Files view to save power. It restarts the moment you enter any sensing mode.

**Global keys** (work in all modes):
- **C** = cycle channel 1->2->...->13->1
- **+/-** = channel up/down
- **P** = toggle active / passive injection (not available in Console, Motion, or Corr)
- **ESC / M** = back to menu

## LOS detector

Press **F** to scan for APs, select one, stand still during calibration, three ascending beeps = armed.

Measures how much frame-to-frame channel differences exceed the calibrated baseline (z-score on differential amplitude). Score 0-100.

**Calibration** collects a baseline in passive mode (80 frames, ~40-80 s) or active injection mode (100 frames, ~10 s at 10 fps).

**Active injection** is on by default. Press **P** in LOS, Spectrum, Variance, or Training mode to toggle between active and passive. ChanOcc is always passive.

In LOS mode: **F** = scan APs, **P** = toggle active/passive, **R** = recalibrate, **C** = cycle channel, **Q** = quit.

**Audio alerts** - Geiger-counter style: beep rate scales continuously with score. Below 20 = silence.

| Score | Beep rate | Tone |
|-------|-----------|------|
| 20-35 | 2 Hz | 800 Hz |
| 35-50 | 5 Hz | 950 Hz |
| 50-65 | 11 Hz | 1200 Hz |
| 65-80 | 22 Hz | 1500 Hz |
| >80 | 22 Hz | 2000 Hz |

## channel occupation (ChanOcc)

Passive ambient survey. Hops through channels 1-13, dwelling 500 ms per channel (100 ms hardware settle + 400 ms count window). Displays passive frame-rate as horizontal bars for all 13 channels simultaneously.

- Bars are auto-scaled to the **historical peak** seen this session - scale never shrinks.
- A white tick marks the per-channel peak on each bar.
- Stale fps values (zero on current sweep) are shown dimmed in grey.
- **P** toggles active/passive injection. In passive mode, beacon frames are the main signal source (~10 fps per AP). In active mode, broadcast probe requests are sent on each channel during the dwell window, prompting probe responses from nearby APs and increasing the count.
- OFDM beacons/responses (802.11g/n, >=6 Mbps) produce valid CSI; legacy DSSS frames (802.11b, 1 Mbps) do not.

## training mode

Guided structured data-collection sessions that produce labelled CSV files for ML training.

### workflow

1. Enter Training (key 8)
2. Choose a procedure with W/S, press Enter
3. Type a session name (alphanumeric, `_`, `-`; Enter to confirm; ESC to cancel)
4. Follow the on-screen instructions - audio cues tell you when to move and when to hold still
5. CSV is written to SD card at `/non-magical-csi/<board>/<name>_<proc>.csv`

### audio cues

- **3 descending beeps** (1400->900->600 Hz) = transition phase - move to next position
- **3 ascending beeps** (600->900->1400 Hz) = capture phase - hold still, recording starts
- **3 rising beeps at end** = session complete

### procedures

**indoor** (~100 s) - walk the door-to-device axis in a room. Labels: `absent`, `approach`, `still_mid`, `still_near`, `recede`. Requires line-of-sight between door and device.

**outdoor** (~50 s) - linear distance sweep, walk ~10 m away and return. Labels: `still_near`, `recede`, `still_far`, `approach`.

**zone** (~80 s) - distance sweep along the device axis, near to far and back. Labels: `still_near`, `still_mid`, `still_far`, `approach`, `recede`, `absent`. Covers the full shared label set so zone + free sessions combine cleanly.

**free** - record any label at any time. Labels are drawn from whichever guided procedure was highlighted last. R=repeat, N=new label, ESC=finish. All captures append to one file.

### CSV format

```
ts_ms,label,rssi,ch,a00,a01,...,a55
```

56 LLTF HT20 subcarrier amplitudes per row. Transition rows (null label steps) are never written.

### data collection strategy

- **Active injection on** (press P before starting) - gives stable 10 fps regardless of ambient traffic. Passive mode fps is unpredictable and makes temporal features unreliable.
- **Multiple sessions** - collect at least 5-10 runs of each procedure in each environment. CSI fingerprints are highly environment-specific: furniture, people walking past, even temperature changes can shift them.
- **Vary conditions** - different times of day, different people, door open vs closed. A model trained in one rigid condition will not generalise.
- **Session naming** - name sessions meaningfully. There is no RTC on the ESP32; timestamps are milliseconds since boot and are useless across sessions.
- **Check ChanOcc first** - use the channel occupation view to confirm your AP is active and delivering consistent fps on the chosen channel before starting a training session.

## file manager (v2.1)

Browse and delete files on the SD card without removing it from the device. Access via key 9 from the menu.

- W/S or arrow keys: scroll through files and directories
- Enter or D: open a directory / confirm delete for a file
- A or Back: go up one directory level
- ESC or M: back to the menu

Selecting a file shows a delete-confirm screen. Only deletion is supported - CSV data files are not meant to be edited on-device.

WiFi is stopped while the file manager is open.

## ML pipeline *(work in progress)*

The firmware collects labelled CSV data. What you do with it on a PC and how you eventually run inference on the device is covered in **[ML_PIPELINE.md](ML_PIPELINE.md)**.

Short version: each frame is 56 subcarrier amplitudes. With enough labelled sessions you can train a small 1D CNN, quantize it to int8, and deploy it on the ESP32-S3 via ESP-DL (Espressif's native inference library, exploits S3 SIMD) or TFLite Micro as a fallback. The fingerprint is environment-specific - expect to retrain per deployment. On-device inference is the planned next step, not yet implemented.

## why the DC null gap in the spectrum

OFDM WiFi reserves subcarrier 32 as a DC null. Subcarriers 0-3 and 61-63 are guard bands. The gap in the middle of the spectrum view is expected and correct.

## build

Dependencies are vendored in `lib/`. No internet required at build time.

```sh
pio run -e m5-cardputer-adv              # build
pio run -e m5-cardputer-adv -t upload   # build + flash
```

The pre-build script `scripts/patch_libnet80211.py` weakens the `ieee80211_raw_frame_sanity_check` symbol in the ESP32 WiFi library so that `esp_wifi_80211_tx()` accepts all management frame subtypes. The patch is idempotent and survives incremental builds.

Output binary: `bin/non-magical-csi-<version>-<env>.bin` (factory merged, flash at 0x0).

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
los start               begin LOS sequence (countdown -> calibrate -> scan)
los stop                stop
los recal               recalibrate baseline
```

## references

- ESP32 CSI Toolkit - Hernandez & Kubisch (2021)
- espressif/esp-csi - Espressif Systems (2023)
- espressif/esp-dl - Espressif Systems (2024)
- WiSee: Whole-Home Gesture Recognition - Patel et al., MobiCom 2013
- LLTF subcarrier mapping - ESP-IDF wifi_csi_info_t documentation
