/*
 * mod_training.cpp - Guided ML data-collection sessions.
 *
 * Flow (guided: indoor / outdoor / zone):
 *   PROC_SEL -> NAME -> RUNNING (steps) -> DONE
 *
 * Flow (free record):
 *   PROC_SEL -> NAME -> FREE_LABEL (label selector)
 *           -> FREE_CFG (cooldown + duration) -> RUNNING -> FREE_DONE
 *   FREE_DONE: R=repeat same label, N=new label (back to FREE_LABEL), ESC=finish
 *
 * The free label selector draws its list from the last non-free procedure the
 * user had highlighted in PROC_SEL (s_free_src_proc, defaults to indoor).
 * This lets the user navigate to "indoor", inspect labels, then select "free"
 * and get indoor labels without any extra screen.
 *
 * Each guided step has two phases:
 *   BUF  : transition time (person moves to position) - frames discarded
 *          starts with 3 descending beeps (1400->900->600 Hz)
 *   CAP  : capture time (frames written to SD with label)
 *          starts with 3 ascending beeps (600->900->1400 Hz)
 *
 * Frames with label == nullptr are never written (door events, transitions).
 *
 * Output files:
 *   Guided: /non-magical-csi/<board>/<name>_<proc>.csv
 *   Free:   /non-magical-csi/<board>/<name>_free_<src_proc>.csv  (appended across captures)
 *   columns: ts_ms,label,rssi,ch,a00..a55
 */

#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <stdio.h>
#include "mod_training.h"
#include "mod_los.h"

extern "C" {
#include "sic/storage/sd.h"
}

/* -- Step definition -- */
typedef struct {
    const char *label;    /* CSV label; nullptr = discard (never written) */
    const char *instr1;   /* screen line 1 */
    const char *instr2;   /* screen line 2 */
    uint16_t    buf_s;    /* transition buffer seconds (frames discarded)  */
    uint16_t    cap_s;    /* capture seconds (frames written)              */
} train_step_t;

/* -- Procedures (guided) -- */

/* INDOOR: walk door->center->near device->center->door.  Total ~100 s. */
static const train_step_t kStepsIndoor[] = {
    { "absent",     "LEAVE THE ROOM",    "CLOSE THE DOOR",    15, 10 },
    { "approach",   "OPEN DOOR",         "WALK TO CENTER",    10,  6 },
    { "still_mid",  "STOP AT CENTER",    "HOLD POSITION",      4,  6 },
    { "approach",   "WALK TO DEVICE",    "",                   0,  5 },
    { "still_near", "STOP NEAR DEVICE",  "HOLD POSITION",      4,  6 },
    { "recede",     "WALK TO CENTER",    "",                   0,  5 },
    { "still_mid",  "STOP AT CENTER",    "HOLD POSITION",      4,  5 },
    { "recede",     "WALK TO DOOR",      "LEAVE THE ROOM",     0,  8 },
    { "absent",     "CLOSE DOOR",        "STAND OUTSIDE",      8,  8 },
};

/* OUTDOOR: linear distance sweep.  Total ~50 s. */
static const train_step_t kStepsOutdoor[] = {
    { "still_near", "STAND NEAR DEVICE", "HOLD POSITION",      5,  6 },
    { "recede",     "WALK AWAY SLOWLY",  "TARGET: ~10m",       0, 10 },
    { "still_far",  "STOP",              "HOLD FAR POSITION",  4,  8 },
    { "approach",   "WALK BACK SLOWLY",  "RETURN TO DEVICE",   0, 10 },
    { "still_near", "STOP NEAR DEVICE",  "HOLD POSITION",      3,  6 },
};

/* ZONE: distance sweep along device axis.  Total ~80 s.
 * Covers the full shared label set so free-mode sessions add directly. */
static const train_step_t kStepsZone[] = {
    { "still_near", "STAND NEAR DEVICE",  "HOLD POSITION",      5,  8 },
    { "recede",     "STEP AWAY SLOWLY",   "MOVE TO CENTER",     0,  6 },
    { "still_mid",  "STOP AT CENTER",     "HOLD POSITION",      3,  8 },
    { "recede",     "CONTINUE BACK",      "MOVE TO FAR SPOT",   0,  6 },
    { "still_far",  "STOP AT FAR SPOT",   "HOLD POSITION",      3,  8 },
    { "approach",   "WALK BACK",          "RETURN TO DEVICE",   0,  6 },
    { "still_near", "STOP NEAR DEVICE",   "HOLD POSITION",      3,  6 },
    { "absent",     "LEAVE THE AREA",     "HOLD OUTSIDE",       6,  8 },
};

