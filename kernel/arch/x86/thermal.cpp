// Copyright 2017 The Fuchsia Authors
// Copyright (c) 2016 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <zircon/compiler.h>
#include <err.h>
#include <inttypes.h>
#include <string.h>
#include <arch/x86/feature.h>
#include <kernel/auto_lock.h>
#include <kernel/mp.h>
#include <lib/console.h>

#define PRINT_MSR(msr)     print_msr(msr, #msr, 0)
#define PRINT_CLR_MSR(msr) print_msr(msr, #msr, 1)

static void print_msr(uint32_t msr_id, const char* name, bool clear) {
    uint64_t v = read_msr(msr_id);
    printf("    %s=0x%016" PRIx64 "\n", name, v);
    if (clear) write_msr(msr_id, v);
}

static void thermal_default(void) {
    uint64_t d = read_msr(X86_MSR_PKG_POWER_INFO) & 0x7f;
    uint64_t v = read_msr(X86_MSR_PKG_POWER_LIMIT);
    v &= ~0x7f;
    v |= d;
    write_msr(X86_MSR_PKG_POWER_LIMIT, v);
}

static void thermal_set(void) {
    uint64_t target_w = 3; // watts

    // sets PL1
    uint64_t u = (1 << (read_msr(X86_MSR_RAPL_POWER_UNIT) & 0xf));
    uint64_t v = read_msr(X86_MSR_PKG_POWER_LIMIT);
    v &= ~0x7f;
    v |= (target_w * u) & 0x7f;
    write_msr(X86_MSR_PKG_POWER_LIMIT, v);
}

static void thermal_dump(void) {
    PRINT_MSR(X86_MSR_IA32_MISC_ENABLE);
    PRINT_MSR(X86_MSR_IA32_THERM_STATUS);
    PRINT_MSR(X86_MSR_IA32_THERM_INTERRUPT);
    PRINT_MSR(X86_MSR_IA32_PACKAGE_THERM_STATUS);
    PRINT_MSR(X86_MSR_IA32_PACKAGE_THERM_INTERRUPT);
    PRINT_MSR(X86_MSR_THERM2_CTL);
    PRINT_MSR(X86_MSR_RAPL_POWER_UNIT);
    PRINT_MSR(X86_MSR_PKG_POWER_LIMIT);
    PRINT_MSR(X86_MSR_PKG_ENERGY_STATUS);
    PRINT_MSR(X86_MSR_PKG_PERF_STATUS);
    PRINT_MSR(X86_MSR_PKG_POWER_INFO);
#if 0
    PRINT_MSR(X86_MSR_DRAM_POWER_LIMIT);
    PRINT_MSR(X86_MSR_DRAM_ENERGY_STATUS);
    PRINT_MSR(X86_MSR_DRAM_PERF_STATUS);
    PRINT_MSR(X86_MSR_DRAM_POWER_INFO);
    PRINT_MSR(X86_MSR_PP0_ENERGY_STATUS);
    PRINT_MSR(X86_MSR_PP0_POWER_LIMIT);
    PRINT_MSR(X86_MSR_PP0_POLICY);
    PRINT_MSR(X86_MSR_PP1_ENERGY_STATUS);
    PRINT_MSR(X86_MSR_PP1_POWER_LIMIT);
    PRINT_MSR(X86_MSR_PP1_POLICY);
#endif
}

static int cmd_thermal(int argc, const cmd_args* argv, uint32_t flags) {
    if (argc < 2) {
usage:
        printf("usage:\n");
        printf("%s dump\n", argv[0].str);
        printf("%s set\n", argv[0].str);
        printf("%s default\n", argv[0].str);
        return ZX_ERR_INTERNAL;
    }

    if (!strcmp(argv[1].str, "dump")) {
        thermal_dump();
    } else if (!strcmp(argv[1].str, "set")) {
        thermal_set();
    } else if (!strcmp(argv[1].str, "default")) {
        thermal_default();
    } else {
        printf("unknown command\n");
        goto usage;
    }
    return ZX_OK;
}

STATIC_COMMAND_START
STATIC_COMMAND("thermal", "thermal features\n", &cmd_thermal)
STATIC_COMMAND_END(thermal);
