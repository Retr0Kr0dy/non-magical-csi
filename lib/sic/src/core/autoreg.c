/*
 * autoreg.c — Conditional registration of built-in SIC drivers.
 *
 * By default every driver whose source file is compiled gets registered.
 * Use -DSIC_NO_DRV_<NAME> to exclude a driver even if its source compiles.
 * Use -DSIC_DRV_ONLY together with -DSIC_DRV_<NAME> for explicit opt-in
 * (advanced: disables all defaults).
 *
 * To add a new driver: add an #ifdef block below and call its register fn.
 */

#include "sic/sic_registry.h"

/* ── Opt-out / opt-in resolution ─────────────────────────────────────── */

#if !defined(SIC_DRV_ONLY)
    /* Default: enable all known drivers unless explicitly suppressed. */
#   if !defined(SIC_NO_DRV_KBD_74HC138)
#       define SIC__AUTOREG_KBD_74HC138
#   endif
#   if !defined(SIC_NO_DRV_TP4057)
#       define SIC__AUTOREG_TP4057
#   endif
#   if defined(SIC_DRV_BQ25896) || defined(SIC_TARGET_TPAGER)
#       define SIC__AUTOREG_BQ25896
#   endif
    /* TCA8418 is auto-enabled for boards that declare it, otherwise opt-in. */
#   if defined(SIC_DRV_KBD_TCA8418) || defined(SIC_TARGET_TPAGER) || defined(SIC_TARGET_CARDPUTER_ADV)
#       define SIC__AUTOREG_KBD_TCA8418
#   endif
#   if defined(SIC_DRV_ENCODER_GPIO)
#       define SIC__AUTOREG_ENCODER_GPIO
#   endif
#   if defined(SIC_DRV_CODEC_ES8311)
#       define SIC__AUTOREG_CODEC_ES8311
#   endif
#   if defined(SIC_DRV_MIC_PDM) || defined(SIC_TARGET_CARDPUTER)
#       define SIC__AUTOREG_MIC_PDM
#   endif
#   if defined(SIC_DRV_AMP_I2S) || defined(SIC_TARGET_CARDPUTER)
#       define SIC__AUTOREG_AMP_I2S
#   endif
#   if defined(SIC_DRV_IR_GPIO) || defined(SIC_TARGET_CARDPUTER)
#       define SIC__AUTOREG_IR_GPIO
#   endif
#   if defined(SIC_DRV_SD_SPI) || defined(SIC_TARGET_CARDPUTER) || defined(SIC_TARGET_TPAGER)
#       define SIC__AUTOREG_SD_SPI
#   endif
#else
    /* Opt-in mode: only register what is explicitly requested. */
#   if defined(SIC_DRV_KBD_74HC138)
#       define SIC__AUTOREG_KBD_74HC138
#   endif
#   if defined(SIC_DRV_TP4057)
#       define SIC__AUTOREG_TP4057
#   endif
#   if defined(SIC_DRV_BQ25896)
#       define SIC__AUTOREG_BQ25896
#   endif
#   if defined(SIC_DRV_KBD_TCA8418)
#       define SIC__AUTOREG_KBD_TCA8418
#   endif
#   if defined(SIC_DRV_ENCODER_GPIO)
#       define SIC__AUTOREG_ENCODER_GPIO
#   endif
#   if defined(SIC_DRV_CODEC_ES8311)
#       define SIC__AUTOREG_CODEC_ES8311
#   endif
#   if defined(SIC_DRV_MIC_PDM)
#       define SIC__AUTOREG_MIC_PDM
#   endif
#   if defined(SIC_DRV_AMP_I2S)
#       define SIC__AUTOREG_AMP_I2S
#   endif
#   if defined(SIC_DRV_IR_GPIO)
#       define SIC__AUTOREG_IR_GPIO
#   endif
#   if defined(SIC_DRV_SD_SPI)
#       define SIC__AUTOREG_SD_SPI
#   endif
#endif

/* ── Forward declarations ─────────────────────────────────────────────── */

#ifdef SIC__AUTOREG_KBD_74HC138
extern void sic_register_driver_kbd_74hc138(void);
#endif

#ifdef SIC__AUTOREG_TP4057
extern void sic_register_driver_tp4057(void);
#endif
#ifdef SIC__AUTOREG_BQ25896
extern void sic_register_driver_bq25896(void);
#endif

#ifdef SIC__AUTOREG_KBD_TCA8418
extern void sic_register_driver_kbd_tca8418(void);
#endif

#ifdef SIC__AUTOREG_ENCODER_GPIO
extern void sic_register_driver_encoder_gpio(void);
#endif

#ifdef SIC__AUTOREG_CODEC_ES8311
extern void sic_register_driver_codec_es8311(void);
#endif

#ifdef SIC__AUTOREG_MIC_PDM
extern void sic_register_driver_mic_pdm(void);
#endif

#ifdef SIC__AUTOREG_AMP_I2S
extern void sic_register_driver_amp_i2s(void);
#endif

#ifdef SIC__AUTOREG_IR_GPIO
extern void sic_register_driver_ir_gpio(void);
#endif

#ifdef SIC__AUTOREG_SD_SPI
extern void sic_register_driver_sd_spi(void);
#endif

/* dummy is always registered — it is the safe no-op fallback */
extern void sic_register_driver_dummy(void);

/* ── Entry point called by sic_begin_legacy ───────────────────────────── */

void sic_autoreg_drivers(void) {
#ifdef SIC__AUTOREG_KBD_74HC138
    sic_register_driver_kbd_74hc138();
#endif
#ifdef SIC__AUTOREG_TP4057
    sic_register_driver_tp4057();
#endif
#ifdef SIC__AUTOREG_BQ25896
    sic_register_driver_bq25896();
#endif
#ifdef SIC__AUTOREG_KBD_TCA8418
    sic_register_driver_kbd_tca8418();
#endif
    sic_register_driver_dummy();
#ifdef SIC__AUTOREG_ENCODER_GPIO
    sic_register_driver_encoder_gpio();
#endif
#ifdef SIC__AUTOREG_CODEC_ES8311
    sic_register_driver_codec_es8311();
#endif
#ifdef SIC__AUTOREG_MIC_PDM
    sic_register_driver_mic_pdm();
#endif
#ifdef SIC__AUTOREG_AMP_I2S
    sic_register_driver_amp_i2s();
#endif
#ifdef SIC__AUTOREG_IR_GPIO
    sic_register_driver_ir_gpio();
#endif
#ifdef SIC__AUTOREG_SD_SPI
    sic_register_driver_sd_spi();
#endif
}
