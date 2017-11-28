// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2014 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "vm_priv.h"
#include <arch/mmu.h>
#include <assert.h>
#include <debug.h>
#include <err.h>
#include <fbl/algorithm.h>
#include <inttypes.h>
#include <kernel/thread.h>
#include <lib/console.h>
#include <lib/crypto/global_prng.h>
#include <string.h>
#include <trace.h>
#include <vm/bootalloc.h>
#include <vm/init.h>
#include <vm/physmap.h>
#include <vm/pmm.h>
#include <vm/vm.h>
#include <vm/vm_aspace.h>
#include <zircon/types.h>

#define LOCAL_TRACE MAX(VM_GLOBAL_TRACE, 0)

extern int __code_start;
extern int __code_end;
extern int __rodata_start;
extern int __rodata_end;
extern int __data_start;
extern int __data_end;
extern int __bss_start;
extern int _end;

// boot time allocated page full of zeros
vm_page_t* zero_page;
paddr_t zero_page_paddr;

namespace {

// mark the physical pages backing a range of virtual as in use.
// allocate the physical pages and throw them away
void MarkPagesInUse(vaddr_t va, size_t len) {
    LTRACEF("va %#" PRIxPTR ", len %#zx\n", va, len);

    // make sure we are inclusive of all of the pages in the address range
    len = PAGE_ALIGN(len + (va & (PAGE_SIZE - 1)));
    va = ROUNDDOWN(va, PAGE_SIZE);

    LTRACEF("aligned va %#" PRIxPTR ", len 0x%zx\n", va, len);

    list_node list = LIST_INITIAL_VALUE(list);

    paddr_t start_pa = ULONG_MAX;
    paddr_t runlen = 0;
    for (size_t offset = 0; offset < len; offset += PAGE_SIZE) {
        uint flags;
        paddr_t pa;

        zx_status_t err = VmAspace::kernel_aspace()->arch_aspace().Query(va + offset, &pa, &flags);
        if (err >= 0) {
            LTRACEF("va %#" PRIxPTR ", pa %#" PRIxPTR ", flags %#x, err %d, start_pa %#" PRIxPTR
                    " runlen %#" PRIxPTR "\n",
                    va + offset, pa, flags, err, start_pa, runlen);

            // see if we continue the run
            if (pa == start_pa + runlen) {
                runlen += PAGE_SIZE;
            } else {
                if (start_pa != ULONG_MAX) {
                    // we just completed the run
                    pmm_alloc_range(start_pa, runlen / PAGE_SIZE, &list);
                }

                // starting a new run
                start_pa = pa;
                runlen = PAGE_SIZE;
            }
        } else {
            panic("Could not find pa for va %#" PRIxPTR "\n", va);
        }
    }

    if (start_pa != ULONG_MAX && runlen > 0)
        pmm_alloc_range(start_pa, runlen / PAGE_SIZE, &list);

    // mark all of the pages we allocated as WIRED
    vm_page_t* p;
    list_for_every_entry (&list, p, vm_page_t, free.node) { p->state = VM_PAGE_STATE_WIRED; }
}

zx_status_t ProtectRegion(VmAspace* aspace, vaddr_t va, uint arch_mmu_flags) {
    auto r = aspace->FindRegion(va);
    if (!r)
        return ZX_ERR_NOT_FOUND;

    auto vm_mapping = r->as_vm_mapping();
    if (!vm_mapping)
        return ZX_ERR_NOT_FOUND;

    return vm_mapping->Protect(vm_mapping->base(), vm_mapping->size(), arch_mmu_flags);
}

} // namespace {}

