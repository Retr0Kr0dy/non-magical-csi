/*
 * csi_app.cpp — CSI Sense firmware main application
 *
 * Architecture mirrors neutrino but owns the display (SGFX used directly,
 * not as a text terminal mirror).  Serial console (konsole) runs in parallel
 * for configuration and debug.
 *
 * Main loop
 * ─────────
 *   kbd_poll()      → SIC key events → ring buffer → konsole + app nav
 *   csi_pop_frame() → feed los_update(), update views ring buffers
 *   los_update()    → state machine + audio scheduling
 *   konsole_poll()  → serial commands
 *   ui_render()     → SGFX scene for current mode
 *   sgfx_present()  → flush to display (called inside ui_render)
 *
 * Serial commands
 * ───────────────
 *   sys                      — firmware + chip info
 *   hw                       — SIC driver list
 *   csi start [ch]           — start CSI on channel (default 6)
 *   csi stop                 — stop
 *   csi info                 — frame count, FPS, per-sub mean/var
 *   csi scan                 — scan APs (brief pause)
 *   csi ch <N>               — change channel
 *   csi ap <idx>             — select AP from last scan
 *   mode [los|spec|var|motion|corr|console]  — switch mode
 *   los start                — start LOS sequence
 *   los stop                 — stop LOS
 *   los recal                — recalibrate
 *   beep [hz] [ms]           — test speaker
 */

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "sic/sic.h"
#include "sic/audio/amp.h"
#include "sic/bus/i2c_bus.h"
#include "konsole/konsole.h"
#include "konsole/static.h"
}

#include "sgfx.h"
#include "sgfx_port.h"

#include "app.h"
#include "mod_csi.h"
#include "mod_los.h"
#include "mod_views.h"

/* ── Global app state ───────────────────────────────────────────────────── */
app_state_t g_app = {};

/* ── Forward declarations ───────────────────────────────────────────────── */
static void menu_up(void);
static void menu_down(void);
static void enter_mode(app_mode_t m);
static void app_handle_char(char c);

/* pending deferred AP scan — set by 'F' shortcut, consumed in csi_app_run */
static bool s_scan_pending = false;

/* ── Console instance ───────────────────────────────────────────────────── */
static struct konsole        g_ks;
static struct kon_line_state g_line;

/* ── Display ─────────────────────────────────────────────────────────────── */
static sgfx_device_t s_gfx;
static uint8_t       s_gfx_scratch[SGFX_SCRATCH_BYTES];
static int           s_gfx_ok = 0;

/* ── On-screen terminal (MODE_CONSOLE) ──────────────────────────────────── */
#define SCR_SCALE 1
#define SCR_CHAR_W 6
#define SCR_CHAR_H 8
#define SCR_COLS   (SGFX_W / SCR_CHAR_W)
#define SCR_ROWS   (UI_MAIN_H / SCR_CHAR_H)
static char    s_scr[SCR_ROWS][SCR_COLS + 1];
static int     s_scr_col = 0, s_scr_row = 0;

/* ── Menu selection (declared here — used by process_nav before menu_* fns) */
static int s_menu_sel = 1;   /* 1..APP_MODE__COUNT-1 */

typedef enum { SCR_N, SCR_E, SCR_C } scr_esc_t;
static scr_esc_t s_esc = SCR_N;

static void scr_scroll(void) {
    memmove(s_scr[0], s_scr[1], (SCR_ROWS - 1) * (SCR_COLS + 1));
    memset(s_scr[SCR_ROWS - 1], 0, SCR_COLS + 1);
}
static void scr_newline(void) {
    s_scr_col = 0;
    if (++s_scr_row >= SCR_ROWS) { s_scr_row = SCR_ROWS - 1; scr_scroll(); }
}
static void scr_putc(char c) {
    if (s_esc == SCR_E) { s_esc = (c == '[') ? SCR_C : SCR_N; return; }
    if (s_esc == SCR_C) {
        if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='~') s_esc = SCR_N;
        return;
    }
    if (c == '\033') { s_esc = SCR_E; return; }
    if (c == '\r')   { s_scr_col = 0; return; }
    if (c == '\n')   { scr_newline(); return; }
    if (c == '\b' || c == 0x7f) {
        if (s_scr_col > 0) { s_scr[s_scr_row][--s_scr_col] = 0; }
        return;
    }
    if (c < 32 || c > 126) return;
    if (s_scr_col >= SCR_COLS) scr_newline();
    s_scr[s_scr_row][s_scr_col++] = c;
    s_scr[s_scr_row][s_scr_col]   = 0;
}

