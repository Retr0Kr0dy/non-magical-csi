/*
 * hal_core.cpp — Arduino/ESP32-specific backend: sic_sysinfo().
 *
 * All platform-agnostic core functions (sic_begin, sic_has, sic_ir_send_nec,
 * sic_charger_state, …) live in src/core/sic_core.c.
 *
 * To port to a new platform, implement the backend contract documented in
 * include/sic/sic_backend.h and provide your own sic_sysinfo().
 */
#if defined(ARDUINO) || defined(SIC_BACKEND_ARDUINO)

#include <Arduino.h>
#include <esp_system.h>
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>   /* IDF 5.x: esp_efuse_mac_get_default lives here */
#endif

extern "C" {
#include "sic/hal.h"
}

extern "C" int sic_sysinfo(sic_sysinfo_t* out) {
    if (!out) return -1;
    out->chip_model  = "ESP32";
    out->chip_rev    = 0;
    out->cpu_mhz     = (uint32_t)(F_CPU / 1000000ul);
    out->flash_bytes = 8u * 1024u * 1024u;
    out->flash_hz    = 80000000u;
    out->psram_bytes = 0;

    uint8_t mac[6] = {0};
#if __has_include(<esp_mac.h>)
    esp_efuse_mac_get_default(mac);   /* reads eFuse base MAC, no WiFi init needed */
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    for (int i = 0; i < 6; i++) out->mac[i] = mac[i];
    return 0;
}

#endif /* SIC_BACKEND_ARDUINO */