void vm_init_preheap() {
    LTRACE_ENTRY;

    // allow the vmm a shot at initializing some of its data structures
    VmAspace::KernelAspaceInitPreHeap();

    // mark all of the kernel pages in use
    LTRACEF("marking all kernel pages as used\n");
    MarkPagesInUse((vaddr_t)&__code_start,
                   (uintptr_t)&_end - (uintptr_t)&__code_start);

    // mark the physical pages used by the boot time allocator
    if (boot_alloc_end != boot_alloc_start) {
        dprintf(INFO, "VM: marking boot alloc used range [%#" PRIxPTR ", %#" PRIxPTR ")\n", boot_alloc_start,
                boot_alloc_end);

        MarkPagesInUse((vaddr_t)paddr_to_physmap(boot_alloc_start), boot_alloc_end - boot_alloc_start);
    }

    // Reserve up to 15 pages as a random padding in the kernel physical mapping
    uchar entropy;
    crypto::GlobalPRNG::GetInstance()->Draw(&entropy, sizeof(entropy));
    struct list_node list;
    list_initialize(&list);
    size_t page_count = entropy % 16;
    size_t allocated = pmm_alloc_pages(page_count, 0, &list);
    DEBUG_ASSERT(page_count == allocated);
    LTRACEF("physical mapping padding page count %#" PRIxPTR "\n", page_count);

    // grab a page and mark it as the zero page
    zero_page = pmm_alloc_page(0, &zero_page_paddr);
    DEBUG_ASSERT(zero_page);

    void* ptr = paddr_to_physmap(zero_page_paddr);
    DEBUG_ASSERT(ptr);

    arch_zero_page(ptr);
}

void vm_init() {
    LTRACE_ENTRY;

    VmAspace* aspace = VmAspace::kernel_aspace();

    // we expect the kernel to be in a temporary mapping, define permanent
    // regions for those now
    struct temp_region {
        const char* name;
        vaddr_t base;
        size_t size;
        uint arch_mmu_flags;
    } regions[] = {
        {
            .name = "kernel_code",
            .base = (vaddr_t)&__code_start,
            .size = ROUNDUP((size_t)&__code_end - (size_t)&__code_start, PAGE_SIZE),
            .arch_mmu_flags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_EXECUTE,
        },
        {
            .name = "kernel_rodata",
            .base = (vaddr_t)&__rodata_start,
            .size = ROUNDUP((size_t)&__rodata_end - (size_t)&__rodata_start, PAGE_SIZE),
            .arch_mmu_flags = ARCH_MMU_FLAG_PERM_READ,
        },
        {
            .name = "kernel_data",
            .base = (vaddr_t)&__data_start,
            .size = ROUNDUP((size_t)&__data_end - (size_t)&__data_start, PAGE_SIZE),
            .arch_mmu_flags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE,
        },
        {
            .name = "kernel_bss",
            .base = (vaddr_t)&__bss_start,
            .size = ROUNDUP((size_t)&_end - (size_t)&__bss_start, PAGE_SIZE),
            .arch_mmu_flags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE,
        },
    };

    for (uint i = 0; i < fbl::count_of(regions); ++i) {
        temp_region* region = &regions[i];
        ASSERT(IS_PAGE_ALIGNED(region->base));

        dprintf(INFO, "VM: reserving kernel region [%#" PRIxPTR ", %#" PRIxPTR ") flags %#x name '%s'\n",
                region->base, region->base + region->size, region->arch_mmu_flags, region->name);

        zx_status_t status = aspace->ReserveSpace(region->name, region->size, region->base);
        ASSERT(status == ZX_OK);
        status = ProtectRegion(aspace, region->base, region->arch_mmu_flags);
        ASSERT(status == ZX_OK);
    }

    // reserve the kernel aspace where the physmap is
    aspace->ReserveSpace("physmap", PHYSMAP_SIZE, PHYSMAP_BASE);

    // Reserve random padding of up to 64GB after first mapping. It will make
    // the adjacent memory mappings (kstack_vmar, arena:handles and others) at
    // non-static virtual addresses.
    size_t entropy;
    crypto::GlobalPRNG::GetInstance()->Draw(&entropy, sizeof(entropy));

    size_t random_size = PAGE_ALIGN(entropy % (64ULL * GB));
    zx_status_t status = aspace->ReserveSpace("random_padding", random_size, PHYSMAP_BASE + PHYSMAP_SIZE);
    ASSERT(status == ZX_OK);
    LTRACEF("VM: aspace random padding size: %#" PRIxPTR "\n", random_size);
}