/* ── Keyboard ring (SIC → konsole + app nav) ─────────────────────────────── */
#define KBD_BUF 32
static uint8_t s_kbd[KBD_BUF];
static int s_kh = 0, s_kt = 0;
static void kbd_push(uint8_t c) {
    int n = (s_kh + 1) % KBD_BUF;
    if (n != s_kt) { s_kbd[s_kh] = c; s_kh = n; }
}
static int kbd_avail(void) { return (s_kh - s_kt + KBD_BUF) % KBD_BUF; }
static int kbd_pop(void) {
    if (s_kh == s_kt) return -1;
    uint8_t c = s_kbd[s_kt]; s_kt = (s_kt + 1) % KBD_BUF; return c;
}
/* Abstract nav events (separate from konsole's text input) */
typedef enum { NAV_NONE, NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_OK, NAV_BACK } nav_t;
static nav_t s_nav_pending = NAV_NONE;

static void neu_kbd_poll(void) {
    sic_key_event_t ev;
    for (int i = 0; i < 8; i++) {
        if (sic_key_poll(&ev) <= 0) break;
        if (!ev.pressed) continue;
        switch (ev.code) {
        /* Navigation events routed to app, NOT to konsole */
        case SIC_KEY_UP:    s_nav_pending = NAV_UP;    break;
        case SIC_KEY_DOWN:  s_nav_pending = NAV_DOWN;  break;
        case SIC_KEY_LEFT:  s_nav_pending = NAV_LEFT;  break;
        case SIC_KEY_RIGHT: s_nav_pending = NAV_RIGHT; break;
        case SIC_KEY_ENTER: s_nav_pending = NAV_OK;
                            kbd_push(0x0D); break;   /* also let konsole handle Enter */
        case SIC_KEY_ESC:       s_nav_pending = NAV_BACK; kbd_push(0x1B); break;
        case SIC_KEY_BACKSPACE: s_nav_pending = NAV_BACK; kbd_push(0x08); break;
        default:
            if (ev.ascii) {
                /* WASD = arrows, DEL = back — unconditional */
                if      (ev.ascii == 'w' || ev.ascii == 'W') { s_nav_pending = NAV_UP;    break; }
                else if (ev.ascii == 's' || ev.ascii == 'S') { s_nav_pending = NAV_DOWN;  break; }
                else if (ev.ascii == 'a' || ev.ascii == 'A') { s_nav_pending = NAV_LEFT;  break; }
                else if (ev.ascii == 'd' || ev.ascii == 'D') { s_nav_pending = NAV_RIGHT; break; }
                else if (ev.ascii == 0x7F)                   { s_nav_pending = NAV_BACK;  break; }
                /* Menu: number keys jump immediately without needing Enter */
                else if (g_app.mode == APP_MODE_MENU
                         && ev.ascii >= '1' && ev.ascii <= '6') {
                    s_menu_sel    = ev.ascii - '0';
                    s_nav_pending = NAV_OK;
                    break;
                }
                /* LOS mode: single-char shortcuts are immediate, bypass konsole */
                else if (g_app.mode == APP_MODE_LOS
                         && (ev.ascii == 'f' || ev.ascii == 'F'
                          || ev.ascii == 'c' || ev.ascii == 'C'
                          || ev.ascii == 'r' || ev.ascii == 'R'
                          || ev.ascii == 'q' || ev.ascii == 'Q'
                          || ev.ascii == 'p' || ev.ascii == 'P'
                          || ev.ascii == '\r')) {
                    app_handle_char(ev.ascii);
                    break;
                }
                kbd_push((uint8_t)ev.ascii);
            }
        }
    }
}

