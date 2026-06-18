/*
 * mod_views.cpp — SGFX framebuffer UI for CSI Sense
 *
 * Display: 240×135, landscape, dark theme.
 *
 * Layout
 * ──────
 *   [0..11]    status bar  (12 px): mode + channel + SSID + FPS
 *   [12..124]  main area  (113 px): mode-specific content
 *   [125..134] foot bar   (10 px):  keyboard hints
 *
 * Color palette  (phosphor green / tactical NVG theme)
 * -----------------------------------------------------
 *   C_BG     (  0, 10,  2) near-black, green tint
 *   C_TEXT   ( 51,255, 85) phosphor green  (primary text)
 *   C_ACCENT (204,255, 68) yellow-green  (selection, headings, mode tag)
 *   C_GREEN  ( 51,255, 85) phosphor green  (static, active, success)
 *   C_AMBER  (255, 51,  0) orange-red  (caution, motion, scanning)
 *   C_RED    (220, 20,  0) alert red  (high motion, recording)
 *   C_DIM    (  8, 68, 20) dark green  (bar fills, separator lines)
 *   C_LABEL  ( 26,170, 51) medium green  (secondary text)
 *   C_DARK   (  0,  5,  1) near-pure black  (statusbar/footer bg)
 *   C_BAR_HI (160,255, 50) yellow-green  (high-signal bars)
 *   C_BAR_LO (  5, 40, 12) dark green  (low-signal bars)
 *   C_PANEL  (  4, 28,  8) very dark green  (panel/selection bg)
 */

#include <Arduino.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "mod_views.h"
#include "mod_los.h"
#include "mod_csi.h"
#include "mod_training.h"
#include "mod_chanoccup.h"
#include "mod_fileman.h"
#include "app.h"
#include "sgfx.h"
#include "sgfx_fb.h"
#include "sgfx_font_builtin.h"

/* -- Palette -- */
#define C_BG     ((sgfx_rgba8_t){  0, 10,  2,255})
#define C_TEXT   ((sgfx_rgba8_t){ 51,255, 85,255})
#define C_ACCENT ((sgfx_rgba8_t){204,255, 68,255})
#define C_GREEN  ((sgfx_rgba8_t){ 51,255, 85,255})
#define C_AMBER  ((sgfx_rgba8_t){255, 51,  0,255})
#define C_RED    ((sgfx_rgba8_t){220, 20,  0,255})
#define C_DIM    ((sgfx_rgba8_t){  8, 68, 20,255})
#define C_LABEL  ((sgfx_rgba8_t){ 26,170, 51,255})
#define C_DARK   ((sgfx_rgba8_t){  0,  5,  1,255})
#define C_BAR_HI ((sgfx_rgba8_t){160,255, 50,255})
#define C_BAR_LO ((sgfx_rgba8_t){  5, 40, 12,255})
#define C_PANEL  ((sgfx_rgba8_t){  4, 28,  8,255})

static sgfx_device_t  *s_dev = nullptr;
static sgfx_fb_t       s_fb;
static sgfx_present_t  s_pr;
static bool            s_fb_ok = false;

/* CSI display state — file scope so ui_csi_reset() can clear them */
static uint8_t  s_wf[CSI_HIST_LEN][CSI_N_SUB];   /* waterfall ring buffer       */
static float    s_motion_hist[CSI_HIST_LEN];
static int      s_wf_head = 0;                     /* next write slot in s_wf     */
static uint32_t s_wf_n    = 0;                     /* valid rows, capped at HIST  */

