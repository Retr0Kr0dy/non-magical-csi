/*
 * wsl_bypasser.c - Override for ieee80211_raw_frame_sanity_check().
 *
 * Problem
 * -------
 * Espressif's closed-source libnet80211.a contains a strong symbol
 * ieee80211_raw_frame_sanity_check() that inspects each frame passed to
 * esp_wifi_80211_tx() and returns an error for management frame types
 * (deauth, disassoc, beacon, probe, etc.), silently dropping them.
 *
 * This prevents sending raw 802.11 management frames even when the driver
 * is in promiscuous / raw-tx mode — which breaks deauth injection, beacon
 * flooding, and similar features.
 *
 * Solution
 * --------
 * The pre-build script scripts/patch_libnet80211.py runs objcopy
 *   --weaken-symbol=ieee80211_raw_frame_sanity_check
 * on libnet80211.a before compilation.  Once the symbol is weak, the
 * linker prefers this strong definition over the library's weak one.
 *
 * This override returns 0 (ESP_OK) unconditionally, allowing all frame
 * types to pass through esp_wifi_80211_tx() without modification.
 *
 * Reference
 * ---------
 * This approach is known as "wsl_bypasser" and is widely used in the
 * ESP32 security research / wardriving community.
 * Original concept: https://github.com/risinek/esp-wifi-penetration-tool
 */
#ifdef ARDUINO_ARCH_ESP32

#include <stdint.h>

/*
 * ieee80211_raw_frame_sanity_check - permissive override.
 *
 * The real function signature is not documented; based on binary analysis
 * and community research it takes three 32-bit arguments (frame type,
 * length, flags) and returns 0 on pass or non-zero to reject.
 * We always return 0.
 */
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    (void)arg;
    (void)arg2;
    (void)arg3;
    return 0;
}

#endif /* ARDUINO_ARCH_ESP32 */