static void process_nav(void) {
    nav_t nav = s_nav_pending;
    s_nav_pending = NAV_NONE;
    if (nav == NAV_NONE) return;

    /* ── Menu ── */
    if (g_app.mode == APP_MODE_MENU) {
        if (nav == NAV_UP)   { menu_up();   return; }
        if (nav == NAV_DOWN) { menu_down(); return; }
        if (nav == NAV_OK)   { enter_mode((app_mode_t)s_menu_sel); return; }
        return;
    }

    /* ── LOS — handled before global back so los_stop() fires correctly ── */
    if (g_app.mode == APP_MODE_LOS) {
        /* ESC / DEL / backspace → stop and return to menu */
        if (nav == NAV_BACK) {
            csi_active_stop();
            los_stop();
            g_app.mode = APP_MODE_MENU;
            return;
        }
        /* AP selection list navigation */
        if (g_app.los_state == LOS_SELECTING) {
            int n = csi_ap_count();
            if (nav == NAV_UP) {
                if (--g_app.los_ap_sel < 0) g_app.los_ap_sel = n > 0 ? n - 1 : 0;
                return;
            }
            if (nav == NAV_DOWN) {
                if (++g_app.los_ap_sel >= n && n > 0) g_app.los_ap_sel = 0;
                return;
            }
            if (nav == NAV_OK && n > 0) {
                csi_select_ap(g_app.los_ap_sel);
                if (g_app.los_active_mode) csi_active_start();
                los_start();
                return;
            }
        }
        if (nav == NAV_OK && g_app.los_state == LOS_IDLE) { los_start(); return; }
        return;
    }

    /* ── Global back for all other modes ── */
    if (nav == NAV_BACK) { g_app.mode = APP_MODE_MENU; return; }

    /* Channel change with left/right in spectrum/motion modes */
    if (g_app.mode == APP_MODE_SPECTRUM || g_app.mode == APP_MODE_MOTION) {
        if (nav == NAV_RIGHT) csi_set_channel(g_app.wifi_channel < 13 ? g_app.wifi_channel+1 : 1);
        if (nav == NAV_LEFT)  csi_set_channel(g_app.wifi_channel > 1  ? g_app.wifi_channel-1 : 13);
    }
}

/* ── konsole I/O ─────────────────────────────────────────────────────────── */
static size_t io_avail(void *ctx) { (void)ctx; return 1024; }
static size_t io_read(void *ctx,  uint8_t *buf, size_t len) {
    (void)ctx; size_t n = 0;
    while (n < len && kbd_avail()) { int c = kbd_pop(); if (c >= 0) buf[n++] = (uint8_t)c; }
    if (n < len) {
        int r = Serial.available();
        while (r-- > 0 && n < len) buf[n++] = (uint8_t)Serial.read();
    }
    return n;
}
static size_t io_write(void *ctx, const uint8_t *buf, size_t len) {
    (void)ctx;
    size_t n = Serial.write(buf, len);
    for (size_t i = 0; i < len; i++) scr_putc((char)buf[i]);
    return n;
}
static uint32_t io_millis(void *ctx) { (void)ctx; return millis(); }

static const struct konsole_io s_io = {
    .read_avail = io_avail,
    .read       = io_read,
    .write      = io_write,
    .millis     = io_millis,
    .ctx        = nullptr
};

/* ── Menu navigation state ───────────────────────────────────────────────── */
static void enter_mode(app_mode_t m) {
    g_app.mode = m;
}

static void menu_up(void) {
    if (--s_menu_sel < 1) s_menu_sel = APP_MODE__COUNT - 1;
}
static void menu_down(void) {
    if (++s_menu_sel >= APP_MODE__COUNT) s_menu_sel = 1;
}

/* Process a single printable/control char for app navigation.
 * Called after konsole has had first pick (konsole processes Enter,
 * backspace, history — app sees single-char shortcuts). */
/* Unknown-command hook: single-char input that isn't a konsole command
 * is dispatched here for mode navigation.                                   */
static int app_unknown_cmd(struct konsole *ks, const char *line) {
    (void)ks;
    if (line && line[0] && line[1] == 0)
        app_handle_char(line[0]);
    return 0;
}