paddr_t vaddr_to_paddr(const void* ptr) {
    if (is_physmap_addr(ptr))
        return physmap_to_paddr(ptr);

    auto aspace = VmAspace::vaddr_to_aspace(reinterpret_cast<uintptr_t>(ptr));
    if (!aspace)
        return (paddr_t) nullptr;

    paddr_t pa;
    zx_status_t rc = aspace->arch_aspace().Query((vaddr_t)ptr, &pa, nullptr);
    if (rc)
        return (paddr_t) nullptr;

    return pa;
}

static int cmd_vm(int argc, const cmd_args* argv, uint32_t flags) {
    if (argc < 2) {
    notenoughargs:
        printf("not enough arguments\n");
    usage:
        printf("usage:\n");
        printf("%s phys2virt <address>\n", argv[0].str);
        printf("%s virt2phys <address>\n", argv[0].str);
        printf("%s map <phys> <virt> <count> <flags>\n", argv[0].str);
        printf("%s unmap <virt> <count>\n", argv[0].str);
        return ZX_ERR_INTERNAL;
    }

    if (!strcmp(argv[1].str, "phys2virt")) {
        if (argc < 3)
            goto notenoughargs;

        if (!is_physmap_phys_addr(argv[2].u)) {
            printf("address isn't in physmap\n");
            return -1;
        }

        void* ptr = paddr_to_physmap((paddr_t)argv[2].u);
        printf("paddr_to_physmap returns %p\n", ptr);
    } else if (!strcmp(argv[1].str, "virt2phys")) {
        if (argc < 3)
            goto notenoughargs;

        VmAspace* aspace = VmAspace::vaddr_to_aspace(argv[2].u);
        if (!aspace) {
            printf("ERROR: outside of any address space\n");
            return -1;
        }

        paddr_t pa;
        uint flags;
        zx_status_t err = aspace->arch_aspace().Query(argv[2].u, &pa, &flags);
        printf("arch_mmu_query returns %d\n", err);
        if (err >= 0) {
            printf("\tpa %#" PRIxPTR ", flags %#x\n", pa, flags);
        }
    } else if (!strcmp(argv[1].str, "map")) {
        if (argc < 6)
            goto notenoughargs;

        VmAspace* aspace = VmAspace::vaddr_to_aspace(argv[2].u);
        if (!aspace) {
            printf("ERROR: outside of any address space\n");
            return -1;
        }

        size_t mapped;
        auto err =
            aspace->arch_aspace().Map(argv[3].u, argv[2].u, (uint)argv[4].u, (uint)argv[5].u, &mapped);
        printf("arch_mmu_map returns %d, mapped %zu\n", err, mapped);
    } else if (!strcmp(argv[1].str, "unmap")) {
        if (argc < 4)
            goto notenoughargs;

        VmAspace* aspace = VmAspace::vaddr_to_aspace(argv[2].u);
        if (!aspace) {
            printf("ERROR: outside of any address space\n");
            return -1;
        }

        size_t unmapped;
        auto err = aspace->arch_aspace().Unmap(argv[2].u, (uint)argv[3].u, &unmapped);
        printf("arch_mmu_unmap returns %d, unmapped %zu\n", err, unmapped);
    } else {
        printf("unknown command\n");
        goto usage;
    }

    return ZX_OK;
}

STATIC_COMMAND_START
#if LK_DEBUGLEVEL > 0
STATIC_COMMAND("vm", "vm commands", &cmd_vm)
#endif
STATIC_COMMAND_END(vm);
