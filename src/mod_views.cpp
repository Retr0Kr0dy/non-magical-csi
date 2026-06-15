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
 * Color palette
 * ─────────────
 *   C_BG     ( 5, 10, 18)  very dark navy
 *   C_TEXT   (200,220,235) pale blue-white
 *   C_ACCENT (  0,180,255) cyan
 *   C_GREEN  (  0,220, 60) green
 *   C_AMBER  (255,160,  0) amber
 *   C_RED    (255, 48, 32) red
 *   C_DIM    ( 18, 45, 70) dim blue (bar backgrounds)
 *   C_LABEL  (110,140,165) grey-blue label text
 */

#include <Arduino.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "mod_views.h"
#include "mod_los.h"
#include "mod_csi.h"
#include "app.h"
#include "sgfx.h"
#include "sgfx_fb.h"
#include "sgfx_font_builtin.h"

/* ── Palette ─────────────────────────────────────────────────────────────── */
#define C_BG     ((sgfx_rgba8_t){  5, 10, 18,255})
#define C_TEXT   ((sgfx_rgba8_t){200,220,235,255})
#define C_ACCENT ((sgfx_rgba8_t){  0,180,255,255})
#define C_GREEN  ((sgfx_rgba8_t){  0,220, 60,255})
#define C_AMBER  ((sgfx_rgba8_t){255,160,  0,255})
#define C_RED    ((sgfx_rgba8_t){255, 48, 32,255})
#define C_DIM    ((sgfx_rgba8_t){ 18, 45, 70,255})
#define C_LABEL  ((sgfx_rgba8_t){110,140,165,255})
#define C_DARK   ((sgfx_rgba8_t){  2,  5, 10,255})
#define C_BAR_HI ((sgfx_rgba8_t){  0,200,255,255})
#define C_BAR_LO ((sgfx_rgba8_t){ 10, 60,120,255})
#define C_PANEL  ((sgfx_rgba8_t){  8, 20, 36,255})

static sgfx_device_t  *s_dev = nullptr;
static sgfx_fb_t       s_fb;
static sgfx_present_t  s_pr;
static bool            s_fb_ok = false;

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

/* amplitude (0..255) → waterfall heat color */
static sgfx_rgba8_t heat(uint8_t v) {
    if (v < 128) {
        uint8_t t = v * 2;
        return (sgfx_rgba8_t){
            (uint8_t)(5  - (uint8_t)(5  * t / 255)),
            (uint8_t)(10 + (uint8_t)(190 * t / 255)),
            (uint8_t)(18 + (uint8_t)(237 * t / 255)),
            255
        };
    } else {
        uint8_t t = (v - 128) * 2;
        return (sgfx_rgba8_t){
            (uint8_t)(0  + t),
            (uint8_t)(200 + (uint8_t)(55 * t / 255)),
            255,
            255
        };
    }
}

/* score (0..100) → status color (threshold matches LOS_SILENT_BELOW = 20) */
static sgfx_rgba8_t score_color(float s) {
    if (s < 20.0f) return C_GREEN;
    if (s < 45.0f) return C_AMBER;
    if (s < 75.0f) return (sgfx_rgba8_t){255,100,0,255};
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
    { "MENU",    "" },
    { "LOS",     "Line-of-sight disturbance" },
    { "SPECTRUM","Subcarrier amplitude waterfall" },
    { "VARIANCE","Per-subcarrier variance bars" },
    { "MOTION",  "Scalar motion score + history" },
    { "CORR",    "Cross-subcarrier correlation" },
    { "CONSOLE", "Serial console on screen" },
};