static void app_handle_char(char c) {
    if (g_app.mode == APP_MODE_MENU) {
        if (c == '\r' || c == '\n') {
            enter_mode((app_mode_t)s_menu_sel);
            return;
        }
        if (c >= '1' && c <= '6') {
            s_menu_sel = c - '0';
            enter_mode((app_mode_t)s_menu_sel);
            return;
        }
        return;
    }

    /* Global: ESC or 'm' → back to menu */
    if (c == 0x1B || c == 'm' || c == 'M') {
        g_app.mode = APP_MODE_MENU;
        return;
    }

    /* Mode-specific shortcuts */
    if (g_app.mode == APP_MODE_LOS) {
        if (c == '\r' || c == '\n') {
            if (g_app.los_state == LOS_IDLE) los_start();
            if (g_app.los_state == LOS_SELECTING && csi_ap_count() > 0) {
                csi_select_ap(g_app.los_ap_sel);
                if (g_app.los_active_mode) csi_active_start();
                los_start();
            }
        }
        /* P = toggle active / passive injection mode */
        if (c == 'p' || c == 'P') {
            g_app.los_active_mode = !g_app.los_active_mode;
            /* apply immediately if already sensing */
            if (g_app.los_state == LOS_SCANNING || g_app.los_state == LOS_CALIBRATING) {
                if (g_app.los_active_mode) csi_active_start();
                else                        csi_active_stop();
            }
        }
        /* R = recalibrate */
        if (c == 'r' || c == 'R') los_recalibrate();
        /* F = find / scan APs — deferred so display shows "SCANNING" first */
        if (c == 'f' || c == 'F') {
            g_app.los_ap_sel      = 0;
            g_app.los_state       = LOS_SELECTING;
            g_app.los_is_scanning = true;
            s_scan_pending        = true;
        }
        /* C = cycle channel (1→6→11→13→1) */
        if (c == 'c' || c == 'C') {
            static const int kCh[] = {1, 6, 11, 13};
            static int s_ch_idx = 1;   /* default points at ch6 */
            int cur = g_app.wifi_channel;
            for (int i = 0; i < 4; i++) if (kCh[i] == cur) { s_ch_idx = i; break; }
            s_ch_idx = (s_ch_idx + 1) % 4;
            csi_set_channel(kCh[s_ch_idx]);
        }
        /* Q = quit LOS */
        if (c == 'q' || c == 'Q') { csi_active_stop(); los_stop(); g_app.mode = APP_MODE_MENU; }
    }

    if (c == '+' || c == '=') csi_set_channel(g_app.wifi_channel < 13 ? g_app.wifi_channel + 1 : 1);
    if (c == '-' || c == '_') csi_set_channel(g_app.wifi_channel > 1  ? g_app.wifi_channel - 1 : 13);
}

/* ── konsole commands ────────────────────────────────────────────────────── */
static int cmd_sys(struct konsole *ks, int argc, char **argv) {
    (void)argc; (void)argv;
    kon_printf(ks, "fw      : csi-sense %s\r\n", CSI_SENSE_VERSION);
    kon_printf(ks, "board   : %s\r\n", BOARD_NAME);
    kon_printf(ks, "uptime  : %u ms\r\n", (unsigned)millis());
    kon_printf(ks, "display : %dx%d\r\n", SGFX_W, SGFX_H);
    sic_sysinfo_t si; memset(&si, 0, sizeof si);
    if (sic_sysinfo(&si) == 0)
        kon_printf(ks, "chip    : %s rev%u @ %u MHz  psram=%uKB\r\n",
                   si.chip_model, si.chip_rev, si.cpu_mhz, si.psram_bytes/1024);
    return 0;
}

static int cmd_hw(struct konsole *ks, int argc, char **argv) {
    (void)argc; (void)argv;
    static const struct { sic_func_id_t f; const char *l; } kF[] = {
        {SIC_F_KSCAN,"kbd"},{SIC_F_MIC,"mic"},{SIC_F_AMP,"amp"},
        {SIC_F_CHARGER,"charger"},{SIC_F_IR_TX,"ir"},{SIC_F_SD,"sd"},
    };
    for (int i = 0; i < (int)(sizeof kF / sizeof kF[0]); i++) {
        int n = sic_count_fn(kF[i].f);
        kon_printf(ks, "%-8s: ", kF[i].l);
        if (!n) { kon_printf(ks, "none\r\n"); continue; }
        for (int j = 0; j < n; j++)
            kon_printf(ks, "%s%s", sic_name_fn(kF[i].f, j), j+1<n?", ":"");
        kon_printf(ks, "\r\n");
    }
    return 0;
}

