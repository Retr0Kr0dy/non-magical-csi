#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct amp_vtbl_s {
    void (*enable)(const void* self, int on);  /* 1 = unmute DAC + power amp, 0 = mute */

    /* Optional high-level playback hooks.  Drivers that only expose a mute or
     * power GPIO may leave these NULL; direct-I2S amp drivers should implement
     * them so examples/apps never need board-specific speaker pins.
     */
    int  (*play_mono)(const void* self, const int16_t* mono, size_t nframes, int sample_rate_hz);
    int  (*beep_ms)(const void* self, unsigned ms);
};

typedef struct amp_s {
    const struct amp_vtbl_s* v;
    void* impl;
} amp_t;

const amp_t* sic_amp(int index);

#ifdef __cplusplus
}
#endif
