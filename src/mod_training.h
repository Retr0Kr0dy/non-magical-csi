#pragma once
#include "app.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRAIN_PROC_INDOOR = 0,  /* Indoor presence: leave/enter room along door-device axis */
    TRAIN_PROC_OUTDOOR,     /* Outdoor distance: walk away and return                   */
    TRAIN_PROC_ZONE,        /* Zone sweep: stand at 4 labeled positions                 */
    TRAIN_PROC__COUNT
} train_proc_t;

typedef enum {
    TRAIN_UI_PROC_SEL = 0, /* choose procedure                */
    TRAIN_UI_NAME,         /* type session name               */
    TRAIN_UI_HELP,         /* procedure description           */
    TRAIN_UI_RUNNING,      /* session in progress             */
    TRAIN_UI_DONE,         /* session complete                */
} train_ui_t;

void training_init(void);
void training_key(char c);
void training_on_frame(const csi_frame_t *f, uint32_t now_ms);
void training_tick(uint32_t now_ms);
void training_stop(void);

/* View read-only accessors */
train_ui_t   training_ui(void);
train_proc_t training_proc(void);
int          training_step(void);
int          training_step_total(void);
bool         training_in_cap(void);   /* true = recording; false = buffer/transition */
int          training_remain_s(void); /* seconds left in current phase */
int          training_frames_written(void);
const char*  training_step_label(void);
const char*  training_step_instr1(void);
const char*  training_step_instr2(void);
const char*  training_next_instr1(void);  /* next step's instr1, "" if last */
int          training_phase_dur_s(void);  /* total seconds of current phase */
const char*  training_session_name(void);
const char*  training_proc_name(train_proc_t p);
const char*  training_proc_help(train_proc_t p);
const char*  training_filename(void);
int          training_proc_cursor(void);

#ifdef __cplusplus
}
#endif