void ui_draw_menu(int sel) {
    fill(0, 0, SGFX_W, SGFX_H, C_BG);

    /* Header */
    fill(0, 0, SGFX_W, 28, C_DARK);
    text2(6, 6, "NON MAGICAL CSI", C_ACCENT);
    hline(0, 27, SGFX_W, C_DIM);

    /* Mode list — one line per mode, 13 px step */
    int y = 31;
    for (int i = 1; i < APP_MODE__COUNT; i++) {
        bool active = (i == sel);
        if (active) {
            fill(0, y - 1, SGFX_W, 13, C_PANEL);
            vline(0, y - 1, 13, C_ACCENT);   /* accent left bar */
        }
        char num[4]; snprintf(num, sizeof num, "%d", i);
        sgfx_rgba8_t nc = active ? C_LABEL : (sgfx_rgba8_t){50,70,90,255};
        sgfx_rgba8_t tc = active ? C_ACCENT : C_TEXT;
        text1(4,  y, num,           nc);
        text1(14, y, kModes[i].tag, tc);
        y += 13;
    }

    /* Selected description — below list, separated */
    int desc_y = y + 2;
    if (sel > 0 && sel < APP_MODE__COUNT) {
        hline(0, desc_y, SGFX_W, C_DIM);
        fb_text_clip(4, desc_y + 3, kModes[sel].desc, C_LABEL, 1, SGFX_W - 4);
    }

    ui_footbar("W/S:select  ENT:open  1-6:jump");
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
        text1(6, UI_BAR_H + 54, "C    cycle channel (1/6/11/13)", C_LABEL);
        text1(6, UI_BAR_H + 65, "R    recalibrate", C_LABEL);
        /* Mode selector */
        text1(6, UI_BAR_H + 77, "P    mode:", C_LABEL);
        bool am = g_app.los_active_mode;
        text1(66, UI_BAR_H + 77, "ACTIVE",
              am  ? C_GREEN : (sgfx_rgba8_t){35,55,75,255});
        text1(66 + 42, UI_BAR_H + 77, "/", C_DIM);
        text1(66 + 50, UI_BAR_H + 77, "PASSIVE",
              !am ? C_TEXT  : (sgfx_rgba8_t){35,55,75,255});
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
                text1(4,  y, buf, active ? C_LABEL : (sgfx_rgba8_t){60,80,100,255});
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
        rect(bx, by, bw, bh, (sgfx_rgba8_t){30,70,110,255});

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
    rect(bx, by, bw, 10, (sgfx_rgba8_t){25,55,90,255});

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

/* ── SPECTRUM waterfall ──────────────────────────────────────────────────── */
void ui_draw_spectrum(const uint8_t amp[CSI_N_SUB],
                      const uint8_t waterfall[CSI_HIST_LEN][CSI_N_SUB],
                      int wf_head) {
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
        int bh = (int)amp[i] * chart_h / 255;
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
        fill(dc_x, chart_y, dc_w, chart_h, (sgfx_rgba8_t){12,25,42,255});
        /* "DC" label centred over the gap */
        int lx = dc_x + dc_w / 2 - 6;
        text1(lx, chart_y + (chart_h - 7) / 2, "DC", (sgfx_rgba8_t){40,70,100,255});
    }

    /* Direct row writes — avoid 56*rows fill() + dirty-tile calls per frame */
    int rows_to_draw = wf_h < CSI_HIST_LEN ? wf_h : CSI_HIST_LEN;
    for (int row = 0; row < rows_to_draw; row++) {
        int ri = ((wf_head - 1 - row) + CSI_HIST_LEN) % CSI_HIST_LEN;
        int dy = wf_y + row;
        for (int i = 0; i < CSI_N_SUB; i++) {
            uint16_t pix = pack565(heat(waterfall[ri][i]));
            int x = bar_off + i * bar_w;
            fb_raw_hspan(x, dy, bar_w - 1, pix);
        }
    }
    if (rows_to_draw > 0)
        sgfx_fb_mark_dirty_px(&s_fb, bar_off, wf_y, bar_w * CSI_N_SUB, rows_to_draw);

    ui_footbar("UP/DN:ch  ESC:menu  L:LOS");
}

