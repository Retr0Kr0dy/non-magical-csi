#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int  sic_i2c_begin_bus(int bus, int sda, int scl, uint32_t hz);
int  sic_i2c_scan_bus(int bus, uint8_t* addrs, int max);
int  sic_i2c_write(int bus, uint8_t addr, const uint8_t* buf, int n);
int  sic_i2c_read (int bus, uint8_t addr, uint8_t* buf, int n);
int  sic_i2c_writeread(int bus, uint8_t addr, const uint8_t* wr, int nw, uint8_t* rd, int nr);
#ifdef __cplusplus
}
#endif
