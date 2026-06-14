/*
 * mod_los.cpp — Line-of-Sight disturbance sensing (differential CSI)
 *
 * Flow
 * ────
 *   IDLE → user triggers → COUNTDOWN (5 s) → CALIBRATING (80 frame-pairs)
 *   → SCANNING (continuous)
 *
 * Algorithm: frame-to-frame differential
 * ───────────────────────────────────────
 *   Calibration: collect |amp[k,n] - amp[k,n-1]| diffs → μ_d[k], σ_d[k]
 *   Scanning:    excess[k] = (|diff[k]| - μ_d[k]) / σ_d[k]
 *                score = EMA(Σ clamp(excess, 0, 3) / (3·N_sub) · 100)
 *
 *   At rest  : diff ≈ μ_d  → excess ≈ 0  → score ≈ 0
 *   Movement : diff >> μ_d → excess >> 0 → score spikes
 *
 * Audio (async FreeRTOS queue → Core 1 task)
 * ────────────────────────────────────────────
 *   score < 20        : silence
 *   score 20..40      : 600 Hz, 60 ms,  period 1800 ms
 *   score 40..65      : 1200 Hz, 80 ms,  period 600 ms
 *   score 65..85      : 2200 Hz, 100 ms, period 200 ms
 *   score > 85        : 3200 Hz, 120 ms, period 80 ms
 *
 *   Armed signal: 3 ascending beeps (600→900→1400 Hz) queued async.
 */

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "mod_los.h"
#include "app.h"

extern "C" {
#include "sic/audio/amp.h"
}

/* ── Tuning ──────────────────────────────────────────────────────────────── */
#define LOS_COUNTDOWN_S    5
#define LOS_EMA_ALPHA      0.25f
#define LOS_DIFF_SIGMA_EPS 0.5f
#define LOS_SCORE_MAX_STD  3.0f
#define LOS_SILENT_BELOW   20.0f

/* ── State ───────────────────────────────────────────────────────────────── */
static los_state_t s_state = LOS_IDLE;
static uint32_t    s_t0    = 0;
static int         s_cd    = LOS_COUNTDOWN_S;

static float s_prev[CSI_N_SUB];
static bool  s_prev_valid = false;

static int   s_cal_n = 0;
static float s_cal_dsum[CSI_N_SUB];
static float s_cal_dsum2[CSI_N_SUB];

static float s_diff_mean[CSI_N_SUB];
static float s_diff_sigma[CSI_N_SUB];

static float    s_score     = 0.0f;
static uint32_t s_beep_next = 0;

/* ── Async audio queue ───────────────────────────────────────────────────── */
typedef struct { float hz; int dur_ms; int gap_ms; } beep_req_t;
static QueueHandle_t s_beep_q = nullptr;

static void play_tone_blocking(float hz, int dur_ms) {
    const amp_t *amp = sic_amp(0);
    if (!amp || !amp->v) return;

    int n = 16 * dur_ms;
    if (n > 2400) n = 2400;
    static int16_t buf[2400];

    float w = 2.0f * (float)M_PI * hz / 16000.0f;
    for (int i = 0; i < n; i++)
        buf[i] = (int16_t)(sinf(w * (float)i) * 22000.0f);

    if (amp->v->play_mono)
        amp->v->play_mono(amp, buf, (size_t)n, 16000);
    else if (amp->v->beep_ms)
        amp->v->beep_ms(amp, (unsigned)dur_ms);
}

static void audio_task(void *) {
    beep_req_t b;
    for (;;) {
        if (xQueueReceive(s_beep_q, &b, portMAX_DELAY) == pdTRUE) {
            play_tone_blocking(b.hz, b.dur_ms);
            if (b.gap_ms > 0)
                vTaskDelay((TickType_t)b.gap_ms / portTICK_PERIOD_MS);
        }
    }
}