/* FNV-1a over a byte array — tiny, collision-free for small changes */
static inline uint32_t fnv32(const uint8_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* ── Framebuffer primitives ──────────────────────────────────────────────── */
static inline uint16_t pack565(sgfx_rgba8_t c) {
    return (uint16_t)(((c.r & 0xF8u) << 8) | ((c.g & 0xFCu) << 3) | (c.b >> 3));
}

/* Write one pixel directly to the framebuffer (no dirty tracking). */
static inline void fb_raw_px(int x, int y, uint16_t v565) {
    ((uint16_t *)((uint8_t *)s_fb.px + (size_t)y * (size_t)s_fb.stride))[x] = v565;
}

/* Write a horizontal span directly (no dirty tracking). */
static inline void fb_raw_hspan(int x, int y, int w, uint16_t v565) {
    uint16_t *p = (uint16_t *)((uint8_t *)s_fb.px + (size_t)y * (size_t)s_fb.stride) + x;
    for (int i = 0; i < w; i++) p[i] = v565;
}

static void fb_fill(int x, int y, int w, int h, sgfx_rgba8_t c) {
    if (!s_fb.px || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SGFX_W) w = SGFX_W - x;
    if (y + h > SGFX_H) h = SGFX_H - y;
    if (w <= 0 || h <= 0) return;
    uint16_t v = pack565(c);
    for (int j = 0; j < h; j++) {
        uint16_t *row = (uint16_t *)((uint8_t *)s_fb.px
                        + (size_t)(y + j) * (size_t)s_fb.stride) + x;
        for (int i = 0; i < w; i++) row[i] = v;
    }
    sgfx_fb_mark_dirty_px(&s_fb, x, y, w, h);
}

static void fb_text5x7(int x0, int y0, const char *s, sgfx_rgba8_t c, int scale) {
    if (!s_fb.px || !s || scale < 1) return;
    int adv = 6 * scale;
    for (; *s; ++s, x0 += adv) {
        uint8_t cols[5];
        if (!sgfx_font5x7_get(*s, cols)) continue;
        for (int col = 0; col < 5; col++) {
            int px = x0 + col * scale;
            for (int row = 0; row < 7; row++) {
                if (cols[col] & (uint8_t)(1u << row))
                    fb_fill(px, y0 + row * scale, scale, scale, c);
            }
        }
    }
}

/* Clipped text — will not draw past x_max */
static void fb_text_clip(int x0, int y0, const char *s, sgfx_rgba8_t c,
                          int scale, int x_max) {
    if (!s_fb.px || !s || scale < 1) return;
    int adv = 6 * scale;
    for (; *s; ++s, x0 += adv) {
        if (x0 + 5 * scale > x_max) return;
        uint8_t cols[5];
        if (!sgfx_font5x7_get(*s, cols)) continue;
        for (int col = 0; col < 5; col++) {
            int px = x0 + col * scale;
            if (px >= x_max) break;
            for (int row = 0; row < 7; row++) {
                if (cols[col] & (uint8_t)(1u << row))
                    fb_fill(px, y0 + row * scale, scale, scale, c);
            }
        }
    }
}

/* ── Aliases ─────────────────────────────────────────────────────────────── */
static inline void fill(int x, int y, int w, int h, sgfx_rgba8_t c) { fb_fill(x,y,w,h,c); }
static inline void text1(int x, int y, const char *s, sgfx_rgba8_t c) { fb_text5x7(x,y,s,c,1); }
static inline void text2(int x, int y, const char *s, sgfx_rgba8_t c) { fb_text5x7(x,y,s,c,2); }
static inline void text3(int x, int y, const char *s, sgfx_rgba8_t c) { fb_text5x7(x,y,s,c,3); }
static inline void hline(int x, int y, int w, sgfx_rgba8_t c) { fb_fill(x,y,w,1,c); }
static inline void vline(int x, int y, int h, sgfx_rgba8_t c) { fb_fill(x,y,1,h,c); }
static inline void rect(int x, int y, int w, int h, sgfx_rgba8_t c) {
    fb_fill(x,     y,     w, 1, c);
    fb_fill(x,     y+h-1, w, 1, c);
    fb_fill(x,     y+1,   1, h-2, c);
    fb_fill(x+w-1, y+1,   1, h-2, c);
}

/* amplitude (0..255) -> waterfall heat color
 * near-black (low) -> medium green (mid) -> yellow-green (high) */
static sgfx_rgba8_t heat(uint8_t v) {
    if (v < 128) {
        uint8_t t = v * 2;
        /* (0,10,2) -> (26,170,51) */
        return (sgfx_rgba8_t){
            (uint8_t)(  0 + (uint8_t)( 26 * t / 255)),
            (uint8_t)( 10 + (uint8_t)(160 * t / 255)),
            (uint8_t)(  2 + (uint8_t)( 49 * t / 255)),
            255
        };
    } else {
        uint8_t t = (v - 128) * 2;
        /* (26,170,51) -> (204,255,68) */
        return (sgfx_rgba8_t){
            (uint8_t)( 26 + (uint8_t)(178 * t / 255)),
            (uint8_t)(170 + (uint8_t)( 85 * t / 255)),
            (uint8_t)( 51 + (uint8_t)( 17 * t / 255)),
            255
        };
    }
}

/* score (0..100) → status color (threshold matches LOS_SILENT_BELOW = 20) */
static sgfx_rgba8_t score_color(float s) {
    if (s < 20.0f) return C_GREEN;
    if (s < 45.0f) return C_AMBER;
    if (s < 75.0f) return (sgfx_rgba8_t){255, 51, 0,255};
    return C_RED;
}

/* ── Init / present ─────────────────────────────────────────────────────── */
void ui_init(sgfx_device_t *dev) {
    s_dev = dev;
    if (sgfx_fb_create(&s_fb, SGFX_W, SGFX_H, 16, 16) == SGFX_OK && s_fb.px) {
        sgfx_present_init(&s_pr, SGFX_W);
        s_fb_ok = true;
        fb_fill(0, 0, SGFX_W, SGFX_H, C_BG);
    }
}

void ui_present(void) {
    if (s_fb_ok)
        sgfx_present_frame(&s_pr, s_dev, &s_fb);
}

/* ── Status bar ─────────────────────────────────────────────────────────── */
void ui_statusbar(const char *mode_tag) {
    fill(0, 0, SGFX_W, UI_BAR_H, C_DARK);
    hline(0, UI_BAR_H - 1, SGFX_W, C_DIM);

    /* Mode tag — left, cyan, max 8 chars (48 px) */
    char tag[9]; snprintf(tag, sizeof tag, "%.8s", mode_tag);
    text1(2, 2, tag, C_ACCENT);

    /* Right cluster: channel + fps (fps in green when active injection is on) */
    int rw = 0;
    if (g_app.csi_running) {
        char ch_part[10], fps_part[10];
        snprintf(ch_part,  sizeof ch_part,  "ch%d ", g_app.wifi_channel);
        snprintf(fps_part, sizeof fps_part, "%ufps", (unsigned)g_app.csi_fps);
        rw = (int)(strlen(ch_part) + strlen(fps_part)) * 6;
        int rx = SGFX_W - rw - 2;
        text1(rx, 2, ch_part, C_LABEL);
        text1(rx + (int)strlen(ch_part) * 6, 2, fps_part,
              csi_active_running() ? C_GREEN : C_LABEL);
    } else {
        rw = 18;
        text1(SGFX_W - rw - 2, 2, "off", C_LABEL);
    }

    /* SSID — centre, clipped so it doesn't collide with right cluster */
    if (g_app.ap_ssid[0]) {
        int x_ssid = 56;
        int x_limit = SGFX_W - rw - 8;
        fb_text_clip(x_ssid, 2, g_app.ap_ssid, C_LABEL, 1, x_limit);
    }
}

/* ── Foot bar ────────────────────────────────────────────────────────────── */
void ui_footbar(const char *hints) {
    fill(0, UI_MAIN_YEND, SGFX_W, UI_FOOT_H, C_DARK);
    hline(0, UI_MAIN_YEND, SGFX_W, C_DIM);
    fb_text_clip(2, UI_MAIN_YEND + 2, hints, C_LABEL, 1, SGFX_W - 2);
}

/* ── MENU ────────────────────────────────────────────────────────────────── */
static const struct { const char *tag; const char *desc; } kModes[APP_MODE__COUNT] = {
    { "MENU",     "" },
    { "LOS",      "Line-of-sight disturbance" },
    { "SPECTRUM", "Subcarrier amplitude waterfall" },
    { "VARIANCE", "Per-subcarrier variance bars" },
    { "MOTION",   "Scalar motion score + history" },
    { "CORR",     "Cross-subcarrier correlation" },
    { "CHANOCC",  "Passive per-channel frame-rate survey" },
    { "CONSOLE",  "Serial console on screen" },
    { "TRAINING", "Guided ML data-collection session" },
    { "FILES",    "SD card file browser" },
};

void ui_draw_menu(int sel) {
    fill(0, 0, SGFX_W, SGFX_H, C_BG);

    /* Header */
    fill(0, 0, SGFX_W, 28, C_DARK);
    text2(6, 6, "NON MAGICAL CSI", C_ACCENT);
    hline(0, 27, SGFX_W, C_DIM);

    /* Scrolling mode list */
    const int ITEM_H  = 13;
    const int LIST_Y  = 31;
    const int n_modes = APP_MODE__COUNT - 1;          /* modes 1..n */
    const int visible = (UI_MAIN_YEND - LIST_Y) / ITEM_H;

    /* Keep selection centred in the window, clamped to valid range */
    int view_start = sel - visible / 2;
    if (view_start > n_modes - visible + 1) view_start = n_modes - visible + 1;
    if (view_start < 1) view_start = 1;

    /* Scroll indicators */
    if (view_start > 1)
        text1(SGFX_W - 8, LIST_Y, "^", C_DIM);
    if (view_start + visible - 1 < n_modes)
        text1(SGFX_W - 8, LIST_Y + visible * ITEM_H - 9, "v", C_DIM);

    int y = LIST_Y;
    for (int i = view_start; i < view_start + visible && i < APP_MODE__COUNT; i++) {
        bool active = (i == sel);
        if (active) {
            fill(0, y - 1, SGFX_W, ITEM_H, C_PANEL);
            vline(0, y - 1, ITEM_H, C_ACCENT);
        }
        char num[4]; snprintf(num, sizeof num, "%d", i);
        sgfx_rgba8_t nc = active ? C_LABEL : (sgfx_rgba8_t){ 8,55,18,255};
        sgfx_rgba8_t tc = active ? C_ACCENT : C_TEXT;
        text1(4,  y, num,           nc);
        text1(14, y, kModes[i].tag, tc);
        y += ITEM_H;
    }

    /* Description of selected mode in footbar */
    if (sel > 0 && sel < APP_MODE__COUNT && kModes[sel].desc[0])
        ui_footbar(kModes[sel].desc);
    else
        ui_footbar("W/S:select  ENT:open  1-9:jump");
}

/* ── LOS mode ────────────────────────────────────────────────────────────── */
void ui_draw_los(void) {
    fill(0, UI_BAR_H, SGFX_W, UI_MAIN_H, C_BG);
    ui_statusbar("LOS");

    los_state_t st = g_app.los_state;

    /* ── IDLE ── */
    if (st == LOS_IDLE) {
        text2(6, UI_BAR_H + 6, "READY", C_TEXT);
        hline(6, UI_BAR_H + 28, SGFX_W - 12, C_DIM);
        text1(6, UI_BAR_H + 32, "ENT  start LOS scan", C_LABEL);
        text1(6, UI_BAR_H + 43, "F    find / scan APs", C_LABEL);
        text1(6, UI_BAR_H + 54, "C    cycle channel (1-13)", C_LABEL);
        text1(6, UI_BAR_H + 65, "R    recalibrate", C_LABEL);
        /* Mode selector */
        text1(6, UI_BAR_H + 77, "P    mode:", C_LABEL);
        bool am = g_app.active_mode;
        text1(66, UI_BAR_H + 77, "ACTIVE",
              am  ? C_GREEN : (sgfx_rgba8_t){ 8,42,14,255});
        text1(66 + 42, UI_BAR_H + 77, "/", C_DIM);
        text1(66 + 50, UI_BAR_H + 77, "PASSIVE",
              !am ? C_TEXT  : (sgfx_rgba8_t){ 8,42,14,255});
        ui_footbar("ENT:start  F:scan  P:mode  ESC:menu");
        return;
    }

    /* ── SELECTING — show scanned AP list ── */
    if (st == LOS_SELECTING) {
        int n = csi_ap_count();
        if (g_app.los_is_scanning || n == 0) {
            text2(6, UI_BAR_H + 6, "SCANNING", C_AMBER);
            text1(6, UI_BAR_H + 30, "Looking for APs...", C_LABEL);
        } else {
            text1(6, UI_BAR_H + 2, "SELECT AP", C_ACCENT);
            int sel = g_app.los_ap_sel;
            int max_show = (UI_MAIN_H - 14) / 13;
            int start = sel - max_show / 2;
            if (start < 0) start = 0;
            if (start + max_show > n) start = n - max_show;
            if (start < 0) start = 0;
            int y = UI_BAR_H + 13;
            for (int i = start; i < n && i < start + max_show; i++) {
                bool active = (i == sel);
                if (active) {
                    fill(0, y - 1, SGFX_W, 13, C_PANEL);
                    vline(0, y - 1, 13, C_ACCENT);
                }
                sgfx_rgba8_t tc = active ? C_ACCENT : C_TEXT;
                char buf[32];
                snprintf(buf, sizeof buf, "ch%-2d %4ddBm",
                         csi_ap_channel(i), (int)csi_ap_rssi(i));
                text1(4,  y, buf, active ? C_LABEL : (sgfx_rgba8_t){10,55,20,255});
                fb_text_clip(76, y, csi_ap_ssid(i), tc, 1, SGFX_W - 4);
                y += 13;
            }
        }
        ui_footbar("W/S:pick  ENT:lock  F:rescan  ESC:back");
        return;
    }

    /* ── COUNTDOWN ── */
    if (st == LOS_COUNTDOWN) {
        int cd = g_app.los_countdown;
        fill(0, UI_BAR_H, SGFX_W, UI_MAIN_H, C_BG);
        text2(6, UI_BAR_H + 4, "STEP AWAY", cd <= 2 ? C_RED : C_AMBER);
        text1(6, UI_BAR_H + 26, "Keep environment still.", C_LABEL);

        /* Large countdown digit */
        char num[4]; snprintf(num, sizeof num, "%d", cd);
        text3(SGFX_W / 2 - 9, UI_BAR_H + 38, num, cd <= 2 ? C_RED : C_ACCENT);

        /* Dot indicators */
        for (int i = 0; i < 5; i++) {
            int dx = 6 + i * 22;
            sgfx_rgba8_t dc = i < cd ? C_ACCENT : C_DIM;
            fill(dx, UI_BAR_H + 80, 16, 16, dc);
            /* Rounded feel: dim corners */
            fill(dx,      UI_BAR_H + 80,      1, 1, C_BG);
            fill(dx+15,   UI_BAR_H + 80,      1, 1, C_BG);
            fill(dx,      UI_BAR_H + 80 + 15, 1, 1, C_BG);
            fill(dx+15,   UI_BAR_H + 80 + 15, 1, 1, C_BG);
        }
        ui_footbar("ESC:cancel");
        return;
    }

    /* ── CALIBRATING ── */
    if (st == LOS_CALIBRATING) {
        int prog = g_app.los_cal_progress;
        text2(6, UI_BAR_H + 4, "CALIBRATING", C_ACCENT);
        text1(6, UI_BAR_H + 28, "Stand still...", C_LABEL);

        /* Progress bar */
        int bx = 6, bw = SGFX_W - 12, bh = 12;
        int by = UI_BAR_H + 50;
        fill(bx, by, bw, bh, C_DIM);
        int filled = bw * prog / 100;
        if (filled > 0) fill(bx, by, filled, bh, C_ACCENT);
        rect(bx, by, bw, bh, (sgfx_rgba8_t){10,80,25,255});

        char pct[8]; snprintf(pct, sizeof pct, "%d%%", prog);
        text1(bx, by + bh + 4, pct, C_TEXT);
        text1(bx + 28, by + bh + 4,
              csi_active_running() ? "active injection" : "passive",
              csi_active_running() ? C_GREEN : C_LABEL);
        ui_footbar("P:mode  ESC:cancel");
        return;
    }

    /* ── SCANNING ── */
    float score = g_app.motion_score;
    sgfx_rgba8_t sc = score_color(score);

    /* Large score + label */
    char snum[8]; snprintf(snum, sizeof snum, "%.0f", score);
    text3(6, UI_BAR_H + 4, snum, sc);
    text1(6, UI_BAR_H + 32, "/ 100  DISTURBANCE", C_LABEL);

    /* Score bar */
    int bx = 6, bw = SGFX_W - 12;
    int by = UI_BAR_H + 44;
    fill(bx, by, bw, 10, C_DIM);
    int filled = (int)(bw * score / 100.0f);
    if (filled > 0) fill(bx, by, filled, 10, sc);
    rect(bx, by, bw, 10, (sgfx_rgba8_t){10,80,25,255});

    /* Status text */
    int ty = by + 14;
    if (score < 20.0f)       text1(bx, ty, "static", C_GREEN);
    else if (score < 40.0f)  text1(bx, ty, "slight movement", C_AMBER);
    else if (score < 65.0f)  text1(bx, ty, "MOTION DETECTED", C_AMBER);
    else if (score < 85.0f)  text1(bx, ty, "STRONG MOTION", C_RED);
    else                     text1(bx, ty, "HIGH ACTIVITY", C_RED);

    /* Calibration sigma mini-chart (bottom strip) */
    {
        float bl[CSI_N_SUB], sg[CSI_N_SUB];
        los_get_baseline(bl, sg);
        int sy2 = UI_MAIN_YEND - 20;
        fill(0, sy2, SGFX_W, 20, C_DARK);
        hline(0, sy2, SGFX_W, C_DIM);
        for (int i = 0; i < CSI_N_SUB; i++) {
            int x = 2 + i * (SGFX_W - 4) / CSI_N_SUB;
            int sh = (int)(sg[i] * 14 / 10); if (sh > 14) sh = 14;
            fill(x, sy2 + 15 - sh, 3, sh, C_DIM);
        }
        text1(2, sy2 + 2, "baseline", C_LABEL);
        text1(SGFX_W - 46, sy2 + 2,
              csi_active_running() ? "ACTIVE" : "PASSIVE",
              csi_active_running() ? C_GREEN  : C_LABEL);
    }

    ui_footbar("R:recal  P:mode  ESC:stop");
}

/* amplitude (0..255) -> stretched 0..255, clamped */
static inline uint8_t amp_stretch(uint8_t v, uint8_t lo, uint8_t hi) {
    if (hi <= lo) return 0;
    int s = ((int)(v - lo) * 255) / (hi - lo);
    return (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
}

/* ── SPECTRUM waterfall ──────────────────────────────────────────────────── */
void ui_draw_spectrum(const uint8_t amp[CSI_N_SUB],
                      const uint8_t waterfall[CSI_HIST_LEN][CSI_N_SUB],
                      int wf_head, uint8_t amp_lo, uint8_t amp_hi) {
    ui_statusbar("SPECTRUM");
    fill(0, UI_BAR_H, SGFX_W, UI_MAIN_H, C_BG);

    int chart_y = UI_BAR_H;
    int chart_h = 28;
    int wf_y    = chart_y + chart_h + 1;
    int wf_h    = UI_MAIN_YEND - wf_y;

    fill(0, chart_y, SGFX_W, chart_h, C_DARK);

    int bar_w   = SGFX_W / CSI_N_SUB;
    int bar_off = (SGFX_W - bar_w * CSI_N_SUB) / 2;

    for (int i = 0; i < CSI_N_SUB; i++) {
        int bh = (int)amp_stretch(amp[i], amp_lo, amp_hi) * chart_h / 255;
        int x  = bar_off + i * bar_w;
        fill(x, chart_y, bar_w - 1, chart_h, C_DIM);
        if (bh > 0)
            fill(x, chart_y + chart_h - bh, bar_w - 1, bh, C_BAR_HI);
    }

    hline(0, wf_y - 1, SGFX_W, C_DIM);

    /* DC null marker — subs 23-32 are always zero (LLTF OFDM standard) */
    {
        int dc_x = bar_off + 23 * bar_w;
        int dc_w = 10 * bar_w;
        fill(dc_x, chart_y, dc_w, chart_h, (sgfx_rgba8_t){ 2,14, 5,255});
        int lx = dc_x + dc_w / 2 - 6;
        text1(lx, chart_y + (chart_h - 7) / 2, "DC", (sgfx_rgba8_t){16,80,28,255});
    }

    /* Direct row writes — avoid 56*rows fill() + dirty-tile calls per frame */
    int rows_to_draw = wf_h < CSI_HIST_LEN ? wf_h : CSI_HIST_LEN;
    for (int row = 0; row < rows_to_draw; row++) {
        int ri = ((wf_head - 1 - row) + CSI_HIST_LEN) % CSI_HIST_LEN;
        int dy = wf_y + row;
        for (int i = 0; i < CSI_N_SUB; i++) {
            uint16_t pix = pack565(heat(amp_stretch(waterfall[ri][i], amp_lo, amp_hi)));
            int x = bar_off + i * bar_w;
            fb_raw_hspan(x, dy, bar_w - 1, pix);
        }
    }
    if (rows_to_draw > 0)
        sgfx_fb_mark_dirty_px(&s_fb, bar_off, wf_y, bar_w * CSI_N_SUB, rows_to_draw);

    ui_footbar(g_app.active_mode ? "C/UP/DN:ch  R:reset  P:passive  ESC:menu"
                                 : "C/UP/DN:ch  R:reset  P:active   ESC:menu");
}

/* ── VARIANCE bars ───────────────────────────────────────────────────────── */
void ui_draw_variance(const float var[CSI_N_SUB], const float mean_[CSI_N_SUB]) {
    ui_statusbar("VARIANCE");
    fill(0, UI_BAR_H, SGFX_W, UI_MAIN_H, C_BG);

    float maxv = 0.0f;
    for (int i = 0; i < CSI_N_SUB; i++)
        if (var[i] > maxv) maxv = var[i];

    float max_mean = 1.0f;
    for (int i = 0; i < CSI_N_SUB; i++)
        if (mean_[i] > max_mean) max_mean = mean_[i];

    /* Header — show max variance so user can see data is arriving */
    text1(4, UI_BAR_H + 2, "var", C_LABEL);
    {
        char mvb[24];
        if (maxv > 0.f) snprintf(mvb, sizeof mvb, "max:%.1f", (double)maxv);
        else            snprintf(mvb, sizeof mvb, "stable");
        text1(SGFX_W - 2 - (int)strlen(mvb) * 6, UI_BAR_H + 2, mvb, C_LABEL);
    }

    int ay  = UI_BAR_H + 13;
    int ah  = UI_MAIN_YEND - ay - 16;
    int bar_w   = SGFX_W / CSI_N_SUB;
    int bar_off = (SGFX_W - bar_w * CSI_N_SUB) / 2;

    for (int i = 0; i < CSI_N_SUB; i++) {
        int x = bar_off + i * bar_w;
        fill(x, ay, bar_w - 1, ah, C_DIM);

        /* Mean amplitude profile — always drawn as dim reference */
        int mh = (int)((float)ah * mean_[i] / max_mean);
        if (mh > 0)
            fill(x, ay + ah - mh, bar_w - 1, mh, C_BAR_LO);

        /* Variance overlay — bright, drawn over mean when non-zero */
        if (maxv > 0.f) {
            int bh = (int)((float)ah * var[i] / maxv);
            if (bh > 0) {
                float t = var[i] / maxv;
                sgfx_rgba8_t bc = {
                    (uint8_t)( 26 + (uint8_t)(178.f * t)),
                    (uint8_t)(170 + (uint8_t)( 85.f * t)),
                    (uint8_t)( 51 + (uint8_t)( 17.f * t)),
                    255
                };
                fill(x, ay + ah - bh, bar_w - 1, bh, bc);
            }
        }
    }

    hline(bar_off, ay + ah, bar_w * CSI_N_SUB, C_DIM);
    text1(bar_off,                          ay + ah + 3, "-28", C_LABEL);
    text1(SGFX_W - bar_off - 18,           ay + ah + 3, "+28", C_LABEL);

    /* DC null band — subs 23-32 are zero by OFDM spec, shade + label */
    {
        int dc_x = bar_off + 23 * bar_w;
        int dc_w = 10 * bar_w;
        fill(dc_x, ay, dc_w, ah, (sgfx_rgba8_t){ 2,12, 4,255});
        int lx = dc_x + dc_w / 2 - 6;
        text1(lx, ay + ah / 2 - 4, "DC", (sgfx_rgba8_t){16,80,28,255});
        text1(lx - 6, ay + ah / 2 + 4, "NULL", (sgfx_rgba8_t){12,68,22,255});
    }

    ui_footbar(g_app.active_mode ? "C:ch  R:reset  P:passive  ESC:menu"
                                 : "C:ch  R:reset  P:active   ESC:menu");
}

/* ── MOTION score + sparkline ────────────────────────────────────────────── */
void ui_draw_motion(float score, const float hist[CSI_HIST_LEN]) {
    ui_statusbar("MOTION");
    fill(0, UI_BAR_H, SGFX_W, UI_MAIN_H, C_BG);

    sgfx_rgba8_t sc = score_color(score);

    /* Score panel (left) */
    fill(0, UI_BAR_H, 80, UI_MAIN_H, C_DARK);
    vline(80, UI_BAR_H, UI_MAIN_H, C_DIM);

    char snum[8]; snprintf(snum, sizeof snum, "%.0f", score);
    text3(8, UI_BAR_H + 8, snum, sc);
    text1(8, UI_BAR_H + 36, "/100", C_LABEL);
    text1(8, UI_BAR_H + 48, "MOTION", C_LABEL);
    text1(8, UI_BAR_H + 58, "SCORE",  C_LABEL);

    /* Sparkline panel (right) */
    int sx = 84, sy = UI_BAR_H + 4;
    int sw = SGFX_W - sx - 4;
    int sh = 50;

    fill(sx, sy, sw, sh, C_PANEL);
    rect(sx, sy, sw, sh, C_DIM);

    float hmax = 1.0f;
    for (int i = 0; i < CSI_HIST_LEN; i++)
        if (hist[i] > hmax) hmax = hist[i];

    int step = sw / CSI_HIST_LEN;
    if (step < 1) step = 1;
    int prev_py = -1;
    for (int i = 0; i < CSI_HIST_LEN && (i * step) < sw; i++) {
        float v  = hist[CSI_HIST_LEN - 1 - i];
        int   py = sy + sh - 2 - (int)((float)(sh - 4) * v / hmax);
        if (py < sy + 1)   py = sy + 1;
        if (py > sy + sh - 2) py = sy + sh - 2;
        int   px = sx + sw - 2 - i * step;
        /* Vertical connector between consecutive points */
        if (prev_py >= 0) {
            int top = py < prev_py ? py : prev_py;
            int bot = py > prev_py ? py : prev_py;
            fill(px, top, 1, bot - top + 1, C_ACCENT);
        } else {
            fill(px, py, 1, 1, C_ACCENT);
        }
        prev_py = py;
    }

    /* Threshold line at 20/100 */
    int thr_y = sy + sh - 2 - (int)((float)(sh - 4) * 20.0f / hmax);
    if (thr_y >= sy && thr_y < sy + sh)
        hline(sx + 1, thr_y, sw - 2, (sgfx_rgba8_t){ 8,68,20,255});

    /* Stats below sparkline */
    float sum = 0, mx = 0;
    for (int i = 0; i < CSI_HIST_LEN; i++) {
        sum += hist[i];
        if (hist[i] > mx) mx = hist[i];
    }
    int ty2 = sy + sh + 6;
    char buf[32];
    snprintf(buf, sizeof buf, "avg %.1f  peak %.1f", (double)(sum/CSI_HIST_LEN), (double)mx);
    fb_text_clip(sx, ty2, buf, C_LABEL, 1, SGFX_W - 2);

    snprintf(buf, sizeof buf, "frames: %lu", (unsigned long)g_app.csi_total);
    fb_text_clip(sx, ty2 + 10, buf, C_DIM, 1, SGFX_W - 2);

    ui_footbar("R:reset  L:LOS  ESC:menu");
}

/* ── CORRELATION viewer ──────────────────────────────────────────────────── */
void ui_draw_corr(const uint8_t amp[CSI_N_SUB]) {
    ui_statusbar("CORR");
    fill(0, UI_BAR_H, SGFX_W, UI_MAIN_H, C_BG);

    /* 56×56 amplitude product matrix, centred horizontally */
    int sz = CSI_N_SUB;   /* 56 */
    int ox = (SGFX_W - sz) / 2;
    int oy = UI_BAR_H + 4;

    /* Direct pixel writes — avoid 3136 individual fill() + dirty-tile calls */
    for (int i = 0; i < sz; i++) {
        for (int j = 0; j < sz; j++) {
            uint32_t v = (uint32_t)amp[i] * amp[j] / 255;
            fb_raw_px(ox + j, oy + i, pack565(heat((uint8_t)(v > 255 ? 255 : v))));
        }
    }
    sgfx_fb_mark_dirty_px(&s_fb, ox, oy, sz, sz);
    rect(ox - 1, oy - 1, sz + 2, sz + 2, C_DIM);

    /* Labels below matrix — x=4 so they never overflow */
    int ly = oy + sz + 3;
    text1(4, ly,      "amp product (i,j) matrix", C_LABEL);
    text1(4, ly + 10, "bright = coherent subs",   C_LABEL);

    ui_footbar("ESC:menu");
}

/* ── On-screen console ───────────────────────────────────────────────────── */
void ui_draw_console(const char *scr, int stride, int rows, int cols,
                     int cur_row, int cur_col) {
    (void)cur_col; (void)cols;
    ui_statusbar("CONSOLE");
    fill(0, UI_BAR_H, SGFX_W, UI_MAIN_H, C_BG);

    for (int r = 0; r < rows; r++) {
        int y = UI_BAR_H + r * 8;
        if (y + 8 > UI_MAIN_YEND) break;
        const char *row = scr + r * stride;
        if (row[0])
            fb_text_clip(0, y, row,
                         r == cur_row ? C_TEXT : C_LABEL, 1, SGFX_W);
    }
    ui_footbar("ESC:menu");
}

/* ── Training mode ───────────────────────────────────────────────────────── */
static void draw_progress_bar(int x, int y, int w, int h,
                              int elapsed_s, int total_s, sgfx_rgba8_t col) {
    fill(x, y, w, h, C_DIM);
    if (total_s > 0) {
        int filled = (elapsed_s * w) / total_s;
        if (filled > w) filled = w;
        if (filled > 0) fill(x, y, filled, h, col);
    }
    rect(x, y, w, h, C_LABEL);
}

void ui_draw_training(void) {
    fill(0, 0, SGFX_W, SGFX_H, C_BG);
    ui_statusbar("TRAIN");

    train_ui_t ui = training_ui();

    /* ── Procedure selection ─────────────────────────────────────────────── */
    if (ui == TRAIN_UI_PROC_SEL) {
        fill(0, UI_BAR_H, SGFX_W, 14, C_PANEL);
        text1(4, UI_BAR_H + 3, "SELECT PROCEDURE", C_ACCENT);
        hline(0, UI_BAR_H + 13, SGFX_W, C_DIM);

        int y = UI_BAR_H + 17;
        for (int i = 0; i < TRAIN_PROC__COUNT; i++) {
            bool sel = (i == training_proc_cursor());
            if (sel) {
                fill(0, y - 1, SGFX_W, 13, C_PANEL);
                vline(0, y - 1, 13, C_ACCENT);
            }
            char buf[40];
            snprintf(buf, sizeof buf, "%d  %s", i + 1, training_proc_name((train_proc_t)i));
            text1(6, y, buf, sel ? C_TEXT : C_LABEL);
            y += 14;
        }
        ui_footbar(g_app.active_mode ? "W/S:sel  ENT:go  ?:help  P:passive  ESC:menu"
                                     : "W/S:sel  ENT:go  ?:help  P:active   ESC:menu");
        return;
    }

    /* ── Help ────────────────────────────────────────────────────────────── */
    if (ui == TRAIN_UI_HELP) {
        fill(0, UI_BAR_H, SGFX_W, 14, C_PANEL);
        char title[32];
        snprintf(title, sizeof title, "HELP: %s", training_proc_name(training_proc()));
        text1(4, UI_BAR_H + 3, title, C_ACCENT);
        hline(0, UI_BAR_H + 13, SGFX_W, C_DIM);

        const char *help = training_proc_help(training_proc());
        int y = UI_BAR_H + 18;
        const char *p = help;
        char line[42];
        while (*p && y < UI_MAIN_YEND - 10) {
            int i = 0;
            while (*p && *p != '\n' && i < 40) line[i++] = *p++;
            line[i] = '\0';
            if (*p == '\n') p++;
            text1(4, y, line, C_LABEL);
            y += 9;
        }
        ui_footbar("ESC/ENT:back");
        return;
    }

    /* ── Name entry ──────────────────────────────────────────────────────── */
    if (ui == TRAIN_UI_NAME) {
        fill(0, UI_BAR_H, SGFX_W, 14, C_PANEL);
        char title[40];
        snprintf(title, sizeof title, "SESSION NAME  [%s]",
                 training_proc_name(training_proc()));
        text1(4, UI_BAR_H + 3, title, C_ACCENT);
        hline(0, UI_BAR_H + 13, SGFX_W, C_DIM);

        text1(4, UI_BAR_H + 22, "Name:", C_LABEL);

        {
            const char *name = training_session_name();
            int name_len = (int)strlen(name);
            char cur[34];
            snprintf(cur, sizeof cur, "%s_", name);
            /* text2: each char is 12px wide (6px adv × scale 2).
             * Scroll so cursor is always visible; show '<' when clipped. */
            const int CHAR_W2  = 12;
            const int TEXT_X   = 4;
            const int avail_px = SGFX_W - TEXT_X - 4;
            int max_vis = avail_px / CHAR_W2;
            int total   = name_len + 1;   /* +1 for cursor '_' */
            int offset  = (total > max_vis) ? total - max_vis : 0;
            if (offset > 0) text1(TEXT_X, UI_BAR_H + 34 + 4, "<", C_DIM);
            fb_text_clip(TEXT_X, UI_BAR_H + 34, cur + offset, C_TEXT, 2, SGFX_W - 4);
        }

        text1(4, UI_BAR_H + 62, "alphanum and _ only", C_LABEL);
        text1(4, UI_BAR_H + 73, "ENT: start  ESC+empty: back", C_LABEL);
        ui_footbar("type name then ENT to start");
        return;
    }

    /* ── Running ─────────────────────────────────────────────────────────── */
    if (ui == TRAIN_UI_RUNNING) {
        int total_steps  = training_step_total();
        int cur_step     = training_step();
        bool in_cap      = training_in_cap();
        int remain       = training_remain_s();
        int phase_dur    = training_phase_dur_s();
        const char *label = training_step_label();
        const char *ins1  = training_step_instr1();
        const char *ins2  = training_step_instr2();
        const char *next1 = training_next_instr1();

        /* Header: proc name + step counter */
        fill(0, UI_BAR_H, SGFX_W, 14, C_PANEL);
        char hdr[40];
        snprintf(hdr, sizeof hdr, "%s  step %d/%d",
                 training_proc_name(training_proc()), cur_step + 1, total_steps);
        text1(4, UI_BAR_H + 3, hdr, C_LABEL);
        hline(0, UI_BAR_H + 13, SGFX_W, C_DIM);

        /* Phase badge row */
        int y = UI_BAR_H + 17;
        if (in_cap) {
            fill(3, y, 8, 8, C_RED);
            text1(14, y, "REC", C_RED);
            text1(14 + 3 * 6 + 6, y, label, C_AMBER);
        } else {
            fill(3, y, 8, 8, C_DIM);
            text1(14, y, "MOVE", C_LABEL);
        }
        hline(0, y + 10, SGFX_W, C_DIM);

        /* Main instruction — large, bright */
        y += 14;
        if (ins1 && ins1[0]) text2(4, y, ins1, C_TEXT);
        y += 16;

        /* Detail line */
        if (ins2 && ins2[0]) {
            text1(4, y, ins2, C_LABEL);
            y += 10;
        }

        /* Next step preview — only during capture so user can prep */
        if (in_cap && next1 && next1[0]) {
            y += 2;
            char nbuf[48];
            snprintf(nbuf, sizeof nbuf, "next: %s", next1);
            text1(4, y, nbuf, C_DIM);
            y += 10;
        }

        /* Progress bar — fills left→right as phase time elapses */
        y = SGFX_H - UI_FOOT_H - 22;
        {
            const int bw = SGFX_W - 8, bh = 7;
            int dur = phase_dur > 0 ? phase_dur : 1;
            int elapsed_s = dur - remain;
            if (elapsed_s < 0) elapsed_s = 0;
            int filled = (elapsed_s * bw) / dur;
            if (filled > bw) filled = bw;
            fill(4, y, bw, bh, C_DIM);
            if (filled > 0) fill(4, y, filled, bh, in_cap ? C_RED : C_PANEL);
            rect(4, y, bw, bh, C_LABEL);
        }

        /* Time + frame count */
        y += 10;
        char info[40];
        snprintf(info, sizeof info, "%ds left   %d frames", remain, training_frames_written());
        text1(4, y, info, C_LABEL);

        ui_footbar("ESC:abort");
        return;
    }

    /* ── Done ────────────────────────────────────────────────────────────── */
    if (ui == TRAIN_UI_DONE) {
        fill(0, UI_BAR_H, SGFX_W, 14, C_PANEL);
        text1(4, UI_BAR_H + 3, "SESSION COMPLETE", C_GREEN);
        hline(0, UI_BAR_H + 13, SGFX_W, C_DIM);

        int y = UI_BAR_H + 22;
        char buf[48];
        snprintf(buf, sizeof buf, "frames: %d", training_frames_written());
        text2(4, y, buf, C_TEXT);
        y += 20;

        const char *fn = training_filename();
        text1(4, y, fn, C_ACCENT);
        y += 12;
        text1(4, y, "/non-magical-csi/", C_LABEL);

        ui_footbar("ENT/ESC:back to procedure select");
        return;
    }

    /* -- Free mode: label selector (labels from last non-free procedure) -- */
    if (ui == TRAIN_UI_FREE_LABEL) {
        fill(0, UI_BAR_H, SGFX_W, 14, C_PANEL);
        char title[48];
        snprintf(title, sizeof title, "FREE  [%s labels]",
                 training_proc_name(training_free_src_proc()));
        text1(4, UI_BAR_H + 3, title, C_ACCENT);
        hline(0, UI_BAR_H + 13, SGFX_W, C_DIM);

        int y = UI_BAR_H + 17;
        int count  = training_free_label_count();
        int cursor = training_free_label_cursor();
        for (int i = 0; i < count; i++, y += 14) {
            bool sel = (i == cursor);
            if (sel) {
                fill(0, y - 1, SGFX_W, 13, C_PANEL);
                vline(0, y - 1, 13, C_ACCENT);
            }
            text1(6, y, training_free_label_at(i), sel ? C_TEXT : C_LABEL);
        }
        ui_footbar("W/S:select  ENT:confirm  ESC:menu");
        return;
    }

    /* -- Free mode: cooldown + duration config -- */
    if (ui == TRAIN_UI_FREE_CFG) {
        fill(0, UI_BAR_H, SGFX_W, 14, C_PANEL);
        text1(4, UI_BAR_H + 3, "FREE RECORD - TIMING", C_ACCENT);
        hline(0, UI_BAR_H + 13, SGFX_W, C_DIM);

        int y = UI_BAR_H + 17;
        {
            char info[64];
            snprintf(info, sizeof info, "%s > %s",
                     training_session_name(), training_free_label());
            text1(4, y, info, C_LABEL);
        }
        y += 10;
        hline(0, y, SGFX_W, C_DIM);
        y += 5;

        int field = training_free_cfg_field();
        const char *field_labels[2] = { "Cooldown:", "Duration:" };
        int field_vals[2] = { training_free_cooldown(), training_free_cap_s() };

        for (int f = 0; f < 2; f++, y += 16) {
            bool sel = (f == field);
            if (sel) {
                fill(0, y - 2, SGFX_W, 14, C_PANEL);
                vline(0, y - 2, 14, C_ACCENT);
            }
            char row[32];
            snprintf(row, sizeof row, "%s  %d s", field_labels[f], field_vals[f]);
            text1(6, y, row, sel ? C_TEXT : C_LABEL);
            if (sel) text1(SGFX_W - 7 * 6, y, "A:- D:+", C_DIM);
        }

        y += 4;
        hline(0, y, SGFX_W, C_DIM);
        y += 5;
        text1(4, y, g_app.active_mode ? "mode: active" : "mode: passive", C_DIM);

        ui_footbar(g_app.active_mode
            ? "W/S:field  A/D:adj  P:passive  ENT:start  ESC:back"
            : "W/S:field  A/D:adj  P:active   ENT:start  ESC:back");
        return;
    }

    /* -- Free mode: capture done -- */
    if (ui == TRAIN_UI_FREE_DONE) {
        fill(0, UI_BAR_H, SGFX_W, 14, C_PANEL);
        text1(4, UI_BAR_H + 3, "CAPTURE DONE", C_GREEN);
        hline(0, UI_BAR_H + 13, SGFX_W, C_DIM);

        int y = UI_BAR_H + 17;
        {
            char lbuf[40];
            snprintf(lbuf, sizeof lbuf, "label: %s", training_free_label());
            text1(4, y, lbuf, C_ACCENT);
        }
        y += 10;
        hline(0, y, SGFX_W, C_DIM);
        y += 5;

        char fbuf[24];
        snprintf(fbuf, sizeof fbuf, "%d frames", training_frames_written());
        text2(4, y, fbuf, C_TEXT);
        y += 20;

        char dbuf[32];
        snprintf(dbuf, sizeof dbuf, "dur %ds  cooldown %ds",
                 training_free_cap_s(), training_free_cooldown());
        text1(4, y, dbuf, C_LABEL);
        y += 12;

        const char *fn = training_filename();
        text1(4, y, fn, C_DIM);
        y += 14;

        text1(4, y, "R:repeat   N:new label", C_ACCENT);

        ui_footbar("R:repeat  N:new label  ESC:finish");
        return;
    }
}

/* -- Channel Occupation survey -- */
static void ui_draw_chanoccup(void) {
    fill(0, 0, SGFX_W, SGFX_H, C_BG);

    int cur_ch = chanoccup_current_ch();
    bool settling = chanoccup_settling();

    /* Status bar shows current scanning channel */
    {
        char tag[24];
        snprintf(tag, sizeof tag, "CHANOCC ch%02d%s", cur_ch, settling ? "~" : " ");
        ui_statusbar(tag);
    }

    /* Scale is anchored to the historical peak so the view never squishes
     * when current values are low or zero — bars always reflect relative
     * activity compared to the busiest moment ever seen this session. */
    float peak = 1.0f;
    for (int ch = 1; ch <= 13; ch++) {
        float p = chanoccup_peak(ch);
        if (p > peak) peak = p;
    }

    /* 13 horizontal bars, 8px each, starting at UI_BAR_H */
    const int BAR_X    = 18;                      /* left edge of bar */
    const int BAR_W    = SGFX_W - BAR_X - 38;    /* 184px bar area   */
    const int VAL_X    = BAR_X + BAR_W + 2;
    const int ROW_H    = 8;

    for (int i = 0; i < 13; i++) {
        int ch  = i + 1;
        int y   = UI_BAR_H + i * ROW_H;
        float f = chanoccup_fps(ch);
        bool active = (ch == cur_ch);

        /* row background — slightly lighter for current channel */
        if (active) fill(0, y, SGFX_W, ROW_H, C_PANEL);

        /* channel label */
        char lbl[4]; snprintf(lbl, sizeof lbl, "%02d", ch);
        text1(2, y, lbl, active ? C_ACCENT : C_LABEL);

        /* bar fill */
        int filled = (int)((f / peak) * (float)BAR_W);
        if (filled > BAR_W) filled = BAR_W;
        fill(BAR_X, y + 1, BAR_W, ROW_H - 2, C_DIM);
        if (filled > 0) {
            /* colour: green→amber→red by fraction of peak */
            sgfx_rgba8_t bc;
            float frac = f / peak;
            if      (frac < 0.4f) bc = C_GREEN;
            else if (frac < 0.75f) bc = C_AMBER;
            else                   bc = C_RED;
            fill(BAR_X, y + 1, filled, ROW_H - 2, bc);
        }

        /* peak marker — 1px tick at historical max position */
        float pk = chanoccup_peak(ch);
        if (pk > 0.0f) {
            int pk_x = BAR_X + (int)((pk / peak) * (float)(BAR_W - 1));
            if (pk_x >= BAR_X + BAR_W) pk_x = BAR_X + BAR_W - 1;
            vline(pk_x, y, ROW_H, C_TEXT);
        }

        /* fps value: bright if current, dim-grey if stale (last non-zero) */
        {
            float disp = f;
            bool  stale = false;
            if (f <= 0.0f) {
                disp  = chanoccup_last_nz(ch);
                stale = (disp > 0.0f);
            }
            if (disp > 0.0f) {
                char vbuf[8];
                if (disp < 10.0f) snprintf(vbuf, sizeof vbuf, "%.1f", (double)disp);
                else               snprintf(vbuf, sizeof vbuf, "%.0f", (double)disp);
                sgfx_rgba8_t vc = stale ? (sgfx_rgba8_t){20,65,28,255}
                                        : (active ? C_ACCENT : C_TEXT);
                text1(VAL_X, y, vbuf, vc);
            }
        }

        /* accent stripe on active channel */
        if (active) vline(0, y, ROW_H, C_ACCENT);
    }

    ui_footbar(g_app.active_mode ? "C:ch  P:passive  ESC:menu"
                                 : "C:ch  P:active   ESC:menu");
}

/* ── File manager ───────────────────────────────────────────────────────── */
void ui_draw_fileman(void) {
    fill(0, 0, SGFX_W, SGFX_H, C_BG);
    ui_statusbar("FILES");

    if (!fileman_sd_ok()) {
        fill(0, UI_BAR_H, SGFX_W, UI_MAIN_H, C_BG);
        text2(6, UI_BAR_H + 20, "NO CARD", C_RED);
        text1(6, UI_BAR_H + 44, "insert SD card and re-enter", C_LABEL);
        ui_footbar("ESC:menu");
        return;
    }

    /* path header */
    fb_text_clip(4, UI_BAR_H + 2, fileman_cwd(), C_LABEL, 1, SGFX_W - 4);
    hline(0, UI_BAR_H + 11, SGFX_W, C_DIM);

    if (fileman_mode() == FM_CONFIRM) {
        text2(6, UI_BAR_H + 18, "DELETE FILE?", C_AMBER);
        fb_text_clip(6, UI_BAR_H + 40, fileman_confirm_name(), C_TEXT, 1, SGFX_W - 8);
        hline(6, UI_BAR_H + 51, SGFX_W - 12, C_DIM);
        text1(6, UI_BAR_H + 56, "OK:delete   BACK:cancel", C_LABEL);
        ui_footbar("OK:confirm  BACK:cancel");
        return;
    }

    if (fileman_mode() == FM_MSG) {
        text2(6, UI_BAR_H + 30, fileman_msg(), C_ACCENT);
        ui_footbar("");
        return;
    }

    int n = fileman_count();
    if (n == 0) {
        text1(4, UI_BAR_H + 25, "(empty)", C_DIM);
    } else {
        const int ITEM_H = 13;
        const int LIST_Y = UI_BAR_H + 13;
        int vis    = (UI_MAIN_YEND - LIST_Y) / ITEM_H;
        int scroll = fileman_scroll();
        int cursor = fileman_cursor();

        for (int i = scroll; i < n && i < scroll + vis; i++) {
            int  y   = LIST_Y + (i - scroll) * ITEM_H;
            bool sel = (i == cursor);
            if (sel) {
                fill(0, y - 1, SGFX_W, ITEM_H, C_PANEL);
                vline(0, y - 1, ITEM_H, C_ACCENT);
            }
            if (fileman_is_dir(i)) {
                text1(6, y, "/", C_LABEL);
                fb_text_clip(12, y, fileman_name(i), sel ? C_ACCENT : C_TEXT, 1, SGFX_W - 6);
            } else {
                fb_text_clip(6, y, fileman_name(i), sel ? C_ACCENT : C_TEXT, 1, SGFX_W - 68);
                char sb[12];
                long sz = fileman_size(i);
                if      (sz < 1024L)         snprintf(sb, sizeof sb, "%ldb",  sz);
                else if (sz < 1024L * 1024L) snprintf(sb, sizeof sb, "%ldK",  sz / 1024L);
                else                         snprintf(sb, sizeof sb, "%ldM",  sz / (1024L * 1024L));
                text1(SGFX_W - 4 - (int)strlen(sb) * 6, y, sb, sel ? C_LABEL : C_DIM);
            }
        }
        if (scroll > 0)
            text1(SGFX_W - 8, LIST_Y, "^", C_DIM);
        if (scroll + vis < n)
            text1(SGFX_W - 8, LIST_Y + vis * ITEM_H - 7, "v", C_DIM);
    }

    const char *cwd = fileman_cwd();
    bool at_root = (cwd[0] == '/' && cwd[1] == '\0');
    ui_footbar(at_root ? "OK:open/del  ESC:menu"
                       : "OK:open/del  A:up  ESC:menu");
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */
static float s_raw_motion = 0.f;   /* uncalibrated frame-to-frame delta, 0..100 */

void ui_csi_reset(void) {
    memset(s_wf,          0, sizeof s_wf);
    memset(s_motion_hist, 0, sizeof s_motion_hist);
    s_wf_head    = 0;
    s_wf_n       = 0;
    s_raw_motion = 0.f;
}

void ui_render(const csi_frame_t *latest, const float *amp_hist_flat, bool new_frame) {
    if (!s_dev) return;

    /* Only advance data-dependent state when a genuinely new frame arrived */
    if (new_frame && latest) {
        for (int i = 0; i < CSI_N_SUB; i++)
            s_wf[s_wf_head][i] = latest->amp[i];
        s_wf_head = (s_wf_head + 1) % CSI_HIST_LEN;
        if (s_wf_n < (uint32_t)CSI_HIST_LEN) s_wf_n++;

        /* Raw uncalibrated motion: mean |amp[t] - amp[t-1]| per subcarrier.
         * Used in MOTION mode when LOS calibration hasn't run yet.
         * Scale factor ~3: mean diff of ~10 amp units → score ~30/100. */
        if (s_wf_n >= 2) {
            int curr = ((s_wf_head - 1) + CSI_HIST_LEN) % CSI_HIST_LEN;
            int prev = ((s_wf_head - 2) + CSI_HIST_LEN) % CSI_HIST_LEN;
            float d = 0.f;
            for (int i = 0; i < CSI_N_SUB; i++)
                d += fabsf((float)s_wf[curr][i] - (float)s_wf[prev][i]);
            d /= (float)CSI_N_SUB;
            s_raw_motion = 0.15f * (d * 3.f) + 0.85f * s_raw_motion;
            if (s_raw_motion > 100.f) s_raw_motion = 100.f;
        }

        /* Motion history uses LOS-calibrated score when available, raw otherwise */
        float hist_val = (g_app.los_state == LOS_SCANNING)
                         ? g_app.motion_score
                         : s_raw_motion;
        memmove(&s_motion_hist[1], s_motion_hist, (CSI_HIST_LEN-1)*sizeof(float));
        s_motion_hist[0] = hist_val;
    }

    switch (g_app.mode) {
    case APP_MODE_MENU:
        break;
    case APP_MODE_LOS:
        ui_draw_los();
        break;
    case APP_MODE_SPECTRUM: {
        /* Use zero amp when waiting for first frame — renders empty chrome
         * instead of staying frozen on the previous screen. */
        static const uint8_t kZeroAmp[CSI_N_SUB] = {};
        const uint8_t *amp = latest ? latest->amp : kZeroAmp;

        /* Auto-gain: scan waterfall buffer + current amp for actual range */
        uint8_t lo = 255, hi = 0;
        int rows = (int)(s_wf_n < (uint32_t)CSI_HIST_LEN ? s_wf_n : CSI_HIST_LEN);
        for (int r = 0; r < rows; r++) {
            int ri = ((s_wf_head - 1 - r) + CSI_HIST_LEN) % CSI_HIST_LEN;
            for (int i = 0; i < CSI_N_SUB; i++) {
                if (s_wf[ri][i] < lo) lo = s_wf[ri][i];
                if (s_wf[ri][i] > hi) hi = s_wf[ri][i];
            }
        }
        for (int i = 0; i < CSI_N_SUB; i++) {
            if (amp[i] < lo) lo = amp[i];
            if (amp[i] > hi) hi = amp[i];
        }
        /* No data yet or flat signal: use a dim narrow range so the view is visibly empty */
        if (lo >= hi) { lo = 0; hi = 20; }
        else if ((int)hi - (int)lo < 20) {
            int mid = (int)lo + ((int)hi - (int)lo) / 2;
            lo = (uint8_t)(mid > 10  ? mid - 10 : 0);
            hi = (uint8_t)(mid < 245 ? mid + 10 : 255);
        }

        /* Throttled debug: spectrum auto-gain state every 5 s */
        if (g_csi_verbosity >= LOG_LEVEL_DBG) {
            static uint32_t s_spec_dbg_t = 0;
            uint32_t now = millis();
            if (now - s_spec_dbg_t >= 5000u) {
                s_spec_dbg_t = now;
                LOG_D("spec wf_n=%lu gain_range=[%u..%u] cur=[%u..%u]",
                      (unsigned long)s_wf_n, lo, hi,
                      amp[0], amp[CSI_N_SUB-1]);
            }
        }
        /* Hash of current amp[] — logs every 2 s; SAME=stall, changed=live */
        if (g_csi_verbosity >= LOG_LEVEL_DBG) {
            static uint32_t s_spec_hash_t = 0, s_spec_hash_prev = 0;
            uint32_t now = millis();
            if (now - s_spec_hash_t >= 2000u) {
                s_spec_hash_t = now;
                uint32_t h = fnv32(amp, CSI_N_SUB);
                LOG_D("spec amp_hash=%08lx %s",
                      (unsigned long)h,
                      h == s_spec_hash_prev ? "SAME(stall?)" : "changed");
                s_spec_hash_prev = h;
            }
        }

        ui_draw_spectrum(amp, s_wf, s_wf_head, lo, hi);
        break;
    }
    case APP_MODE_VARIANCE: {
        /* Windowed variance over the last s_wf_n frames — no lifetime accumulation */
        int n = (int)(s_wf_n < (uint32_t)CSI_HIST_LEN ? s_wf_n : CSI_HIST_LEN);
        float wmean[CSI_N_SUB] = {};
        float wvar[CSI_N_SUB]  = {};
        if (n > 1) {
            for (int f = 0; f < n; f++) {
                int ri = ((s_wf_head - 1 - f) + CSI_HIST_LEN) % CSI_HIST_LEN;
                for (int i = 0; i < CSI_N_SUB; i++)
                    wmean[i] += (float)s_wf[ri][i];
            }
            for (int i = 0; i < CSI_N_SUB; i++)
                wmean[i] /= (float)n;
            for (int f = 0; f < n; f++) {
                int ri = ((s_wf_head - 1 - f) + CSI_HIST_LEN) % CSI_HIST_LEN;
                for (int i = 0; i < CSI_N_SUB; i++) {
                    float d = (float)s_wf[ri][i] - wmean[i];
                    wvar[i] += d * d;
                }
            }
            for (int i = 0; i < CSI_N_SUB; i++)
                wvar[i] /= (float)(n - 1);
        }

        /* Throttled debug: variance window state every 5 s */
        if (g_csi_verbosity >= LOG_LEVEL_DBG) {
            static uint32_t s_var_dbg_t = 0;
            uint32_t now = millis();
            if (now - s_var_dbg_t >= 5000u) {
                s_var_dbg_t = now;
                float maxv = 0.f;
                int   maxi = 0, nnonzero = 0;
                for (int i = 0; i < CSI_N_SUB; i++) {
                    if (wvar[i] > maxv) { maxv = wvar[i]; maxi = i; }
                    if (wvar[i] > 0.f) nnonzero++;
                }
                LOG_D("var n=%d maxv=%.2f@sub%d nonzero=%d/56 mean[0]=%.1f mean[28]=%.1f",
                      n, (double)maxv, maxi, nnonzero,
                      (double)wmean[0], (double)wmean[28]);
            }
        }
        /* Hash of the latest raw ring-buffer row — proves whether new frames arrive */
        if (g_csi_verbosity >= LOG_LEVEL_DBG) {
            static uint32_t s_var_hash_t = 0, s_var_hash_prev = 0;
            uint32_t now = millis();
            if (now - s_var_hash_t >= 2000u) {
                s_var_hash_t = now;
                uint32_t h = 0;
                if (s_wf_n > 0) {
                    int ri = ((s_wf_head - 1) + CSI_HIST_LEN) % CSI_HIST_LEN;
                    h = fnv32(s_wf[ri], CSI_N_SUB);
                }
                LOG_D("var row_hash=%08lx %s wf_n=%lu",
                      (unsigned long)h,
                      h == s_var_hash_prev ? "SAME(stall?)" : "changed",
                      (unsigned long)s_wf_n);
                s_var_hash_prev = h;
            }
        }

        ui_draw_variance(wvar, wmean);
        break;
    }
    case APP_MODE_MOTION: {
        /* Use LOS-calibrated score when scanning, raw delta score otherwise */
        float motion_score = (g_app.los_state == LOS_SCANNING)
                             ? g_app.motion_score
                             : s_raw_motion;
        ui_draw_motion(motion_score, s_motion_hist);
        break;
    }
    case APP_MODE_CORR: {
        static const uint8_t kZeroAmp[CSI_N_SUB] = {};
        ui_draw_corr(latest ? latest->amp : kZeroAmp);
        break;
    }
    case APP_MODE_TRAINING:
        ui_draw_training();
        break;
    case APP_MODE_CHANOCCUP:
        ui_draw_chanoccup();
        break;
    case APP_MODE_FILEMAN:
        ui_draw_fileman();
        break;
    default:
        break;
    }

    (void)amp_hist_flat;
}
