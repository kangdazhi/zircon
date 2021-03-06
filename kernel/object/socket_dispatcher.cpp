// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <object/socket_dispatcher.h>

#include <string.h>

#include <assert.h>
#include <err.h>
#include <pow2.h>
#include <trace.h>

#include <lib/user_copy/user_ptr.h>

#include <vm/vm_aspace.h>
#include <vm/vm_object.h>
#include <vm/vm_object_paged.h>
#include <object/handle.h>

#include <zircon/rights.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>

using fbl::AutoLock;

#define LOCAL_TRACE 0

// static
zx_status_t SocketDispatcher::Create(uint32_t flags,
                                     fbl::RefPtr<Dispatcher>* dispatcher0,
                                     fbl::RefPtr<Dispatcher>* dispatcher1,
                                     zx_rights_t* rights) {
    LTRACE_ENTRY;

    if (flags & ~ZX_SOCKET_CREATE_MASK)
        return ZX_ERR_INVALID_ARGS;

    fbl::AllocChecker ac;

    zx_signals_t starting_signals = ZX_SOCKET_WRITABLE;

    if (flags & ZX_SOCKET_HAS_ACCEPT)
        starting_signals |= ZX_SOCKET_SHARE;

    fbl::unique_ptr<char[]> control0;
    fbl::unique_ptr<char[]> control1;

    // TODO: use mbufs to avoid pinning control buffer memory.
    if (flags & ZX_SOCKET_HAS_CONTROL) {
        starting_signals |= ZX_SOCKET_CONTROL_WRITABLE;

        control0.reset(new (&ac) char[kControlMsgSize]);
        if (!ac.check())
            return ZX_ERR_NO_MEMORY;

        control1.reset(new (&ac) char[kControlMsgSize]);
        if (!ac.check())
            return ZX_ERR_NO_MEMORY;
    }

    auto socket0 = fbl::AdoptRef(new (&ac) SocketDispatcher(starting_signals, flags, fbl::move(control0)));
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    auto socket1 = fbl::AdoptRef(new (&ac) SocketDispatcher(starting_signals, flags, fbl::move(control1)));
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    socket0->Init(socket1);
    socket1->Init(socket0);

    *rights = ZX_DEFAULT_SOCKET_RIGHTS;
    *dispatcher0 = fbl::move(socket0);
    *dispatcher1 = fbl::move(socket1);
    return ZX_OK;
}

SocketDispatcher::SocketDispatcher(zx_signals_t starting_signals, uint32_t flags,
                                   fbl::unique_ptr<char[]> control_msg)
    : Dispatcher(starting_signals),
      flags_(flags),
      peer_koid_(0u),
      control_msg_(fbl::move(control_msg)),
      control_msg_len_(0),
      read_disabled_(false) {
}

SocketDispatcher::~SocketDispatcher() {
}

// This is called before either SocketDispatcher is accessible from threads other than the one
// initializing the socket, so it does not need locking.
void SocketDispatcher::Init(fbl::RefPtr<SocketDispatcher> other) TA_NO_THREAD_SAFETY_ANALYSIS {
    other_ = fbl::move(other);
    peer_koid_ = other_->get_koid();
}

void SocketDispatcher::on_zero_handles() {
    canary_.Assert();

    fbl::RefPtr<SocketDispatcher> socket;
    {
        AutoLock lock(&lock_);
        socket = fbl::move(other_);
    }
    if (!socket)
        return;

    socket->OnPeerZeroHandles();
}

void SocketDispatcher::OnPeerZeroHandles() {
    canary_.Assert();

    AutoLock lock(&lock_);
    other_.reset();
    UpdateState(ZX_SOCKET_WRITABLE, ZX_SOCKET_PEER_CLOSED);
}

zx_status_t SocketDispatcher::user_signal(uint32_t clear_mask, uint32_t set_mask, bool peer) {
    canary_.Assert();

    if ((set_mask & ~ZX_USER_SIGNAL_ALL) || (clear_mask & ~ZX_USER_SIGNAL_ALL))
        return ZX_ERR_INVALID_ARGS;

    if (!peer) {
        UpdateState(clear_mask, set_mask);
        return ZX_OK;
    }

    fbl::RefPtr<SocketDispatcher> other;
    {
        AutoLock lock(&lock_);
        if (!other_)
            return ZX_ERR_PEER_CLOSED;
        other = other_;
    }

    return other->UserSignalSelf(clear_mask, set_mask);
}

zx_status_t SocketDispatcher::UserSignalSelf(uint32_t clear_mask, uint32_t set_mask) {
    canary_.Assert();
    UpdateState(clear_mask, set_mask);
    return ZX_OK;
}