typedef struct {
    const char           *name;
    const char           *help;
    const train_step_t   *steps;
    int                   n_steps;
} proc_def_t;

static const proc_def_t kProcs[TRAIN_PROC__COUNT] = {
    {
        "indoor",
        "Walk the door-to-device axis.\n"
        "Positions: outside, center, near device.\n"
        "Labels: absent / approach / still_mid\n"
        "        / still_near / recede\n"
        "Duration: ~100 s",
        kStepsIndoor,
        (int)(sizeof kStepsIndoor / sizeof kStepsIndoor[0])
    },
    {
        "outdoor",
        "Linear distance sweep outdoors.\n"
        "Walk ~10 m away then return.\n"
        "Labels: still_near / recede\n"
        "        / still_far / approach\n"
        "Duration: ~50 s",
        kStepsOutdoor,
        (int)(sizeof kStepsOutdoor / sizeof kStepsOutdoor[0])
    },
    {
        "zone",
        "Distance sweep: near -> mid -> far -> return.\n"
        "No floor markers needed.\n"
        "Labels: still_near / still_mid / still_far\n"
        "        / approach / recede / absent\n"
        "Duration: ~80 s",
        kStepsZone,
        (int)(sizeof kStepsZone / sizeof kStepsZone[0])
    },
    {
        "free",
        "Record one label at a time.\n"
        "Labels are drawn from whichever\n"
        "procedure was highlighted last.\n"
        "R=repeat  N=new label  ESC=finish.\n"
        "All captures append to one file.",
        nullptr,
        0
    },
};

/* -- State -- */
static train_ui_t   s_ui         = TRAIN_UI_PROC_SEL;
static train_proc_t s_proc       = TRAIN_PROC_INDOOR;
static int          s_proc_cursor = 0;
static char         s_name[32]   = {};
static int          s_name_len   = 0;

static int          s_step       = 0;
static bool         s_in_cap     = false;
static uint32_t     s_deadline   = 0;
static int          s_frames_written = 0;

static char         s_filename[96] = {};
static File         s_file;
static bool         s_sd_ok      = false;

/* -- Free mode state -- */
/* Tracks the last non-free procedure the cursor rested on.
 * When free is selected, this is the label source. */
static train_proc_t s_free_src_proc   = TRAIN_PROC_INDOOR;
static const char  *s_free_labels[10];
static int          s_free_label_count  = 0;
static int          s_free_label_cursor = 0;
static char         s_free_label[32]    = {};
static int          s_free_cooldown     = 3;
static int          s_free_cap_s        = 10;
static int          s_free_cfg_field    = 0;   /* 0=cooldown, 1=duration */
static train_step_t s_free_step_def;

/* Build the unique (non-null) label list for s_free_src_proc. */
static void build_free_label_list(void) {
    const proc_def_t *p = &kProcs[s_free_src_proc];
    s_free_label_count  = 0;
    s_free_label_cursor = 0;
    for (int i = 0; i < p->n_steps; i++) {
        const char *lbl = p->steps[i].label;
        if (!lbl) continue;
        bool dup = false;
        for (int j = 0; j < s_free_label_count; j++)
            if (strcmp(s_free_labels[j], lbl) == 0) { dup = true; break; }
        if (!dup && s_free_label_count < 10)
            s_free_labels[s_free_label_count++] = lbl;
    }
}

/* -- Audio helpers -- */
static void beep_descending(void) {
    los_beep_async(1400, 60, 40);
    los_beep_async(900,  60, 40);
    los_beep_async(600,  80,  0);
}

static void beep_ascending(void) {
    los_beep_async(600,  60, 40);
    los_beep_async(900,  60, 40);
    los_beep_async(1400, 80,  0);
}

static void beep_done(void) {
    los_beep_async(800,  60, 30);
    los_beep_async(1200, 60, 30);
    los_beep_async(1600, 100,  0);
}

/* -- SD helpers -- */
static void sd_open_guided(void) {
    const sd_t *sd = sic_sd(0);
    if (!sd) return;
    if (sd->v->begin(sd) != 0) { LOG_E("train: SD begin failed"); return; }
    if (!sd->v->present(sd)) { LOG_E("train: no SD card"); return; }
    if (!SD.exists("/non-magical-csi")) SD.mkdir("/non-magical-csi");
    if (!SD.exists("/non-magical-csi/" BOARD_NAME)) SD.mkdir("/non-magical-csi/" BOARD_NAME);

    snprintf(s_filename, sizeof s_filename,
             "/non-magical-csi/" BOARD_NAME "/%s_%s.csv", s_name, kProcs[s_proc].name);
    s_file = SD.open(s_filename, FILE_WRITE);
    if (!s_file) { LOG_E("train: open failed: %s", s_filename); return; }

    s_file.print("ts_ms,label,rssi,ch");
    for (int i = 0; i < CSI_N_SUB; i++) {
        char col[8]; snprintf(col, sizeof col, ",a%02d", i); s_file.print(col);
    }
    s_file.println();
    s_sd_ok = true;
    LOG_I("train: logging -> %s", s_filename);
}

