/*
 * mod_csi.cpp — WiFi Channel State Information collection for ESP32
 *
 * What this actually does
 * ───────────────────────
 * The ESP32 WiFi hardware measures the complex channel response H[k] for each
 * OFDM subcarrier k every time it receives an 802.11 frame.  We read the raw
 * LLTF (Legacy Long Training Field) amplitudes: |H[k]| = sqrt(Im^2 + Re^2).
 *
 * When a person (or object) moves through the room, multipath reflections
 * change, causing |H[k]| to fluctuate.  We detect those fluctuations.
 *
 * Honest limitations
 * ──────────────────
 *   • This is NOT radar.  We observe fading, not angle/distance directly.
 *   • Reliable for large motion (walking) within ~3 m, same room.
 *   • Frame rate depends on AP traffic: typically 10–100 fps.
 *   • ESP32 LLTF gives 56 valid subcarriers at 20 MHz (vs hundreds for
 *     research-grade setups).  Resolution is coarse.
 *   • Environment-sensitive: temperature, people walking outside, furniture
 *     rearrangement all change the baseline.
 *
 * References
 * ──────────
 *   ESP32 CSI Toolkit — Hernandez & Kubisch (2021)
 *   espressif/esp-csi — Espressif Systems (2023)
 *   "WiSee" — Patel et al., MobiCom 2013
 */

#include <Arduino.h>
#include <esp_wifi.h>
#include <string.h>
#include <math.h>
#include "mod_csi.h"
#include "app.h"

/* ── Valid LLTF HT-20 buffer indices ────────────────────────────────────── */
/* buf[k*2]=Im, buf[k*2+1]=Re.  Valid k: [4..31] and [33..60].
 * k=32 is DC; k=0..3 and k=61..63 are guard bands.                         */
static const uint8_t kVI[CSI_N_SUB] = {
     4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,
    20,21,22,23,24,25,26,27,28,29,30,31,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60
};

/* ── SPSC ring buffer: WiFi task → main task ─────────────────────────────── */
static csi_frame_t  s_ring[CSI_RING_SIZE];
static volatile uint32_t s_head = 0;    /* written by WiFi task (Xtensa: atomic 32-bit) */
static volatile uint32_t s_tail = 0;    /* read    by main task                         */

/* ── Online per-subcarrier statistics (Welford) ─────────────────────────── */
static float    s_mean[CSI_N_SUB];
static float    s_M2[CSI_N_SUB];       /* sum of squared deviations          */
static uint32_t s_stats_n = 0;
static uint32_t s_frame_total = 0;

/* ── FPS counter ─────────────────────────────────────────────────────────── */
static uint32_t s_fps = 0, s_fps_cnt = 0, s_fps_t0 = 0;

/* ── AP scan results ─────────────────────────────────────────────────────── */
#define CSI_MAX_APS 16
typedef struct { char ssid[33]; uint8_t bssid[6]; int8_t rssi; int ch; } ap_t;
static ap_t s_aps[CSI_MAX_APS];
static int  s_ap_n = 0;

/* ── BSSID filter ────────────────────────────────────────────────────────── */
static uint8_t s_target[6];
static bool    s_filter = false;

/* Alpha-max beta-min: |sqrt(I²+Q²)| ≈ α·max + β·min, error < 4%.
 * α=1, β=0.5 — single shift, no float.                                      */
static inline uint8_t amp8(int8_t I, int8_t Q) {
    int a = I < 0 ? -I : I;
    int b = Q < 0 ? -Q : Q;
    int v = (a > b ? a : b) + ((a < b ? a : b) >> 1);
    return v > 255 ? 255 : (uint8_t)v;
}

static void csi_cb(void* ctx, wifi_csi_info_t* d) {
    (void)ctx;
    if (!d || !d->buf || d->len < 128) return;

    if (s_filter) {
        for (int i = 0; i < 6; i++)
            if (d->mac[i] != s_target[i]) return;
    }

    uint32_t nxt = (s_head + 1) & CSI_RING_MASK;
    if (nxt == s_tail) return;   /* ring full — drop */

    csi_frame_t *f = &s_ring[s_head];
    f->ts_ms   = (uint32_t)millis();
    f->rssi    = d->rx_ctrl.rssi;
    f->channel = d->rx_ctrl.channel;
    f->fwi     = d->first_word_invalid ? 1 : 0;

    const int8_t *buf = (const int8_t *)d->buf;
    int off = d->first_word_invalid ? 2 : 0;  /* skip corrupt first word */

    for (int i = 0; i < CSI_N_SUB; i++) {
        int k = kVI[i] + off;
        if (k * 2 + 1 >= (int)d->len) { f->amp[i] = 0; continue; }
        f->amp[i] = amp8(buf[k * 2], buf[k * 2 + 1]);
    }

    s_head = nxt;   /* commit (Xtensa aligned 32-bit write is atomic)        */
    s_fps_cnt++;
    s_frame_total++;

    uint32_t dt = f->ts_ms - s_fps_t0;
    if (dt >= 1000) {
        s_fps    = s_fps_cnt * 1000 / dt;
        s_fps_cnt = 0;
        s_fps_t0  = f->ts_ms;
    }
}