zx_status_t SocketDispatcher::Shutdown(uint32_t how) {
    canary_.Assert();

    LTRACE_ENTRY;

    const bool shutdown_read = how & ZX_SOCKET_SHUTDOWN_READ;
    const bool shutdown_write = how & ZX_SOCKET_SHUTDOWN_WRITE;

    fbl::RefPtr<SocketDispatcher> other;
    {
        AutoLock lock(&lock_);
        zx_signals_t signals = GetSignalsState();
        // If we're already shut down in the requested way, return immediately.
        const uint32_t want_signals =
            (shutdown_read ? ZX_SOCKET_READ_DISABLED : 0) |
            (shutdown_write ? ZX_SOCKET_WRITE_DISABLED : 0);
        const uint32_t have_signals = signals & (ZX_SOCKET_READ_DISABLED | ZX_SOCKET_WRITE_DISABLED);
        if (want_signals == have_signals) {
            return ZX_OK;
        }
        other = other_;
        zx_signals_t clear_mask = 0u;
        zx_signals_t set_mask = 0u;
        if (shutdown_read) {
            read_disabled_ = true;
            if (is_empty())
                set_mask |= ZX_SOCKET_READ_DISABLED;
        }
        if (shutdown_write) {
            clear_mask |= ZX_SOCKET_WRITABLE;
            set_mask |= ZX_SOCKET_WRITE_DISABLED;
        }
        UpdateState(clear_mask, set_mask);
    }
    // Our peer already be closed - if so, we've already updated our own bits so we are done. If the
    // peer is done, we need to notify them of the state change.
    if (other) {
        return other->ShutdownOther(how);
    } else {
        return ZX_OK;
    }
}

zx_status_t SocketDispatcher::ShutdownOther(uint32_t how) {
    canary_.Assert();

    const bool shutdown_read = how & ZX_SOCKET_SHUTDOWN_READ;
    const bool shutdown_write = how & ZX_SOCKET_SHUTDOWN_WRITE;

    AutoLock lock(&lock_);
    zx_signals_t clear_mask = 0u;
    zx_signals_t set_mask = 0u;
    if (shutdown_read) {
        // If the other end shut down reading, we can't write any more.
        clear_mask |= ZX_SOCKET_WRITABLE;
        set_mask |= ZX_SOCKET_WRITE_DISABLED;
    }
    if (shutdown_write) {
        // If the other end shut down writing, we can't read any more than already exists in the
        // buffer. If we're empty, set ZX_SOCKET_READ_DISABLED now. If we aren't empty, Read() will
        // set this bit after reading the remaining data from the socket.
        read_disabled_ = true;
        if (is_empty())
            set_mask |= ZX_SOCKET_READ_DISABLED;
    }

    UpdateState(clear_mask, set_mask);
    return ZX_OK;
}

zx_status_t SocketDispatcher::Write(user_in_ptr<const void> src, size_t len,
                                    size_t* nwritten) {
    canary_.Assert();

    LTRACE_ENTRY;

    fbl::RefPtr<SocketDispatcher> other;
    {
        AutoLock lock(&lock_);
        if (!other_)
            return ZX_ERR_PEER_CLOSED;
        zx_signals_t signals = GetSignalsState();
        if (signals & ZX_SOCKET_WRITE_DISABLED)
            return ZX_ERR_BAD_STATE;
        other = other_;
    }

    if (len == 0) {
        *nwritten = 0;
        return ZX_OK;
    }
    if (len != static_cast<size_t>(static_cast<uint32_t>(len)))
        return ZX_ERR_INVALID_ARGS;

    return other->WriteSelf(src, len, nwritten);
}

zx_status_t SocketDispatcher::WriteControl(user_in_ptr<const void> src, size_t len) {
    canary_.Assert();

    if ((flags_ & ZX_SOCKET_HAS_CONTROL) == 0)
        return ZX_ERR_BAD_STATE;

    if (len == 0)
        return ZX_ERR_INVALID_ARGS;

    if (len > kControlMsgSize)
        return ZX_ERR_OUT_OF_RANGE;

    fbl::RefPtr<SocketDispatcher> other;
    {
        AutoLock lock(&lock_);
        if (!other_)
            return ZX_ERR_PEER_CLOSED;
        other = other_;
    }

    return other->WriteControlSelf(src, len);
}

zx_status_t SocketDispatcher::WriteControlSelf(user_in_ptr<const void> src,
                                               size_t len) {
    canary_.Assert();

    AutoLock lock(&lock_);

    if (control_msg_len_ != 0)
        return ZX_ERR_SHOULD_WAIT;

    if (src.copy_array_from_user(control_msg_.get(), len) != ZX_OK)
        return ZX_ERR_INVALID_ARGS; // Bad user buffer.

    control_msg_len_ = static_cast<uint32_t>(len);

    UpdateState(0u, ZX_SOCKET_CONTROL_READABLE);
    if (other_)
        other_->UpdateState(ZX_SOCKET_CONTROL_WRITABLE, 0u);

    return ZX_OK;
}

