#pragma once
#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

void        los_init(void);
void        los_start(void);                    /* begin countdown sequence       */
void        los_stop(void);
void        los_update(const csi_frame_t *f, uint32_t now_ms);
void        los_recalibrate(void);             /* restart calibration in-place   */

float       los_get_score(void);               /* 0..100 EMA-smoothed            */
los_state_t los_get_state(void);
int         los_get_countdown(void);           /* seconds left in countdown      */
int         los_get_cal_progress(void);        /* 0..100                         */

/* Baseline snapshot (for variance view overlay) */
void los_get_baseline(float *mean_out, float *sigma_out); /* arrays[CSI_N_SUB] */

#ifdef __cplusplus
}
#endif
