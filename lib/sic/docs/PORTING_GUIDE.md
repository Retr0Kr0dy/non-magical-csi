# Porting Guide (SIC v2)

1. **Create a board descriptor** in `lib/SIC/src/boards/board_<your>.c`.
2. **Wire buses only via HAL** (`sic_gpio_*`, `sic_i2c_*`, `sic_delay_*`).
3. **Add drivers** under `lib/SIC/src/drivers/<domain>/`.
4. **Register instances** in your board `probe` or a board-specific init.
5. Keep `src/main.cpp` using only `#include "sic/sic.h"`.

See `lib/SIC/src/drivers/dummy.c` for a minimal reference driver.
