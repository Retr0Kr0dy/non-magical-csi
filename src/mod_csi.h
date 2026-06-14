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

#ifdef __cplusplus
}
#endif
