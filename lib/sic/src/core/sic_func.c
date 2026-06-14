
#include <string.h>
#include "sic/sic.h"

const char* sic_func_name(sic_func_id_t id){
  switch(id){
    case SIC_F_KSCAN: return "kscan";
    case SIC_F_MIC: return "mic";
    case SIC_F_AMP: return "audio_amp";
    case SIC_F_PWR_SW: return "pwr_sw";
    case SIC_F_CHARGER: return "charger";
    case SIC_F_IR_TX: return "ir_tx";
    case SIC_F_SD: return "sd";
    case SIC_F_ENCODER: return "encoder";
    default: return "?";
  }
}

sic_func_id_t sic_func_id_from_name(const char* s){
  if (!s) return SIC_F__COUNT;
  if (0==strcmp(s,"kscan")) return SIC_F_KSCAN;
  if (0==strcmp(s,"mic")) return SIC_F_MIC;
  if (0==strcmp(s,"audio_amp")) return SIC_F_AMP;
  if (0==strcmp(s,"pwr_sw")) return SIC_F_PWR_SW;
  if (0==strcmp(s,"charger")) return SIC_F_CHARGER;
  if (0==strcmp(s,"ir_tx")) return SIC_F_IR_TX;
  if (0==strcmp(s,"sd")) return SIC_F_SD;
  if (0==strcmp(s,"encoder")) return SIC_F_ENCODER;
  return SIC_F__COUNT;
}
