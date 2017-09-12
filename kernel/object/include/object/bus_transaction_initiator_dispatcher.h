// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <dev/iommu.h>
#include <fbl/canary.h>
#include <fbl/mutex.h>
#include <object/dispatcher.h>
#include <object/pinned_memory_object.h>

#include <sys/types.h>

class Iommu;

class BusTransactionInitiatorDispatcher final : public Dispatcher {
public:
    static zx_status_t Create(fbl::RefPtr<Iommu> iommu, uint64_t bti_id,
                              fbl::RefPtr<Dispatcher>* dispatcher, zx_rights_t* rights);

    ~BusTransactionInitiatorDispatcher() final;
    zx_obj_type_t get_type() const final { return ZX_OBJ_TYPE_BTI; }
    bool has_state_tracker() const final { return true; }

    // Pins the given VMO range and writes the addresses into |mapped_extents|.  The
    // number of addresses returned is given in |actual_mapped_extents_len|.
    //
    // Returns ZX_ERR_INVALID_ARGS if |offset| or |size| are not PAGE_SIZE aligned.
    // Returns ZX_ERR_INVALID_ARGS if |perms| is not suitable to pass to the Iommu::Map() interface.
    // Returns ZX_ERR_BUFFER_TOO_SMALL if |mapped_extents_len| is not at least |size|/PAGE_SIZE.
    zx_status_t Pin(fbl::RefPtr<VmObject> vmo, uint64_t offset, uint64_t size, uint32_t perms,
                    uint64_t* mapped_extents, size_t mapped_extents_len,
                    size_t* actual_mapped_extents_len);

    // Unpins the given list of extents.  Returns an error if the described
    // list of extents do not correspond to the exact set created in a
    // previous call to Pin().
    zx_status_t Unpin(const uint64_t* mapped_extents, size_t mapped_extents_len);

    void on_zero_handles() final;

    fbl::RefPtr<Iommu> iommu() const { return iommu_; }
    uint64_t bti_id() const { return bti_id_; }

private:
    BusTransactionInitiatorDispatcher(fbl::RefPtr<Iommu> iommu, uint64_t bti_id);

    fbl::Canary<fbl::magic("BTID")> canary_;

    fbl::Mutex lock_;
    const fbl::RefPtr<Iommu> iommu_;
    const uint64_t bti_id_;

    fbl::DoublyLinkedList<fbl::unique_ptr<PinnedMemoryObject>> pinned_memory_ TA_GUARDED(lock_);
    bool zero_handles_ TA_GUARDED(lock_);
};