static void sd_open_free(void) {
    const sd_t *sd = sic_sd(0);
    if (!sd) return;
    if (sd->v->begin(sd) != 0) { LOG_E("train: SD begin failed"); return; }
    if (!sd->v->present(sd)) { LOG_E("train: no SD card"); return; }
    if (!SD.exists("/non-magical-csi")) SD.mkdir("/non-magical-csi");
    if (!SD.exists("/non-magical-csi/" BOARD_NAME)) SD.mkdir("/non-magical-csi/" BOARD_NAME);

    /* Filename encodes both session name and label source so files stay separate. */
    snprintf(s_filename, sizeof s_filename,
             "/non-magical-csi/" BOARD_NAME "/%s_free_%s.csv", s_name, kProcs[s_free_src_proc].name);
    bool write_header = !SD.exists(s_filename);
    s_file = SD.open(s_filename, FILE_APPEND);
    if (!s_file) { LOG_E("train: open failed: %s", s_filename); return; }

    if (write_header) {
        s_file.print("ts_ms,label,rssi,ch");
        for (int i = 0; i < CSI_N_SUB; i++) {
            char col[8]; snprintf(col, sizeof col, ",a%02d", i); s_file.print(col);
        }
        s_file.println();
    }
    s_sd_ok = true;
    LOG_I("train: logging -> %s", s_filename);
}

static void sd_write(const csi_frame_t *f, const char *label) {
    if (!s_sd_ok || !s_file) return;
    s_file.printf("%lu,%s,%d,%d",
                  (unsigned long)f->ts_ms, label, (int)f->rssi, (int)f->channel);
    for (int i = 0; i < CSI_N_SUB; i++)
        s_file.printf(",%d", (int)f->amp[i]);
    s_file.println();
}

static void sd_close(void) {
    if (s_file) { s_file.flush(); s_file.close(); }
    s_sd_ok = false;
}

/* -- Step runner -- */

static bool is_free_running(void) { return (s_proc == TRAIN_PROC_FREE); }

static int steps_total(void) {
    return is_free_running() ? 1 : kProcs[s_proc].n_steps;
}

static const train_step_t* cur_step(void) {
    if (is_free_running())
        return (s_step == 0) ? &s_free_step_def : nullptr;
    const proc_def_t *p = &kProcs[s_proc];
    return (s_step < p->n_steps) ? &p->steps[s_step] : nullptr;
}

static void enter_cap(uint32_t now_ms) {
    s_in_cap   = true;
    const train_step_t *st = cur_step();
    s_deadline = now_ms + (uint32_t)(st ? st->cap_s : 0) * 1000u;
    beep_ascending();
    if (st) LOG_I("train: CAP label='%s' dur=%ds",
                  st->label ? st->label : "(discard)", st->cap_s);
}

static void enter_buf(uint32_t now_ms) {
    s_in_cap   = false;
    const train_step_t *st = cur_step();
    if (!st) return;
    beep_descending();
    if (st->buf_s == 0) {
        enter_cap(now_ms);
    } else {
        s_deadline = now_ms + (uint32_t)st->buf_s * 1000u;
        LOG_I("train: BUF step=%d instr='%s' dur=%ds",
              s_step, st->instr1, st->buf_s);
    }
}

static void start_guided(uint32_t now_ms) {
    s_step           = 0;
    s_in_cap         = false;
    s_frames_written = 0;
    s_ui             = TRAIN_UI_RUNNING;
    sd_open_guided();
    enter_buf(now_ms);
}

static void start_free_capture(uint32_t now_ms) {
    if (!s_sd_ok) sd_open_free();   /* open once; stays open across captures */
    s_free_step_def.label  = s_free_label;
    s_free_step_def.instr1 = s_free_label;
    s_free_step_def.instr2 = "";
    s_free_step_def.buf_s  = (uint16_t)s_free_cooldown;
    s_free_step_def.cap_s  = (uint16_t)s_free_cap_s;
    s_step   = 0;
    s_in_cap = false;
    /* s_frames_written accumulates across all captures in this free session */
    s_ui = TRAIN_UI_RUNNING;
    enter_buf(now_ms);
}

