#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "mod_fileman.h"

extern "C" {
#include "sic/storage/sd.h"
}

#define FM_MAX_ENTRIES  32
#define FM_NAME_LEN     48
#define FM_PATH_LEN    128
#define FM_MSG_MS      1400

typedef struct {
    char name[FM_NAME_LEN];
    long size;
    bool is_dir;
} fm_entry_t;

static fileman_mode_t s_mode    = FM_BROWSE;
static bool           s_sd_ok   = false;
static char           s_cwd[FM_PATH_LEN] = "/";
static fm_entry_t     s_ents[FM_MAX_ENTRIES];
static int            s_n       = 0;
static int            s_cursor  = 0;
static int            s_scroll  = 0;
static char           s_msg[48] = {};
static uint32_t       s_msg_end = 0;
static char           s_cname[FM_NAME_LEN] = {};
static char           s_cpath[FM_PATH_LEN] = {};

static bool sd_mount(void) {
    const sd_t *sd = sic_sd(0);
    if (!sd) return false;
    if (sd->v->begin(sd) != 0) return false;
    return sd->v->present(sd) != 0;
}

static void child_path(char *out, size_t sz, const char *dir, const char *name) {
    if (dir[0] == '/' && dir[1] == '\0')
        snprintf(out, sz, "/%s", name);
    else
        snprintf(out, sz, "%s/%s", dir, name);
}

static void go_up(void) {
    char *last = strrchr(s_cwd, '/');
    if (!last) return;
    if (last == s_cwd) { s_cwd[1] = '\0'; return; }
    *last = '\0';
}

static void clamp_scroll(void) {
    const int vis = 7;
    if (s_cursor < s_scroll) s_scroll = s_cursor;
    if (s_cursor >= s_scroll + vis) s_scroll = s_cursor - vis + 1;
    if (s_scroll < 0) s_scroll = 0;
}

static void list_dir(void) {
    s_n     = 0;
    s_sd_ok = false;
    if (!sd_mount()) return;
    s_sd_ok = true;

    File dir = SD.open(s_cwd);
    if (!dir) return;

    for (;;) {
        File f = dir.openNextFile();
        if (!f) break;
        if (s_n < FM_MAX_ENTRIES) {
            s_ents[s_n].is_dir = f.isDirectory();
            s_ents[s_n].size   = s_ents[s_n].is_dir ? 0L : (long)f.size();
            strncpy(s_ents[s_n].name, f.name(), FM_NAME_LEN - 1);
            s_ents[s_n].name[FM_NAME_LEN - 1] = '\0';
            s_n++;
        }
        f.close();
    }
    dir.close();

    /* sort: dirs first, then alphabetical within each group */
    for (int i = 0; i < s_n - 1; i++) {
        for (int j = i + 1; j < s_n; j++) {
            bool swap = false;
            if (s_ents[j].is_dir && !s_ents[i].is_dir)
                swap = true;
            else if (s_ents[i].is_dir == s_ents[j].is_dir)
                swap = strcasecmp(s_ents[i].name, s_ents[j].name) > 0;
            if (swap) {
                fm_entry_t tmp = s_ents[i];
                s_ents[i] = s_ents[j];
                s_ents[j] = tmp;
            }
        }
    }
}

static void do_delete(void) {
    if (SD.remove(s_cpath))
        snprintf(s_msg, sizeof s_msg, "deleted");
    else
        snprintf(s_msg, sizeof s_msg, "delete failed");
}

void fileman_init(void) {
    s_mode   = FM_BROWSE;
    s_cursor = 0;
    s_scroll = 0;
    s_msg[0] = '\0';
    strcpy(s_cwd, "/");
    list_dir();
}

void fileman_key(char c) {
    if (s_mode == FM_MSG) {
        s_mode = FM_BROWSE;
        list_dir();
        if (s_cursor >= s_n) s_cursor = s_n > 0 ? s_n - 1 : 0;
        clamp_scroll();
        return;
    }

    if (s_mode == FM_CONFIRM) {
        if (c == '\r') {
            do_delete();
            s_mode   = FM_MSG;
            s_msg_end = (uint32_t)millis() + FM_MSG_MS;
            list_dir();
            if (s_cursor >= s_n) s_cursor = s_n > 0 ? s_n - 1 : 0;
            clamp_scroll();
        } else {
            s_mode = FM_BROWSE;
        }
        return;
    }

    /* FM_BROWSE */
    if (c == 'w') {
        if (s_cursor > 0) { s_cursor--; clamp_scroll(); }
    } else if (c == 's') {
        if (s_cursor < s_n - 1) { s_cursor++; clamp_scroll(); }
    } else if (c == '\r' || c == 'd') {
        if (s_n == 0) return;
        fm_entry_t *e = &s_ents[s_cursor];
        if (e->is_dir) {
            char newpath[FM_PATH_LEN];
            child_path(newpath, sizeof newpath, s_cwd, e->name);
            strncpy(s_cwd, newpath, sizeof s_cwd - 1);
            s_cwd[sizeof s_cwd - 1] = '\0';
            s_cursor = 0;
            s_scroll = 0;
            list_dir();
        } else {
            strncpy(s_cname, e->name, sizeof s_cname - 1);
            s_cname[sizeof s_cname - 1] = '\0';
            child_path(s_cpath, sizeof s_cpath, s_cwd, e->name);
            s_mode = FM_CONFIRM;
        }
    } else if (c == 'a') {
        if (s_cwd[0] == '/' && s_cwd[1] == '\0') return;
        go_up();
        s_cursor = 0;
        s_scroll = 0;
        list_dir();
    }
}

void fileman_tick(uint32_t now_ms) {
    if (s_mode == FM_MSG && now_ms >= s_msg_end) {
        s_mode = FM_BROWSE;
        list_dir();
        if (s_cursor >= s_n) s_cursor = s_n > 0 ? s_n - 1 : 0;
        clamp_scroll();
    }
}

fileman_mode_t fileman_mode(void)         { return s_mode; }
bool        fileman_sd_ok(void)           { return s_sd_ok; }
const char* fileman_cwd(void)             { return s_cwd; }
int         fileman_count(void)           { return s_n; }
const char* fileman_name(int i)           { return (i >= 0 && i < s_n) ? s_ents[i].name : ""; }
bool        fileman_is_dir(int i)         { return (i >= 0 && i < s_n) && s_ents[i].is_dir; }
long        fileman_size(int i)           { return (i >= 0 && i < s_n) ? s_ents[i].size : 0L; }
int         fileman_cursor(void)          { return s_cursor; }
int         fileman_scroll(void)          { return s_scroll; }
const char* fileman_msg(void)             { return s_msg; }
const char* fileman_confirm_name(void)    { return s_cname; }
