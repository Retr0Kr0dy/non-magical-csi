# Feature Matrix

| Area | Status | Driver / Files |
|------|--------|----------------|
| I2C bus | ✅ | `src/bus/i2c_bus_arduino.cpp` |
| GPIO bus / delay | ✅ | `src/bus/gpio_bus_arduino.c`, `src/bus/delay_arduino.c` |
| Keyboard — TCA8418 (I2C matrix, T-Pager/Cardputer-ADV) | ✅ | `src/drivers/input/kbd_tca8418_i2c.c` |
| Keyboard — 74HC138 (GPIO matrix, Cardputer/v1.1) | ✅ | `src/drivers/input/kbd_74hc138_gpio.c` |
| Keyboard event abstraction | ✅ | `src/hal/hal_kbd.c`, `include/sic/input/kscan.h` |
| Cardputer logical keymap + Fn layer | ✅ | `src/drivers/input/cardputer_keymap.c` |
| Rotary encoder — GPIO | ✅ | `src/drivers/input/encoder_gpio.c` |
| Audio codec — ES8311 (I2S mic + amp) | ✅ | `src/drivers/audio/codec_es8311.c` |
| PDM mic — SPM1423/Cardputer | ✅ | `src/drivers/audio/mic_pdm.c` |
| I2S speaker amp — NS4168/Cardputer | ✅ | `src/drivers/audio/amp_i2s.c` |
| Audio I2S backend | ✅ | `src/backends/arduino/sic_arduino_audio.cpp` |
| Battery — BQ27220 (I2C fuel gauge) | ✅ | `src/drivers/power/bq27220.c` |
| Battery — ADC (simple voltage divider) | ✅ | `src/power/battery_adc.cpp` |
| Charger — TP4057 | ⚠️ minimal | `src/drivers/power/tp4057.c` |
| Charger — BQ25896 / T-Pager | ✅ status | `src/drivers/power/bq25896.c` |
| IR TX — NEC bitbang/Cardputer | ✅ | `src/drivers/ir/ir_tx_gpio.cpp` |
| SD storage — SPI/Cardputer/T-Pager | ✅ basic mount/info | `src/drivers/storage/sd_spi_arduino.cpp` |
| Driver autoregistration | ✅ | `src/core/autoreg.c` |
| Driver registry + typed accessors | ✅ | `src/core/registry.c` |
