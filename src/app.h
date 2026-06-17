#pragma once
#include <stdint.h>
#include <stdbool.h>

/* -- Display geometry -- */
#ifndef SGFX_W
#define SGFX_W 240
#endif
#ifndef SGFX_H
#define SGFX_H 135
#endif

#define UI_BAR_H    12   /* top status bar height (px) */
#define UI_FOOT_H   10   /* bottom hint bar height (px) */
#define UI_MAIN_Y   UI_BAR_H
#define UI_MAIN_H   (SGFX_H - UI_BAR_H - UI_FOOT_H)
#define UI_MAIN_YEND (SGFX_H - UI_FOOT_H)

/* -- CSI parameters -- */
/* LLTF HT20: 64 complex pairs (Im,Re int8), valid indices [4..31],[33..60].
 * Excludes DC at 32 and guard bands at 0-3,61-63.  56 valid subcarriers.    */
#define CSI_N_SUB       56
#define CSI_RING_POW    6               /* 2^6 = 64 ring frames              */
#define CSI_RING_SIZE   (1 << CSI_RING_POW)
#define CSI_RING_MASK   (CSI_RING_SIZE - 1)
#define CSI_HIST_LEN    90              /* waterfall / motion history rows    */
#define CSI_CAL_FRAMES         80      /* passive: ~40-80 s at 1-2 fps       */
#define CSI_CAL_FRAMES_ACTIVE  100     /* active:  ~10 s at 10 fps           */

typedef struct {
    uint32_t ts_ms;
    uint8_t  amp[CSI_N_SUB];           /* |sqrt(Im^2+Re^2)| approx, 0-255   */
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  fwi;                      /* first_word_invalid flag            */
} csi_frame_t;

/* -- Application modes -- */
typedef enum {
    APP_MODE_MENU = 0,
    APP_MODE_LOS,       /* Line-of-Sight disturbance sensing (calibrated)    */
    APP_MODE_SPECTRUM,  /* Real-time amplitude waterfall - all subcarriers    */
    APP_MODE_VARIANCE,  /* Per-subcarrier variance bars - which subs move     */
    APP_MODE_MOTION,    /* Scalar motion score + sparkline history            */
    APP_MODE_CORR,      /* Cross-subcarrier correlation (coherent vs noise)   */
    APP_MODE_CHANOCCUP, /* Passive per-channel frame-rate survey               */
    APP_MODE_CONSOLE,   /* On-screen serial terminal mirror                   */
    APP_MODE_TRAINING,  /* Guided ML data-collection session                  */
    APP_MODE_FILEMAN,   /* SD card file browser + delete                      */
    APP_MODE__COUNT
} app_mode_t;

/* -- LOS sub-states -- */
typedef enum {
    LOS_IDLE = 0,
    LOS_SELECTING,      /* choosing AP or channel                            */
    LOS_COUNTDOWN,      /* 5-second countdown: step away                     */
    LOS_CALIBRATING,    /* collecting baseline frames                        */
    LOS_SCANNING,       /* active sensing, audio alerts                      */
} los_state_t;


/* -- Shared application state (read by all modules) -- */
typedef struct {
    app_mode_t  mode;
    int         wifi_channel;          /* current monitored channel          */
    char        ap_ssid[33];           /* monitored AP SSID ("" = any)      */
    uint8_t     ap_bssid[6];
    bool        csi_running;
    uint32_t    csi_fps;               /* CSI frames per second              */
    uint32_t    csi_total;             /* total frames received              */

    float       motion_score;          /* 0..100 EMA-smoothed score          */
    los_state_t los_state;
    int         los_countdown;         /* seconds remaining                  */
    int         los_cal_progress;      /* 0..100 calibration progress        */
    int         los_ap_sel;            /* AP cursor index during SELECTING   */
    bool        los_is_scanning;      /* scan in flight - show animation    */
    bool        active_mode;          /* true = inject probes; false = passive sniff */
} app_state_t;

extern app_state_t g_app;
