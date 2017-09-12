// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT


#include <object/pinned_memory_object.h>

#include <assert.h>
#include <err.h>
#include <vm/vm.h>
#include <vm/vm_object.h>
#include <zxcpp/new.h>
#include <fbl/algorithm.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <object/bus_transaction_initiator_dispatcher.h>
#include <trace.h>

#define LOCAL_TRACE 0

namespace {

struct IommuMapPageContext {
    fbl::RefPtr<Iommu> iommu;
    uint64_t bus_txn_id;
    PinnedMemoryObject::Extent* page_array;
    size_t num_entries;
    uint32_t perms;
};

// Callback for VmObject::Lookup that handles mapping individual pages into the IOMMU.
zx_status_t IommuMapPage(void* context, size_t offset, size_t index, paddr_t pa) {
    IommuMapPageContext* ctx = static_cast<IommuMapPageContext*>(context);

    dev_vaddr_t vaddr;
    zx_status_t status = ctx->iommu->Map(ctx->bus_txn_id, pa, PAGE_SIZE, ctx->perms, &vaddr);
    if (status != ZX_OK) {
        return status;
    }

    DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
    // TODO(teisenbe): Re-enable this to get run-encoding.
    /*
    if (ctx->num_entries == 0) {
        ctx->page_array[0] = PinnedMemoryObject::Extent(vaddr, 1);
        ctx->num_entries++;
        return ZX_OK;
    }

    PinnedMemoryObject::Extent* prev_extent = &ctx->page_array[ctx->num_entries - 1];
    if (prev_extent->base() + prev_extent->pages() * PAGE_SIZE == vaddr &&
        prev_extent->extend(1) == ZX_OK) {

        return ZX_OK;
    }
    ctx->page_array[ctx->num_entries] = PinnedMemoryObject::Extent(vaddr, 1);
    ctx->num_entries++;
    */
    ctx->page_array[ctx->num_entries] = PinnedMemoryObject::Extent(vaddr, 1);
    ctx->num_entries++;
    return ZX_OK;
}

} // namespace {}

zx_status_t PinnedMemoryObject::Create(const BusTransactionInitiatorDispatcher& bti,
                                       fbl::RefPtr<VmObject> vmo, size_t offset,
                                       size_t size, uint32_t perms,
                                       fbl::unique_ptr<PinnedMemoryObject>* out) {
    LTRACE_ENTRY;
    DEBUG_ASSERT(IS_PAGE_ALIGNED(offset));

    // Pin the memory to make sure it doesn't change from underneath us for the
    // lifetime of the created PMO.
    zx_status_t status = vmo->Pin(offset, size);
    if (status != ZX_OK) {
        LTRACEF("vmo->Pin failed: %d\n", status);
        return status;
    }

    uint64_t expected_addr = 0;
    auto check_contiguous = [](void* context, size_t offset, size_t index, paddr_t pa) {
        auto expected_addr = static_cast<uint64_t*>(context);
        if (index != 0 && pa != *expected_addr) {
            return ZX_ERR_NOT_FOUND;
        }
        *expected_addr = pa + PAGE_SIZE;
        return ZX_OK;
    };
    status = vmo->Lookup(offset, size, 0, check_contiguous, &expected_addr);
    bool is_contiguous = (status == ZX_OK);

    // Set up a cleanup function to undo the pin if we need to fail this
    // operation.
    auto unpin_vmo = fbl::MakeAutoCall([vmo, offset, size]() {
        vmo->Unpin(offset, size);
    });

    // TODO(teisenbe): Be more intelligent about allocating this, since if this
    // is backed by a real IOMMU, we will likely compress the page array greatly
    // using extents.
    fbl::AllocChecker ac;
    const size_t num_pages = is_contiguous ? 1 : ROUNDUP(size, PAGE_SIZE) / PAGE_SIZE;
    fbl::unique_ptr<Extent[]> page_array(new (&ac) Extent[num_pages]);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    fbl::unique_ptr<PinnedMemoryObject> pmo(
            new (&ac) PinnedMemoryObject(bti, fbl::move(vmo), offset, size, is_contiguous,
                                         fbl::move(page_array)));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    // Now that the pmo object has been created, it is responsible for
    // unpinning.
    unpin_vmo.cancel();

    status = pmo->MapIntoIommu(perms);
    if (status != ZX_OK) {
        LTRACEF("MapIntoIommu failed: %d\n", status);
        return status;
    }

    *out = fbl::move(pmo);
    return ZX_OK;
}

