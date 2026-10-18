#ifndef LIMON_SYSINFO_H
#define LIMON_SYSINFO_H
#include <stdint.h>
#include "cpuinfo.h"

#define LIMON_VERSION "0.4"

struct sysinfo {
    uint32_t fb_w, fb_h, fb_bpp;
    int mem_valid;
    uint64_t mem_usable, mem_reclaim, mem_reserved;   /* в байтах */
    struct cpu_info cpu;
};

extern struct sysinfo g_sys;
#endif