zx_status_t SocketDispatcher::WriteSelf(user_in_ptr<const void> src, size_t len,
                                        size_t* written) {
    canary_.Assert();

    AutoLock lock(&lock_);

    if (is_full())
        return ZX_ERR_SHOULD_WAIT;

    bool was_empty = is_empty();

    size_t st = 0u;
    zx_status_t status;
    if (flags_ & ZX_SOCKET_DATAGRAM) {
        status = data_.WriteDatagram(src, len, &st);
    } else {
        status = data_.WriteStream(src, len, &st);
    }
    if (status)
        return status;

    if (st > 0) {
        if (was_empty)
            UpdateState(0u, ZX_SOCKET_READABLE);
    }

    if (other_ && is_full())
        other_->UpdateState(ZX_SOCKET_WRITABLE, 0u);

    *written = st;
    return status;
}

zx_status_t SocketDispatcher::Read(user_out_ptr<void> dst, size_t len,
                                   size_t* nread) {
    canary_.Assert();

    LTRACE_ENTRY;

    AutoLock lock(&lock_);

    // Just query for bytes outstanding.
    if (!dst && len == 0) {
        *nread = data_.size();
        return ZX_OK;
    }

    if (len != (size_t)((uint32_t)len))
        return ZX_ERR_INVALID_ARGS;

    if (is_empty()) {
        if (!other_)
            return ZX_ERR_PEER_CLOSED;
        // If reading is disabled on our end and we're empty, we'll never become readable again.
        // Return a different error to let the caller know.
        if (read_disabled_)
            return ZX_ERR_BAD_STATE;
        return ZX_ERR_SHOULD_WAIT;
    }

    bool was_full = is_full();

    auto st = data_.Read(dst, len, flags_ & ZX_SOCKET_DATAGRAM);

    if (is_empty()) {
        uint32_t set_mask = 0u;
        if (read_disabled_)
            set_mask |= ZX_SOCKET_READ_DISABLED;
        UpdateState(ZX_SOCKET_READABLE, set_mask);
    }

    if (other_ && was_full && (st > 0))
        other_->UpdateState(0u, ZX_SOCKET_WRITABLE);

    *nread = static_cast<size_t>(st);
    return ZX_OK;
}

zx_status_t SocketDispatcher::ReadControl(user_out_ptr<void> dst, size_t len,
                                          size_t* nread) {
    canary_.Assert();

    if ((flags_ & ZX_SOCKET_HAS_CONTROL) == 0) {
        return ZX_ERR_BAD_STATE;
    }

    AutoLock lock(&lock_);

    if (control_msg_len_ == 0)
        return ZX_ERR_SHOULD_WAIT;

    size_t copy_len = MIN(control_msg_len_, len);
    if (dst.copy_array_to_user(control_msg_.get(), copy_len) != ZX_OK)
        return ZX_ERR_INVALID_ARGS; // Invalid user buffer.

    control_msg_len_ = 0;
    UpdateState(ZX_SOCKET_CONTROL_READABLE, 0u);
    if (other_)
        other_->UpdateState(0u, ZX_SOCKET_CONTROL_WRITABLE);

    *nread = copy_len;
    return ZX_OK;
}

zx_status_t SocketDispatcher::CheckShareable(SocketDispatcher* to_send) {
    // We disallow sharing of sockets that support sharing themselves
    // and disallow sharing either end of the socket we're going to
    // share on, thus preventing loops, etc.
    AutoLock lock(&lock_);
    if ((to_send->flags_ & ZX_SOCKET_HAS_ACCEPT) ||
        (to_send == this) || (to_send == other_.get()))
        return ZX_ERR_BAD_STATE;
    return ZX_OK;
}

zx_status_t SocketDispatcher::Share(Handle* h) {
    canary_.Assert();

    LTRACE_ENTRY;

    if (!(flags_ & ZX_SOCKET_HAS_ACCEPT))
        return ZX_ERR_NOT_SUPPORTED;

    fbl::RefPtr<SocketDispatcher> other;
    {
        AutoLock lock(&lock_);
        if (!other_)
            return ZX_ERR_PEER_CLOSED;
        other = other_;
    }

    return other->ShareSelf(h);
}

zx_status_t SocketDispatcher::ShareSelf(Handle* h) {
    canary_.Assert();

    fbl::RefPtr<SocketDispatcher> other;
    {
        AutoLock lock(&lock_);
        if (accept_queue_)
            return ZX_ERR_SHOULD_WAIT;

        accept_queue_.reset(h);

        UpdateState(0, ZX_SOCKET_ACCEPT);
        other = other_;
    }
    if (other)
        other->UpdateState(ZX_SOCKET_SHARE, 0);

    return ZX_OK;
}

zx_status_t SocketDispatcher::Accept(Handle** h) {
    canary_.Assert();

    if (!(flags_ & ZX_SOCKET_HAS_ACCEPT))
        return ZX_ERR_NOT_SUPPORTED;

    AutoLock lock(&lock_);

    if (!accept_queue_)
        return ZX_ERR_SHOULD_WAIT;

    *h = accept_queue_.release();

    UpdateState(ZX_SOCKET_ACCEPT, 0);
    if (other_)
        other_->UpdateState(0, ZX_SOCKET_SHARE);

    return ZX_OK;
}