static void advance_step(uint32_t now_ms) {
    s_step++;
    if (s_step >= steps_total()) {
        beep_done();
        LOG_I("train: capture done - %d frames written", s_frames_written);
        if (is_free_running()) {
            s_ui = TRAIN_UI_FREE_DONE;   /* SD stays open for more captures */
        } else {
            sd_close();
            s_ui = TRAIN_UI_DONE;
        }
    } else {
        enter_buf(now_ms);
    }
}

/* -- Public API -- */
void training_init(void) {
    s_ui          = TRAIN_UI_PROC_SEL;
    s_proc        = TRAIN_PROC_INDOOR;
    s_proc_cursor = 0;
    s_name_len    = 0;
    memset(s_name, 0, sizeof s_name);
    s_step           = 0;
    s_in_cap         = false;
    s_frames_written = 0;
    s_free_src_proc     = TRAIN_PROC_INDOOR;
    s_free_label_count  = 0;
    s_free_label_cursor = 0;
    memset(s_free_label, 0, sizeof s_free_label);
    s_free_cooldown  = 3;
    s_free_cap_s     = 10;
    s_free_cfg_field = 0;
    sd_close();
}

void training_stop(void) {
    sd_close();
    s_ui = TRAIN_UI_PROC_SEL;
}

void training_key(char c) {
    switch (s_ui) {

    case TRAIN_UI_PROC_SEL:
        if (c == 'w' || c == 'W') {
            if (s_proc_cursor > 0) s_proc_cursor--;
        } else if (c == 's' || c == 'S') {
            if (s_proc_cursor < TRAIN_PROC__COUNT - 1) s_proc_cursor++;
        }
        /* Track last non-free cursor position as the free label source. */
        if (s_proc_cursor < (int)TRAIN_PROC_FREE)
            s_free_src_proc = (train_proc_t)s_proc_cursor;

        if (c == '\r' || c == '\n') {
            s_proc = (train_proc_t)s_proc_cursor;
            s_ui   = TRAIN_UI_NAME;
        } else if (c == '?' || c == 'h' || c == 'H') {
            s_proc = (train_proc_t)s_proc_cursor;
            s_ui   = TRAIN_UI_HELP;
        } else if (c == '\x1b' || c == 'q' || c == 'Q') {
            g_app.mode = APP_MODE_MENU;
        }
        break;

    case TRAIN_UI_HELP:
        if (c == '\x1b' || c == 'q' || c == 'Q' || c == '\r')
            s_ui = TRAIN_UI_PROC_SEL;
        break;

    case TRAIN_UI_NAME:
        if (c == '\r' || c == '\n') {
            if (s_name_len == 0) {
                strncpy(s_name, "session", sizeof s_name - 1);
                s_name_len = 7;
            }
            if (s_proc == TRAIN_PROC_FREE) {
                build_free_label_list();
                s_frames_written = 0;
                s_ui = TRAIN_UI_FREE_LABEL;
            } else {
                start_guided(millis());
            }
        } else if ((c == '\x7f' || c == '\x08') && s_name_len > 0) {
            s_name[--s_name_len] = '\0';
        } else if (c >= 0x20 && c < 0x7f && s_name_len < 31) {
            char safe = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '-') ? c : '_';
            s_name[s_name_len++] = safe;
            s_name[s_name_len]   = '\0';
        } else if (c == '\x1b') {
            s_name_len = 0;
            s_name[0] = '\0';
            s_ui = TRAIN_UI_PROC_SEL;
        }
        break;

    case TRAIN_UI_RUNNING:
        if (c == '\x1b' || c == 'q' || c == 'Q') {
            sd_close();
            s_ui = TRAIN_UI_PROC_SEL;
        }
        break;

    case TRAIN_UI_DONE:
        if (c == '\x1b' || c == 'q' || c == 'Q' || c == '\r')
            s_ui = TRAIN_UI_PROC_SEL;
        break;

    /* -- Free mode -- */
    case TRAIN_UI_FREE_LABEL:
        if (c == 'w' || c == 'W') {
            if (s_free_label_cursor > 0) s_free_label_cursor--;
        } else if (c == 's' || c == 'S') {
            if (s_free_label_cursor < s_free_label_count - 1) s_free_label_cursor++;
        } else if (c == '\r' || c == '\n') {
            if (s_free_label_count == 0) break;
            strncpy(s_free_label, s_free_labels[s_free_label_cursor],
                    sizeof s_free_label - 1);
            s_free_label[sizeof s_free_label - 1] = '\0';
            s_free_cfg_field = 0;
            s_ui = TRAIN_UI_FREE_CFG;
        } else if (c == '\x1b') {
            sd_close();
            s_ui = TRAIN_UI_PROC_SEL;
        }
        break;

    case TRAIN_UI_FREE_CFG:
        if (c == 'w' || c == 'W') {
            if (s_free_cfg_field > 0) s_free_cfg_field--;
        } else if (c == 's' || c == 'S') {
            if (s_free_cfg_field < 1) s_free_cfg_field++;
        } else if (c == 'a' || c == 'A') {
            if (s_free_cfg_field == 0 && s_free_cooldown > 0)  s_free_cooldown--;
            if (s_free_cfg_field == 1 && s_free_cap_s   > 1)   s_free_cap_s--;
        } else if (c == 'd' || c == 'D') {
            if (s_free_cfg_field == 0 && s_free_cooldown < 30) s_free_cooldown++;
            if (s_free_cfg_field == 1 && s_free_cap_s   < 120) s_free_cap_s++;
        } else if (c == '\r' || c == '\n') {
            start_free_capture(millis());
        } else if (c == '\x1b') {
            s_ui = TRAIN_UI_FREE_LABEL;
        }
        break;

    case TRAIN_UI_FREE_DONE:
        if (c == 'r' || c == 'R' || c == '\r') {
            start_free_capture(millis());
        } else if (c == 'n' || c == 'N') {
            s_ui = TRAIN_UI_FREE_LABEL;
        } else if (c == '\x1b' || c == 'q' || c == 'Q') {
            sd_close();
            s_ui = TRAIN_UI_PROC_SEL;
        }
        break;
    }
}