static int cmd_csi(struct konsole *ks, int argc, char **argv) {
    if (argc < 2) {
        kon_printf(ks, "usage: csi start [ch] | stop | info | scan | ch N | ap N | active start|stop\r\n");
        return 0;
    }
    if (!strcmp(argv[1], "start")) {
        int ch = (argc >= 3) ? atoi(argv[2]) : (g_app.wifi_channel ? g_app.wifi_channel : 6);
        if (ch < 1 || ch > 13) { kon_printf(ks, "channel must be 1-13\r\n"); return -1; }
        if (g_app.csi_running) csi_deinit();
        csi_init(ch);
        kon_printf(ks, "CSI started, channel %d\r\n", ch);
        return 0;
    }
    if (!strcmp(argv[1], "stop")) {
        csi_deinit();
        kon_printf(ks, "CSI stopped\r\n");
        return 0;
    }
    if (!strcmp(argv[1], "info")) {
        csi_stats_t st; csi_get_stats(&st);
        kon_printf(ks, "frames: %lu  fps: %lu  ch: %d\r\n",
                   (unsigned long)st.frame_count, (unsigned long)st.fps,
                   g_app.wifi_channel);
        kon_printf(ks, "active: %s  tx_ok: %lu  tx_err: %lu\r\n",
                   csi_active_running() ? "YES" : "NO",
                   (unsigned long)csi_active_tx_ok(),
                   (unsigned long)csi_active_tx_err());
        kon_printf(ks, "sub   mean  var\r\n");
        for (int i = 0; i < CSI_N_SUB; i += 4)
            kon_printf(ks, "[%2d] %.1f %.2f | [%2d] %.1f %.2f | [%2d] %.1f %.2f | [%2d] %.1f %.2f\r\n",
                       i,   st.mean[i],   st.var[i],
                       i+1, st.mean[i+1], st.var[i+1],
                       i+2, st.mean[i+2], st.var[i+2],
                       i+3, st.mean[i+3], st.var[i+3]);
        return 0;
    }
    if (!strcmp(argv[1], "scan")) {
        kon_printf(ks, "scanning APs...\r\n");
        int n = csi_scan_aps();
        kon_printf(ks, "found %d AP(s):\r\n", n);
        for (int i = 0; i < n; i++) {
            const uint8_t *b = csi_ap_bssid(i);
            kon_printf(ks, "  [%d] ch%d %3d dBm %02X:%02X:%02X:%02X:%02X:%02X  %s\r\n",
                       i, csi_ap_channel(i), csi_ap_rssi(i),
                       b[0],b[1],b[2],b[3],b[4],b[5], csi_ap_ssid(i));
        }
        return 0;
    }
    if (!strcmp(argv[1], "ch") && argc >= 3) {
        int ch = atoi(argv[2]);
        if (ch < 1 || ch > 13) { kon_printf(ks, "1-13\r\n"); return -1; }
        csi_set_channel(ch);
        kon_printf(ks, "channel → %d\r\n", ch);
        return 0;
    }
    if (!strcmp(argv[1], "ap") && argc >= 3) {
        int idx = atoi(argv[2]);
        if (idx < 0 || idx >= csi_ap_count()) { kon_printf(ks, "invalid index\r\n"); return -1; }
        csi_select_ap(idx);
        kon_printf(ks, "monitoring AP: %s  ch%d\r\n",
                   csi_ap_ssid(idx), csi_ap_channel(idx));
        return 0;
    }
    if (!strcmp(argv[1], "active") && argc >= 3) {
        if (!strcmp(argv[2], "start")) { csi_active_start(); kon_printf(ks, "active injection started\r\n"); return 0; }
        if (!strcmp(argv[2], "stop"))  { csi_active_stop();  kon_printf(ks, "active injection stopped\r\n"); return 0; }
    }
    kon_printf(ks, "unknown csi subcommand\r\n");
    return -1;
}

