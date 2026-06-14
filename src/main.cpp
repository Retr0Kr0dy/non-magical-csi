#include <Arduino.h>

extern "C++" {
    void csi_app_init(void);
    void csi_app_run(void);
}

void setup() { csi_app_init(); }
void loop()  { csi_app_run(); }
