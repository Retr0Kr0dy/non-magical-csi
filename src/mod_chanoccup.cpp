#include <Arduino.h>
#include <string.h>
#include "mod_chanoccup.h"
#include "mod_csi.h"
#include "app.h"

#define N_CH        13
#define SETTLE_MS   100u   /* ignore frames right after hop (hardware pipeline flush) */
#define DWELL_MS    500u   /* total time per channel; capture window = DWELL - SETTLE */

static bool     s_running  = false;
static int      s_ch_idx   = 0;        /* 0-based, maps to channel s_ch_idx+1 */
static uint32_t s_hop_t0   = 0;
static int      s_count    = 0;
static float    s_fps[N_CH];
static float    s_peak[N_CH];          /* per-channel historical peak fps */
static float    s_last_nz[N_CH];       /* last non-zero fps - kept when current is 0 */
static int      s_saved_ch = 6;

static void hop_to(int idx, uint32_t now_ms) {
    s_ch_idx = idx % N_CH;
    s_count  = 0;
    s_hop_t0 = now_ms;
    csi_set_channel(s_ch_idx + 1);
}

void chanoccup_init(void) {
    s_running = false;
    s_ch_idx  = 0;
    s_count   = 0;
    memset(s_fps, 0, sizeof s_fps);
}

void chanoccup_start(void) {
    s_saved_ch = g_app.wifi_channel;
    s_running  = true;
    memset(s_fps,     0, sizeof s_fps);
    memset(s_peak,    0, sizeof s_peak);
    memset(s_last_nz, 0, sizeof s_last_nz);
    hop_to(0, millis());
}

void chanoccup_stop(void) {
    s_running = false;
    csi_set_channel(s_saved_ch);
}

void chanoccup_tick(uint32_t now_ms) {
    if (!s_running) return;
    if ((now_ms - s_hop_t0) < DWELL_MS) return;

    /* commit fps for current channel, update peak */
    float cap_s = (float)(DWELL_MS - SETTLE_MS) * 0.001f;
    s_fps[s_ch_idx] = (float)s_count / cap_s;
    if (s_fps[s_ch_idx] > s_peak[s_ch_idx])   s_peak[s_ch_idx]  = s_fps[s_ch_idx];
    if (s_fps[s_ch_idx] > 0.0f)               s_last_nz[s_ch_idx] = s_fps[s_ch_idx];

    hop_to(s_ch_idx + 1, now_ms);
}

void chanoccup_on_frame(uint32_t now_ms) {
    if (!s_running) return;
    if ((now_ms - s_hop_t0) < SETTLE_MS) return;  /* still settling */
    s_count++;
}

int   chanoccup_current_ch(void) { return s_ch_idx + 1; }
float chanoccup_fps(int ch)       { return (ch >= 1 && ch <= N_CH) ? s_fps[ch-1]  : 0.0f; }
float chanoccup_peak(int ch)      { return (ch >= 1 && ch <= N_CH) ? s_peak[ch-1]   : 0.0f; }
float chanoccup_last_nz(int ch)   { return (ch >= 1 && ch <= N_CH) ? s_last_nz[ch-1] : 0.0f; }
bool  chanoccup_settling(void)    { return s_running && (millis() - s_hop_t0) < SETTLE_MS; }
