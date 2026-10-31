#ifndef LIMON_CPUINFO_H
#define LIMON_CPUINFO_H
#include <stdint.h>

struct cpu_info {
    char vendor[13];
    char brand[49];
    uint32_t family, model, stepping, logical;
    uint32_t feat_ecx, feat_edx, feat7_ebx;
    int hypervisor;
    char hv_vendor[13];
};

const char *cpuinfo_env_name(const struct cpu_info *c);   /* "VirtualBox", "реальное железо" ... */

void cpuinfo_init(struct cpu_info *c);
#endif