/* ── VARIANCE bars ───────────────────────────────────────────────────────── */
void ui_draw_variance(const float var[CSI_N_SUB], const float mean_[CSI_N_SUB]) {
    ui_statusbar("VARIANCE");
    fill(0, UI_BAR_H, SGFX_W, UI_MAIN_H, C_BG);

    float maxv = 1.0f;
    for (int i = 0; i < CSI_N_SUB; i++)
        if (var[i] > maxv) maxv = var[i];

    /* Header */
    text1(4, UI_BAR_H + 2, "per-subcarrier variance", C_LABEL);

    int ay  = UI_BAR_H + 13;
    int ah  = UI_MAIN_YEND - ay - 16;
    int bar_w   = SGFX_W / CSI_N_SUB;
    int bar_off = (SGFX_W - bar_w * CSI_N_SUB) / 2;

    for (int i = 0; i < CSI_N_SUB; i++) {
        int bh = (int)((float)ah * var[i] / maxv);
        int x  = bar_off + i * bar_w;
        fill(x, ay, bar_w - 1, ah, C_DIM);
        if (bh > 0) {
            float t = var[i] / maxv;
            sgfx_rgba8_t bc = {
                (uint8_t)(uint8_t)(255 * t),
                (uint8_t)(180 - (uint8_t)(180 * t)),
                (uint8_t)(255 - (uint8_t)(200 * t)),
                255
            };
            fill(x, ay + ah - bh, bar_w - 1, bh, bc);
        }
    }

    hline(bar_off, ay + ah, bar_w * CSI_N_SUB, C_DIM);
    text1(bar_off,                          ay + ah + 3, "-28", C_LABEL);
    text1(SGFX_W - bar_off - 18,           ay + ah + 3, "+28", C_LABEL);

    /* DC null band — subs 23-32 are zero by OFDM spec, shade + label */
    {
        int dc_x = bar_off + 23 * bar_w;
        int dc_w = 10 * bar_w;
        fill(dc_x, ay, dc_w, ah, (sgfx_rgba8_t){10,20,34,255});
        int lx = dc_x + dc_w / 2 - 6;
        text1(lx, ay + ah / 2 - 4, "DC", (sgfx_rgba8_t){40,70,100,255});
        text1(lx - 6, ay + ah / 2 + 4, "NULL", (sgfx_rgba8_t){35,55,80,255});
    }

    char mvb[20]; snprintf(mvb, sizeof mvb, "max:%.1f", (double)maxv);
    text1(4, UI_MAIN_YEND - 12, mvb, C_LABEL);

    ui_footbar("R:reset  ESC:menu");
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
        hline(sx + 1, thr_y, sw - 2, (sgfx_rgba8_t){60,80,40,255});

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

/* ── Dispatch ────────────────────────────────────────────────────────────── */
void ui_render(const csi_frame_t *latest, const float *amp_hist_flat) {
    if (!s_dev) return;

    static uint8_t  s_wf[CSI_HIST_LEN][CSI_N_SUB];
    static float    s_motion_hist[CSI_HIST_LEN];
    static int      s_wf_head = 0;
    static float    s_var[CSI_N_SUB];
    static float    s_var_mean[CSI_N_SUB];
    static uint32_t s_var_n = 0;

    if (latest) {
        for (int i = 0; i < CSI_N_SUB; i++)
            s_wf[s_wf_head][i] = latest->amp[i];
        s_wf_head = (s_wf_head + 1) % CSI_HIST_LEN;

        memmove(&s_motion_hist[1], s_motion_hist, (CSI_HIST_LEN-1)*sizeof(float));
        s_motion_hist[0] = g_app.motion_score;

        s_var_n++;
        for (int i = 0; i < CSI_N_SUB; i++) {
            float x = (float)latest->amp[i];
            float d = x - s_var_mean[i];
            s_var_mean[i] += d / (float)s_var_n;
            s_var[i]      += d * (x - s_var_mean[i]);
        }
    }

    switch (g_app.mode) {
    case APP_MODE_MENU:
        break;
    case APP_MODE_LOS:
        ui_draw_los();
        break;
    case APP_MODE_SPECTRUM:
        if (latest)
            ui_draw_spectrum(latest->amp, s_wf, s_wf_head);
        break;
    case APP_MODE_VARIANCE: {
        float samplevar[CSI_N_SUB];
        for (int i = 0; i < CSI_N_SUB; i++)
            samplevar[i] = (s_var_n > 1) ? s_var[i] / (float)(s_var_n-1) : 0.f;
        ui_draw_variance(samplevar, s_var_mean);
        break;
    }
    case APP_MODE_MOTION:
        ui_draw_motion(g_app.motion_score, s_motion_hist);
        break;
    case APP_MODE_CORR:
        if (latest) ui_draw_corr(latest->amp);
        break;
    default:
        break;
    }

    (void)amp_hist_flat;
}