static void wifi_sta_start(int channel) {
    /* Clean slate — ignore errors if not yet initialized */
    esp_wifi_stop();
    esp_wifi_deinit();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);   /* keep radio always-on for max CSI rate */
    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
}

static void csi_enable(void) {
    wifi_csi_config_t c = {};
    c.lltf_en           = true;
    c.htltf_en          = false;   /* LLTF only: lower latency, 128 bytes/frame */
    c.stbc_htltf2_en    = false;
    c.ltf_merge_en      = true;    /* merge LTF estimates across sub-frames  */
    c.channel_filter_en = true;    /* HW IIR — accepts more frame types, higher fps */
    c.manu_scale        = false;
    c.shift             = 0;
    esp_wifi_set_csi_config(&c);
    esp_wifi_set_csi_rx_cb(csi_cb, nullptr);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_csi(true);
}

int csi_init(int channel) {
    s_tail = 0; s_head = 0;
    s_frame_total = s_fps = s_fps_cnt = 0;
    s_fps_t0 = millis();
    memset(s_mean, 0, sizeof s_mean);
    memset(s_M2,   0, sizeof s_M2);
    s_stats_n = 0;

    wifi_sta_start(channel);
    csi_enable();

    g_app.wifi_channel = channel;
    g_app.csi_running  = true;
    return 0;
}

void csi_deinit(void) {
    esp_wifi_set_csi(false);
    esp_wifi_set_promiscuous(false);
    g_app.csi_running = false;
}

void csi_set_channel(int ch) {
    esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE);
    g_app.wifi_channel = ch;
}

void csi_select_ap(int idx) {
    if (idx < 0 || idx >= s_ap_n) return;
    memcpy(s_target, s_aps[idx].bssid, 6);
    s_filter = true;
    strncpy(g_app.ap_ssid, s_aps[idx].ssid, 32);
    memcpy(g_app.ap_bssid, s_aps[idx].bssid, 6);
    csi_set_channel(s_aps[idx].ch);
}

void csi_select_any(int channel) {
    s_filter = false;
    memset(s_target, 0, 6);
    g_app.ap_ssid[0] = 0;
    csi_set_channel(channel);
}

/* Pop one frame and update online stats (Welford).  Call from main loop. */
int csi_pop_frame(csi_frame_t *out) {
    if (s_tail == s_head) return 0;
    *out = s_ring[s_tail];
    s_tail = (s_tail + 1) & CSI_RING_MASK;

    s_stats_n++;
    for (int i = 0; i < CSI_N_SUB; i++) {
        float x     = (float)out->amp[i];
        float delta = x - s_mean[i];
        s_mean[i]  += delta / (float)s_stats_n;
        s_M2[i]    += delta * (x - s_mean[i]);  /* Welford M2 update */
    }

    g_app.csi_fps   = s_fps;
    g_app.csi_total = s_frame_total;
    return 1;
}

void csi_get_stats(csi_stats_t *out) {
    for (int i = 0; i < CSI_N_SUB; i++) {
        out->mean[i] = s_mean[i];
        out->var[i]  = (s_stats_n > 1) ? s_M2[i] / (float)(s_stats_n - 1) : 0.0f;
    }
    out->frame_count = s_frame_total;
    out->fps         = s_fps;
}

int csi_scan_aps(void) {
    esp_wifi_set_csi(false);
    esp_wifi_set_promiscuous(false);

    wifi_scan_config_t sc = {};
    sc.show_hidden = true;
    esp_err_t err = esp_wifi_scan_start(&sc, true);   /* blocking */
    if (err != ESP_OK) {
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_csi(true);
        return 0;
    }

    uint16_t cnt = 0;
    esp_wifi_scan_get_ap_num(&cnt);
    if (cnt > CSI_MAX_APS) cnt = CSI_MAX_APS;

    wifi_ap_record_t recs[CSI_MAX_APS];
    uint16_t fetch = cnt;
    esp_wifi_scan_get_ap_records(&fetch, recs);

    s_ap_n = (int)fetch;
    for (int i = 0; i < s_ap_n; i++) {
        strncpy(s_aps[i].ssid, (char *)recs[i].ssid, 32);
        s_aps[i].ssid[32] = 0;
        memcpy(s_aps[i].bssid, recs[i].bssid, 6);
        s_aps[i].rssi = recs[i].rssi;
        s_aps[i].ch   = (int)recs[i].primary;
    }

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_csi(true);
    return s_ap_n;
}

int            csi_ap_count(void)       { return s_ap_n; }
const char*    csi_ap_ssid(int i)       { return i < s_ap_n ? s_aps[i].ssid : ""; }
int8_t         csi_ap_rssi(int i)       { return i < s_ap_n ? s_aps[i].rssi : 0; }
int            csi_ap_channel(int i)    { return i < s_ap_n ? s_aps[i].ch   : 0; }
const uint8_t* csi_ap_bssid(int i)     { return i < s_ap_n ? s_aps[i].bssid : nullptr; }
