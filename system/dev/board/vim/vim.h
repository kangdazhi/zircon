// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/io-buffer.h>
#include <ddk/protocol/platform-bus.h>
#include <ddk/protocol/usb-mode-switch.h>
#include <soc/aml-a113/a113-gpio.h>
#include <soc/aml-a113/a113-i2c.h>

typedef struct {
    platform_bus_protocol_t pbus;
    a113_gpio_t gpio;
    a113_i2c_t i2c;
    usb_mode_switch_protocol_t usb_mode_switch;
    io_buffer_t usb_phy;
    zx_handle_t usb_phy_irq_handle;
    thrd_t phy_irq_thread;
} vim_bus_t;

// vim-usb.c
zx_status_t vim_usb_init(vim_bus_t* bus);
zx_status_t vim_usb_set_mode(vim_bus_t* bus, usb_mode_t mode);
