#include "cpuinfo.h"
#include "kstring.h"

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static void put32(char *dst, uint32_t v) {
    for (int i = 0; i < 4; i++) dst[i] = (char)(v >> (8 * i));
}

void cpuinfo_init(struct cpu_info *ci) {
    uint32_t a, b, c, d, maxleaf;
    memset(ci, 0, sizeof(*ci));
    cpuid(0, 0, &maxleaf, &b, &c, &d);
    put32(ci->vendor, b); put32(ci->vendor + 4, d); put32(ci->vendor + 8, c);
    ci->vendor[12] = 0;

    ci->logical = 1;
    if (maxleaf >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        uint32_t base = (a >> 8) & 0xF;
        ci->family = base == 0xF ? base + ((a >> 20) & 0xFF) : base;
        ci->model = (a >> 4) & 0xF;
        if (base == 6 || base == 0xF) ci->model |= ((a >> 16) & 0xF) << 4;
        ci->stepping = a & 0xF;
        ci->feat_ecx = c; ci->feat_edx = d;
        ci->hypervisor = (c >> 31) & 1;
        if ((d >> 28) & 1) ci->logical = (b >> 16) & 0xFF;
        if (ci->logical == 0) ci->logical = 1;
    }
    if (ci->hypervisor) {
        cpuid(0x40000000, 0, &a, &b, &c, &d);
        put32(ci->hv_vendor, b); put32(ci->hv_vendor + 4, c); put32(ci->hv_vendor + 8, d);
        ci->hv_vendor[12] = 0;
    }
    if (maxleaf >= 7) { cpuid(7, 0, &a, &b, &c, &d); ci->feat7_ebx = b; }

    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        char tmp[49];
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, &a, &b, &c, &d);
            put32(tmp + i * 16, a); put32(tmp + i * 16 + 4, b);
            put32(tmp + i * 16 + 8, c); put32(tmp + i * 16 + 12, d);
        }
        tmp[48] = 0;
        const char *p = tmp;
        while (*p == ' ') p++;
        int n = 0;
        while (p[n] && n < 48) { ci->brand[n] = p[n]; n++; }
        ci->brand[n] = 0;
    }
}

const char *cpuinfo_env_name(const struct cpu_info *c) {
    if (!c->hypervisor) return "реальное железо";
    if (strcmp(c->hv_vendor, "VBoxVBoxVBox") == 0) return "VirtualBox";
    if (strcmp(c->hv_vendor, "KVMKVMKVM") == 0 || strncmp(c->hv_vendor, "KVMKVMKVM", 9) == 0) return "KVM / QEMU";
    if (strcmp(c->hv_vendor, "TCGTCGTCGTCG") == 0) return "QEMU (эмуляция)";
    if (strcmp(c->hv_vendor, "Microsoft Hv") == 0) return "Hyper-V";
    if (strcmp(c->hv_vendor, "VMwareVMware") == 0) return "VMware";
    return "виртуальная машина";
}
