#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { float voltage_v; int percent; } sic_battery_t;
int sic_battery_read(sic_battery_t* out);
#ifdef __cplusplus
}
#endif