static int cmd_mode(struct konsole *ks, int argc, char **argv) {
    if (argc < 2) {
        kon_printf(ks, "mode: %d  (los|spec|var|motion|corr|console|menu)\r\n", g_app.mode);
        return 0;
    }
    const char *m = argv[1];
    if (!strcmp(m,"menu"))    g_app.mode = APP_MODE_MENU;
    else if (!strcmp(m,"los"))     g_app.mode = APP_MODE_LOS;
    else if (!strcmp(m,"spec"))    g_app.mode = APP_MODE_SPECTRUM;
    else if (!strcmp(m,"var"))     g_app.mode = APP_MODE_VARIANCE;
    else if (!strcmp(m,"motion"))  g_app.mode = APP_MODE_MOTION;
    else if (!strcmp(m,"corr"))    g_app.mode = APP_MODE_CORR;
    else if (!strcmp(m,"console")) g_app.mode = APP_MODE_CONSOLE;
    else { kon_printf(ks, "unknown mode\r\n"); return -1; }
    kon_printf(ks, "mode → %s\r\n", argv[1]);
    return 0;
}

static int cmd_los(struct konsole *ks, int argc, char **argv) {
    if (argc < 2) {
        kon_printf(ks, "los: state=%d score=%.1f\r\n",
                   g_app.los_state, (double)g_app.motion_score);
        return 0;
    }
    if (!strcmp(argv[1],"start"))  { los_start(); kon_printf(ks, "LOS started\r\n"); return 0; }
    if (!strcmp(argv[1],"stop"))   { los_stop();  kon_printf(ks, "LOS stopped\r\n"); return 0; }
    if (!strcmp(argv[1],"recal"))  { los_recalibrate(); kon_printf(ks, "LOS recalibrating\r\n"); return 0; }
    kon_printf(ks, "los start|stop|recal\r\n");
    return -1;
}

static int cmd_beep(struct konsole *ks, int argc, char **argv) {
    float hz = 1000.0f;
    int   ms = 300;
    if (argc >= 2) hz = (float)atof(argv[1]);
    if (argc >= 3) ms = atoi(argv[2]);

    const amp_t *amp = sic_amp(0);
    if (!amp || !amp->v) { kon_printf(ks, "no amp\r\n"); return -1; }

    int n = 16 * ms; if (n > 4800) n = 4800;
    static int16_t buf[4800];
    float w = 2.0f * (float)M_PI * hz / 16000.0f;
    for (int i = 0; i < n; i++) buf[i] = (int16_t)(sinf(w * i) * 22000.0f);

    int rc = -1;
    if (amp->v->play_mono) rc = amp->v->play_mono(amp, buf, (size_t)n, 16000);
    else if (amp->v->beep_ms) rc = amp->v->beep_ms(amp, (unsigned)ms);

    kon_printf(ks, "beep %.0f Hz %d ms rc=%d\r\n", (double)hz, ms, rc);
    return rc >= 0 ? 0 : -1;
}

static const struct kon_cmd g_cmds[] = {
    { "sys",  "firmware + chip info",           cmd_sys  },
    { "hw",   "list detected SIC hardware",     cmd_hw   },
    { "csi",  "csi start|stop|info|scan|ch|ap", cmd_csi  },
    { "mode", "mode [los|spec|var|motion|corr]", cmd_mode },
    { "los",  "los start|stop|recal",           cmd_los  },
    { "beep", "beep [hz] [ms]",                 cmd_beep },
};

/* ── Splash screen ───────────────────────────────────────────────────────── */
static void splash(void) {
    if (!s_gfx_ok) return;
    sgfx_clear(&s_gfx, (sgfx_rgba8_t){5,10,18,255});

    sgfx_text5x7_scaled(&s_gfx, 21, 10, "NON MAGICAL", (sgfx_rgba8_t){0,180,255,255}, 3, 3);
    sgfx_text5x7_scaled(&s_gfx, 84, 38, "CSI", (sgfx_rgba8_t){0,220,60,255}, 4, 4);
    sgfx_text5x7       (&s_gfx,  8, 74, "wifi channel state imaging", (sgfx_rgba8_t){110,140,165,255});
    sgfx_text5x7       (&s_gfx,  8, 92, "v" CSI_SENSE_VERSION "  " BOARD_NAME, (sgfx_rgba8_t){70,90,110,255});
    sgfx_text5x7       (&s_gfx,  8,102, "WiFi CSI: fading != radar", (sgfx_rgba8_t){70,90,110,255});

    sgfx_present(&s_gfx);
    delay(1800);
}

