#pragma once
#include "app.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-subcarrier running statistics (Welford online algorithm) */
typedef struct {
    float    mean[CSI_N_SUB];
    float    var[CSI_N_SUB];        /* sample variance, n>1 */
    uint32_t frame_count;
    uint32_t fps;
} csi_stats_t;

/* Lifecycle */
int  csi_init(int channel);        /* init WiFi + CSI, returns 0 on success */
void csi_deinit(void);

/* Channel / target */
void csi_set_channel(int channel);
void csi_select_ap(int scan_idx);  /* monitor one AP from last scan results */
void csi_select_any(int channel);  /* monitor everything on channel         */

/* Frame consumer — call from main loop */
int  csi_pop_frame(csi_frame_t *out); /* 1 = got frame, 0 = queue empty    */
void csi_get_stats(csi_stats_t *out);

/* AP scan (briefly pauses CSI) */
int         csi_scan_aps(void);
int         csi_ap_count(void);
const char* csi_ap_ssid(int i);
int8_t      csi_ap_rssi(int i);
int         csi_ap_channel(int i);
const uint8_t* csi_ap_bssid(int i);

/* Active injection mode (probe-request → probe-response → CSI).
 * csi_select_ap() locks the target but does NOT start injection.
 * Call csi_active_start() explicitly after csi_select_ap() when active mode
 * is desired.  csi_active_stop() is called automatically by csi_select_any()
 * and csi_deinit(). */
bool     csi_active_running(void);
uint32_t csi_active_tx_ok(void);
uint32_t csi_active_tx_err(void);
void csi_active_start(void);   /* begin probe injection to current s_target   */
void csi_active_stop(void);    /* stop injection; safe to call when not running */

#ifdef __cplusplus
}
#endif
