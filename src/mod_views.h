#pragma once
#include "app.h"
#include "sgfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once after display init */
void ui_init(sgfx_device_t *dev);

/* Flush dirty framebuffer tiles to the display — call once per frame */
void ui_present(void);

/* Call each frame from main loop — dispatches to active mode renderer.
 * latest:    last received CSI frame (may be stale — never nullptr after first frame)
 * amp_hist:  unused, reserved
 * new_frame: true only when a new CSI frame arrived in this render cycle */
void ui_render(const csi_frame_t *latest, const float *amp_hist, bool new_frame);
/* amp_hist: float[CSI_HIST_LEN][CSI_N_SUB] — rolling history for waterfall  */

/* Individual viewers (also usable standalone) */
void ui_draw_menu(int sel);
void ui_draw_los(void);
void ui_draw_spectrum(const uint8_t amp[CSI_N_SUB],
                      const uint8_t waterfall[CSI_HIST_LEN][CSI_N_SUB],
                      int wf_row);
void ui_draw_variance(const float var[CSI_N_SUB], const float mean[CSI_N_SUB]);
void ui_draw_motion(float score, const float hist[CSI_HIST_LEN]);
void ui_draw_corr(const uint8_t amp[CSI_N_SUB]);
void ui_draw_console(const char *scr, int stride, int rows, int cols,
                     int cur_row, int cur_col);
void ui_draw_training(void);

/* Shared helpers */
void ui_statusbar(const char *mode_tag);
void ui_footbar(const char *hints);

#ifdef __cplusplus
}
#endif