// Used during initialization to set up the IOMMU state for this PMO.
zx_status_t PinnedMemoryObject::MapIntoIommu(uint32_t perms) {
    if (is_contiguous_) {
        paddr_t paddr = 1;
        auto get_paddr = [](void* context, size_t offset, size_t index, paddr_t pa) {
            *static_cast<paddr_t*>(context) = pa;
            return ZX_OK;
        };
        const size_t lookup_size = fbl::min<size_t>(size_, PAGE_SIZE);
        zx_status_t status = vmo_->Lookup(offset_, lookup_size, 0, get_paddr, &paddr);
        if (status != ZX_OK) {
            return status;
        }
        ASSERT(paddr != 1);

        dev_vaddr_t vaddr;
        status = bti_.iommu()->Map(bti_.bti_id(), paddr, ROUNDUP(size_, PAGE_SIZE), perms, &vaddr);
        if (status != ZX_OK) {
            return status;
        }
        mapped_extents_[0] = PinnedMemoryObject::Extent(vaddr, 1);
        mapped_extents_len_ = 1;
        return ZX_OK;
    }

    IommuMapPageContext context = {
        .iommu = bti_.iommu(),
        .bus_txn_id = bti_.bti_id(),
        .page_array = mapped_extents_.get(),
        .num_entries = 0,
        .perms = perms,
    };
    zx_status_t status = vmo_->Lookup(offset_, size_, 0, IommuMapPage, static_cast<void*>(&context));
    mapped_extents_len_ = context.num_entries;
    if (status != ZX_OK) {
        zx_status_t err = UnmapFromIommu();
        ASSERT(err == ZX_OK);
        return status;
    }

    return ZX_OK;
}

zx_status_t PinnedMemoryObject::UnmapFromIommu() {
    auto iommu = bti_.iommu();
    const uint64_t bus_txn_id = bti_.bti_id();

    if (unlikely(mapped_extents_len_ == 0)) {
        return ZX_OK;
    }

    zx_status_t status = ZX_OK;
    if (is_contiguous_) {
        status = iommu->Unmap(bus_txn_id, mapped_extents_[0].base(), ROUNDUP(size_, PAGE_SIZE));
    } else {
        for (size_t i = 0; i < mapped_extents_len_; ++i) {
            // Try to unmap all pages even if we get an error, and return the
            // first error encountered.
            zx_status_t err = iommu->Unmap(bus_txn_id, mapped_extents_[i].base(),
                                        mapped_extents_[i].pages() * PAGE_SIZE);
            DEBUG_ASSERT(err == ZX_OK);
            if (err != ZX_OK && status == ZX_OK) {
                status = err;
            }
        }
    }
    // Clear this so we won't try again if this gets called again in the
    // destructor.
    mapped_extents_len_ = 0;
    return status;
}

PinnedMemoryObject::~PinnedMemoryObject() {
    zx_status_t status = UnmapFromIommu();
    ASSERT(status == ZX_OK);
    vmo_->Unpin(offset_, size_);
}

PinnedMemoryObject::PinnedMemoryObject(const BusTransactionInitiatorDispatcher& bti,
                                       fbl::RefPtr<VmObject> vmo, size_t offset, size_t size,
                                       bool is_contiguous,
                                       fbl::unique_ptr<Extent[]> mapped_extents)
    : vmo_(fbl::move(vmo)), offset_(offset), size_(size), is_contiguous_(is_contiguous), bti_(bti),
      mapped_extents_(fbl::move(mapped_extents)), mapped_extents_len_(0) {
}
