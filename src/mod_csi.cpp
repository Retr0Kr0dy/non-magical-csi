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
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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

/* ── Active injection ────────────────────────────────────────────────────
 *
 * Probe Request frames are injected at ~100 Hz; each AP reply (Probe Response)
 * is a received 802.11 frame that triggers the CSI callback.  No association
 * or password required.
 *
 * Note: Auth Request frames (FC=0xB0) were tested but cause the ESP32 WiFi
 * driver's internal 802.11 state machine to process Auth Responses, which
 * disrupts promiscuous mode and kills all CSI RX.  Probe-only is used.
 *
 * SA MAC rotation: APs rate-limit responses per source MAC.  Rotating the
 * last 3 SA bytes per frame makes each injection appear to come from a
 * distinct locally-administered unicast client, bypassing per-SA throttling.
 * Bit 1 of SA[0] is forced high (locally administered); bit 0 stays low
 * (unicast).  Bytes [0-2] are the device's OUI; bytes [3-5] count up.
 *
 * Probe Request frame (36 bytes, FCS appended by driver):
 *   [0-1]   FC = 0x40 0x00  (Management, Probe Request)
 *   [2-3]   Duration = 0
 *   [4-9]   DA = broadcast  (only target BSSID responds)
 *   [10-15] SA = rotated per-frame
 *   [16-21] BSSID = target
 *   [22-23] Seq ctrl (driver fills via en_sys_seq=true)
 *   [24-25] SSID IE tag=0 len=0 (wildcard)
 *   [26-35] Supported Rates IE (8 rates)
 * ─────────────────────────────────────────────────────────────────────────*/
#define CSI_ACTIVE_INTERVAL_MS  10   /* ~100 Hz TX */

static TaskHandle_t      s_active_task   = nullptr;
static volatile bool     s_active_run    = false;
static uint8_t           s_probe_frame[36];
static uint8_t           s_sa_base[6];
static volatile uint32_t s_active_tx_ok  = 0;
static volatile uint32_t s_active_tx_err = 0;

/* Write rotating SA into whichever frame is about to be sent.
 * SA is at bytes [10-15] in every 802.11 management frame.                 */
static inline void inject_apply_sa(uint8_t *frame, uint32_t n) {
    frame[10] = s_sa_base[0] | 0x02;
    frame[11] = s_sa_base[1];
    frame[12] = s_sa_base[2];
    frame[13] = (uint8_t)(n      );
    frame[14] = (uint8_t)(n >>  8);
    frame[15] = (uint8_t)(n >> 16);
}

static void active_probe_task(void *) {
    uint32_t log_t   = millis();
    uint32_t log_rx0 = s_frame_total;   /* CSI frame count at last log tick */
    while (s_active_run) {
        uint32_t n = s_active_tx_ok;
        inject_apply_sa(s_probe_frame, n);

        esp_err_t r = esp_wifi_80211_tx(WIFI_IF_AP, s_probe_frame, (int)sizeof s_probe_frame, true);
        if (r == ESP_OK) s_active_tx_ok  = s_active_tx_ok  + 1;
        else             s_active_tx_err = s_active_tx_err + 1;

        uint32_t now = millis();
        if ((now - log_t) >= 5000) {
            uint32_t rx_now  = s_frame_total;
            uint32_t rx_rate = (rx_now - log_rx0) * 1000 / (now - log_t);
            log_rx0 = rx_now;
            log_t   = now;
            Serial.printf("[CSI] inject tx_ok=%lu tx_err=%lu csi_fps=%lu\n",
                          (unsigned long)s_active_tx_ok,
                          (unsigned long)s_active_tx_err,
                          (unsigned long)rx_rate);
        }
        vTaskDelay(pdMS_TO_TICKS(CSI_ACTIVE_INTERVAL_MS));
    }
    s_active_task = nullptr;
    vTaskDelete(nullptr);
}

static void active_start(const uint8_t *bssid) {
    if (s_active_task) return;

    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    memcpy(s_sa_base, mac, 6);

    /* Probe Request frame */
    static const uint8_t kRates[] = {0x82,0x84,0x8b,0x96, 0x24,0x30,0x48,0x6c};
    memset(s_probe_frame, 0, sizeof s_probe_frame);
    s_probe_frame[0] = 0x40;                       /* FC: Management, Probe Request */
    memset(s_probe_frame + 4, 0xff, 6);            /* DA: broadcast */
    /* SA [10-15]: set per-frame by inject_apply_sa() */
    memcpy(s_probe_frame + 16, bssid, 6);          /* BSSID: target */
    s_probe_frame[26] = 0x01; s_probe_frame[27] = 0x08; /* Supported Rates IE */
    memcpy(s_probe_frame + 28, kRates, 8);

    s_active_tx_ok = 0; s_active_tx_err = 0;
    s_active_run = true;
    BaseType_t ok = xTaskCreatePinnedToCore(active_probe_task, "csi_active", 2048,
                                            nullptr, 2, &s_active_task, 0);
    Serial.printf("[CSI] active_start bssid=%02x:%02x:%02x:%02x:%02x:%02x task=%s\n",
                  bssid[0],bssid[1],bssid[2],bssid[3],bssid[4],bssid[5],
                  ok == pdPASS ? "OK" : "FAIL");
}

static void active_stop(void) {
    if (!s_active_run) return;
    s_active_run = false;
    /* task sees flag within INTERVAL_MS, self-deletes, and clears s_active_task */
    uint32_t t0 = millis();
    while (s_active_task && (millis() - t0) < 100)
        vTaskDelay(pdMS_TO_TICKS(5));
    s_active_task = nullptr;   /* safety clear */
}

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
    esp_wifi_stop();
    esp_wifi_deinit();

    /* Default event loop — suppresses "failed to post WiFi event" log spam */
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    /* APSTA: STA stays in promiscuous for CSI; AP interface used for probe injection.
     * In APSTA the driver forces AP to follow the STA channel, so csi_set_channel()
     * keeps both interfaces on the target AP's channel automatically.             */
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    /* Minimal hidden soft-AP — required for WIFI_IF_AP TX; not used for connections */
    wifi_config_t ap_cfg = {};
    memcpy(ap_cfg.ap.ssid, "csi", 3);
    ap_cfg.ap.ssid_len        = 3;
    ap_cfg.ap.channel         = (uint8_t)channel;
    ap_cfg.ap.authmode        = WIFI_AUTH_OPEN;
    ap_cfg.ap.ssid_hidden     = 1;
    ap_cfg.ap.max_connection  = 0;
    ap_cfg.ap.beacon_interval = 60000;  /* max — one beacon per minute, minimal noise */
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
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
    active_stop();
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
    active_stop();
    memcpy(s_target, s_aps[idx].bssid, 6);
    s_filter = true;
    strncpy(g_app.ap_ssid, s_aps[idx].ssid, 32);
    memcpy(g_app.ap_bssid, s_aps[idx].bssid, 6);
    csi_set_channel(s_aps[idx].ch);
    /* caller decides whether to start active injection via csi_active_start() */
}

void csi_select_any(int channel) {
    active_stop();
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
bool           csi_active_running(void) { return s_active_run; }
uint32_t       csi_active_tx_ok(void)  { return s_active_tx_ok; }
uint32_t       csi_active_tx_err(void) { return s_active_tx_err; }

void csi_active_start(void) {
    if (!s_filter) { Serial.printf("[CSI] active_start skipped: no target\n"); return; }
    active_start(s_target);
}
void csi_active_stop(void)  { active_stop(); }
