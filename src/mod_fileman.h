#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FM_BROWSE  = 0,
    FM_CONFIRM,
    FM_MSG,
} fileman_mode_t;

void fileman_init(void);
void fileman_key(char c);
void fileman_tick(uint32_t now_ms);

fileman_mode_t fileman_mode(void);
bool        fileman_sd_ok(void);
const char* fileman_cwd(void);
int         fileman_count(void);
const char* fileman_name(int i);
bool        fileman_is_dir(int i);
long        fileman_size(int i);
int         fileman_cursor(void);
int         fileman_scroll(void);
const char* fileman_msg(void);
const char* fileman_confirm_name(void);

#ifdef __cplusplus
}
#endif
