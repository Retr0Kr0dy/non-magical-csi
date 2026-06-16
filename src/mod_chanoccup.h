#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void  chanoccup_init(void);
void  chanoccup_start(void);
void  chanoccup_stop(void);
void  chanoccup_tick(uint32_t now_ms);
void  chanoccup_on_frame(uint32_t now_ms);

int   chanoccup_current_ch(void);   /* 1-13, channel currently being sampled */
float chanoccup_fps(int ch);        /* last measured fps for channel ch (1-13) */
float chanoccup_peak(int ch);       /* highest fps ever seen on channel ch */
float chanoccup_last_nz(int ch);    /* last non-zero fps (kept when current sweep is 0) */
bool  chanoccup_settling(void);     /* true during post-hop settle window */

#ifdef __cplusplus
}
#endif