/* Non-blocking enqueue — drops silently if queue full */
static void beep_async(float hz, int dur_ms, int gap_ms = 0) {
    if (!s_beep_q) return;
    beep_req_t b = {hz, dur_ms, gap_ms};
    xQueueSend(s_beep_q, &b, 0);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void score_to_beep(float score, float *hz, int *dur_ms, int *period_ms) {
    if (score < LOS_SILENT_BELOW) { *hz = 0; *dur_ms = 0; *period_ms = 9999; return; }
    if (score < 40.0f)  { *hz = 600;  *dur_ms = 60;  *period_ms = 1800; return; }
    if (score < 65.0f)  { *hz = 1200; *dur_ms = 80;  *period_ms = 600;  return; }
    if (score < 85.0f)  { *hz = 2200; *dur_ms = 100; *period_ms = 200;  return; }
                          *hz = 3200; *dur_ms = 120; *period_ms = 80;
}

static void reset_cal(void) {
    s_cal_n = 0;
    s_prev_valid = false;
    memset(s_prev,        0, sizeof s_prev);
    memset(s_cal_dsum,    0, sizeof s_cal_dsum);
    memset(s_cal_dsum2,   0, sizeof s_cal_dsum2);
    memset(s_diff_mean,   0, sizeof s_diff_mean);
    memset(s_diff_sigma,  0, sizeof s_diff_sigma);
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void los_init(void) {
    s_state = LOS_IDLE;
    s_score = 0.0f;
    s_beep_next = 0;
    reset_cal();
    g_app.los_state       = LOS_IDLE;
    g_app.los_countdown   = 0;
    g_app.los_cal_progress = 0;
    g_app.motion_score    = 0.0f;
    g_app.los_ap_sel      = 0;

    /* Create audio queue + task once */
    if (!s_beep_q) {
        s_beep_q = xQueueCreate(6, sizeof(beep_req_t));
        xTaskCreatePinnedToCore(audio_task, "los_audio", 4096,
                                nullptr, 1, nullptr, 1 /* APP_CPU */);
    }
}

void los_start(void) {
    s_state = LOS_COUNTDOWN;
    s_t0    = millis();
    s_cd    = LOS_COUNTDOWN_S;
    s_score = 0.0f;
    s_beep_next = 0;
    reset_cal();
    g_app.los_state        = LOS_COUNTDOWN;
    g_app.los_countdown    = s_cd;
    g_app.los_cal_progress = 0;
}

void los_stop(void) {
    s_state = LOS_IDLE;
    s_score = 0.0f;
    g_app.los_state    = LOS_IDLE;
    g_app.motion_score = 0.0f;
}

void los_recalibrate(void) {
    s_state = LOS_COUNTDOWN;
    s_t0    = millis();
    s_cd    = LOS_COUNTDOWN_S;
    s_score = 0.0f;
    s_beep_next = 0;
    reset_cal();
    g_app.los_state        = LOS_COUNTDOWN;
    g_app.los_countdown    = s_cd;
    g_app.los_cal_progress = 0;
}

void los_update(const csi_frame_t *f, uint32_t now_ms) {
    if (!f) return;

    switch (s_state) {
    case LOS_IDLE:
    case LOS_SELECTING:
        break;

    case LOS_COUNTDOWN: {
        int elapsed = (int)((now_ms - s_t0) / 1000);
        s_cd = LOS_COUNTDOWN_S - elapsed;
        g_app.los_countdown = s_cd > 0 ? s_cd : 0;
        if (s_cd <= 0) {
            s_state = LOS_CALIBRATING;
            g_app.los_state = LOS_CALIBRATING;
            for (int i = 0; i < CSI_N_SUB; i++)
                s_prev[i] = (float)f->amp[i];
            s_prev_valid = true;
        }
        break;
    }

    case LOS_CALIBRATING: {
        if (s_prev_valid) {
            for (int i = 0; i < CSI_N_SUB; i++) {
                float d = fabsf((float)f->amp[i] - s_prev[i]);
                s_cal_dsum[i]  += d;
                s_cal_dsum2[i] += d * d;
            }
            s_cal_n++;
            g_app.los_cal_progress = s_cal_n * 100 / CSI_CAL_FRAMES;

            if (s_cal_n >= CSI_CAL_FRAMES) {
                float N = (float)CSI_CAL_FRAMES;
                for (int i = 0; i < CSI_N_SUB; i++) {
                    s_diff_mean[i] = s_cal_dsum[i] / N;
                    float var = s_cal_dsum2[i] / N - s_diff_mean[i] * s_diff_mean[i];
                    s_diff_sigma[i] = (var > 0.0f) ? sqrtf(var) : 0.0f;
                    if (s_diff_sigma[i] < LOS_DIFF_SIGMA_EPS)
                        s_diff_sigma[i] = LOS_DIFF_SIGMA_EPS;
                }
                g_app.los_cal_progress = 100;
                /* Armed tripwire: three ascending beeps, all async */
                beep_async(600,  80, 80);
                beep_async(900,  80, 80);
                beep_async(1400, 100,  0);
                s_state = LOS_SCANNING;
                g_app.los_state = LOS_SCANNING;
                s_beep_next = now_ms + 600;
            }
        }
        for (int i = 0; i < CSI_N_SUB; i++)
            s_prev[i] = (float)f->amp[i];
        s_prev_valid = true;
        break;
    }

    case LOS_SCANNING: {
        float raw = 0.0f;
        if (s_prev_valid) {
            for (int i = 0; i < CSI_N_SUB; i++) {
                float d      = fabsf((float)f->amp[i] - s_prev[i]);
                float excess = (d - s_diff_mean[i]) / s_diff_sigma[i];
                raw += clampf(excess, 0.0f, LOS_SCORE_MAX_STD);
            }
        }
        float score_raw = (raw / (LOS_SCORE_MAX_STD * (float)CSI_N_SUB)) * 100.0f;
        s_score = LOS_EMA_ALPHA * score_raw + (1.0f - LOS_EMA_ALPHA) * s_score;
        s_score = clampf(s_score, 0.0f, 100.0f);
        g_app.motion_score = s_score;

        for (int i = 0; i < CSI_N_SUB; i++)
            s_prev[i] = (float)f->amp[i];
        s_prev_valid = true;

        float hz; int dur_ms, period_ms;
        score_to_beep(s_score, &hz, &dur_ms, &period_ms);
        if (hz > 0.0f && (int32_t)(now_ms - s_beep_next) >= 0) {
            beep_async(hz, dur_ms);
            s_beep_next = now_ms + (uint32_t)period_ms;
        }
        break;
    }
    }
}

float       los_get_score(void)        { return s_score; }
los_state_t los_get_state(void)        { return s_state; }
int         los_get_countdown(void)    { return s_cd > 0 ? s_cd : 0; }
int         los_get_cal_progress(void) { return g_app.los_cal_progress; }

void los_get_baseline(float *mean_out, float *sigma_out) {
    for (int i = 0; i < CSI_N_SUB; i++) {
        if (mean_out)  mean_out[i]  = s_diff_mean[i];
        if (sigma_out) sigma_out[i] = s_diff_sigma[i];
    }
}