void training_tick(uint32_t now_ms) {
    if (s_ui != TRAIN_UI_RUNNING) return;
    if ((int32_t)(now_ms - s_deadline) < 0) return;
    if (!s_in_cap) {
        enter_cap(now_ms);
    } else {
        advance_step(now_ms);
    }
}

void training_on_frame(const csi_frame_t *f, uint32_t now_ms) {
    (void)now_ms;
    if (s_ui != TRAIN_UI_RUNNING) return;
    if (!s_in_cap) return;
    const train_step_t *st = cur_step();
    if (!st || !st->label) return;
    sd_write(f, st->label);
    s_frames_written++;
}

/* -- View accessors -- */
train_ui_t   training_ui(void)             { return s_ui; }
train_proc_t training_proc(void)           { return s_proc; }
int          training_step(void)           { return s_step; }
int          training_step_total(void)     { return steps_total(); }
bool         training_in_cap(void)         { return s_in_cap; }
int          training_frames_written(void) { return s_frames_written; }
const char*  training_session_name(void)   { return s_name; }
const char*  training_filename(void)       { return s_filename; }
int          training_proc_cursor(void)    { return s_proc_cursor; }
train_proc_t training_free_src_proc(void)  { return s_free_src_proc; }

int training_remain_s(void) {
    uint32_t now = millis();
    if ((int32_t)(s_deadline - now) <= 0) return 0;
    return (int)((s_deadline - now) / 1000u);
}

const char* training_step_label(void) {
    const train_step_t *st = cur_step();
    return st ? (st->label ? st->label : "(skip)") : "";
}
const char* training_step_instr1(void) {
    const train_step_t *st = cur_step();
    return st ? st->instr1 : "";
}
const char* training_step_instr2(void) {
    const train_step_t *st = cur_step();
    return st ? st->instr2 : "";
}

const char* training_next_instr1(void) {
    if (is_free_running()) return "";
    const proc_def_t *pd = &kProcs[s_proc];
    int next = s_step + 1;
    if (next >= pd->n_steps) return "";
    return pd->steps[next].instr1 ? pd->steps[next].instr1 : "";
}

int training_phase_dur_s(void) {
    const train_step_t *st = cur_step();
    if (!st) return 1;
    return s_in_cap ? (int)st->cap_s : (int)st->buf_s;
}

const char* training_proc_name(train_proc_t p) {
    return (p < TRAIN_PROC__COUNT) ? kProcs[p].name : "";
}
const char* training_proc_help(train_proc_t p) {
    return (p < TRAIN_PROC__COUNT) ? kProcs[p].help : "";
}

/* Free-mode accessors */
const char* training_free_label(void)       { return s_free_label; }
int         training_free_cooldown(void)    { return s_free_cooldown; }
int         training_free_cap_s(void)       { return s_free_cap_s; }
int         training_free_cfg_field(void)   { return s_free_cfg_field; }
int         training_free_label_count(void) { return s_free_label_count; }
int         training_free_label_cursor(void){ return s_free_label_cursor; }
const char* training_free_label_at(int i) {
    return (i >= 0 && i < s_free_label_count) ? s_free_labels[i] : "";
}