/* ── Init & run ──────────────────────────────────────────────────────────── */
void csi_app_init(void) {
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 1500) delay(10);
    delay(50);

    /* SIC: board init */
#if defined(I2C_SDA_PIN) && defined(I2C_SCL_PIN)
    sic_i2c_begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);
#endif
    /* sic_board_default() reads SIC_TARGET_* build flags set in platformio.ini */
    const sic_board_t *board = sic_board_default();
    sic_begin_legacy(board, nullptr);

    /* konsole */
    konsole_init_with_storage(&g_ks, &g_line, &s_io,
                              g_cmds, sizeof g_cmds / sizeof g_cmds[0],
                              "> ", true);
    konsole_set_unknown_handler(&g_ks, app_unknown_cmd);

    /* SGFX display */
    int rc = sgfx_autoinit(&s_gfx, s_gfx_scratch, sizeof s_gfx_scratch);
    if (rc == 0) {
        s_gfx_ok = 1;
        ui_init(&s_gfx);
        kon_printf(&g_ks, "[GFX] ok %dx%d\r\n", SGFX_W, SGFX_H);
        splash();
    } else {
        kon_printf(&g_ks, "[GFX] init failed rc=%d\r\n", rc);
    }

    /* App state — must be zeroed before csi_init writes into it */
    memset(&g_app, 0, sizeof g_app);
    g_app.mode             = APP_MODE_MENU;
    g_app.wifi_channel     = 6;
    g_app.los_active_mode  = true;

    los_init();

    /* Start CSI collection on channel 6 by default */
    csi_init(6);

    kon_printf(&g_ks, "[CSI] started ch6  LLTF HT20  56 subcarriers\r\n");

    kon_banner(&g_ks, "NON MAGICAL CSI ready  (type 'help' or navigate with keyboard)");
}

void csi_app_run(void) {
    /* ── Input & math — runs every iteration, never throttled ────────────── */
    neu_kbd_poll();
    process_nav();
    konsole_poll(&g_ks);

    /* Drain all pending CSI frames immediately — LOS/stats update at CSI rate,
     * not display rate.  s_have_frame is true if at least one frame arrived. */
    static csi_frame_t s_frame_buf;   /* static: safe to hold pointer across calls */
    static bool        s_have_frame = false;
    {
        csi_frame_t frame;
        while (csi_pop_frame(&frame)) {
            los_update(&frame, millis());
            s_frame_buf = frame;
            s_have_frame = true;
        }
    }


    /* ── Display — capped at ~15 fps independently of math rate ─────────── */
    static uint32_t s_last_render = 0;
    uint32_t now = millis();
    if ((now - s_last_render) >= 66u) {
        s_last_render = now;
        if (s_gfx_ok) {
            if (g_app.mode == APP_MODE_MENU)
                ui_draw_menu(s_menu_sel);
            else if (g_app.mode == APP_MODE_CONSOLE)
                ui_draw_console(&s_scr[0][0], SCR_COLS+1, SCR_ROWS,
                                SCR_COLS, s_scr_row, s_scr_col);
            else
                ui_render(s_have_frame ? &s_frame_buf : nullptr, nullptr);
            ui_present();
        }
        s_have_frame = false;   /* consumed by renderer */
    }

    /* Deferred AP scan — runs AFTER display already showed "SCANNING..." */
    if (s_scan_pending) {
        s_scan_pending = false;
        /* Force one rendered frame so "SCANNING..." is visible before we block */
        if (s_gfx_ok) {
            ui_render(nullptr, nullptr);
            ui_present();
        }
        csi_scan_aps();
        g_app.los_is_scanning = false;
    }

    /* Short yield — lets WiFi/I2S tasks on core 0 run between our iterations */
    delay(5);
}
