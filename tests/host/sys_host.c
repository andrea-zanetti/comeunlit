/* Host stand-in for sb_sys.c */
#include "sb_sys.h"
uint32_t sb_sys_uptime_ms(void) { return 123456; }
uint32_t sb_sys_heap_free(void) { return 74210; }
int sb_sys_streams(void) { return 0; }
const char* sb_sys_shield(void) { return "1"; }
