// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/platform-defs.h>

#include <zircon/assert.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>

#include "vim.h"
#include "vim-hw.h"

static zx_status_t vim_get_initial_mode(void* ctx, usb_mode_t* out_mode) {
    *out_mode = USB_MODE_HOST;
    return ZX_OK;
}

static zx_status_t vim_set_mode(void* ctx, usb_mode_t mode) {
    vim_bus_t* bus = ctx;
    return vim_usb_set_mode(bus, mode);
}

usb_mode_switch_protocol_ops_t usb_mode_switch_ops = {
    .get_initial_mode = vim_get_initial_mode,
    .set_mode = vim_set_mode,
};

static zx_status_t vim_bus_get_protocol(void* ctx, uint32_t proto_id, void* out) {
    vim_bus_t* bus = ctx;

    switch (proto_id) {
    case ZX_PROTOCOL_USB_MODE_SWITCH:
        memcpy(out, &bus->usb_mode_switch, sizeof(bus->usb_mode_switch));
        return ZX_OK;
    case ZX_PROTOCOL_GPIO:
        memcpy(out, &bus->gpio.proto, sizeof(bus->gpio.proto));
        return ZX_OK;
    case ZX_PROTOCOL_I2C:
        memcpy(out, &bus->i2c.proto, sizeof(bus->i2c.proto));
        return ZX_OK;
    default:
        return ZX_ERR_NOT_SUPPORTED;
    }
}

static pbus_interface_ops_t vim_bus_ops = {
    .get_protocol = vim_bus_get_protocol,
};

static void vim_bus_release(void* ctx) {
    vim_bus_t* bus = ctx;

    a113_gpio_release(&bus->gpio);
    free(bus);
}

static zx_protocol_device_t vim_bus_device_protocol = {
    .version = DEVICE_OPS_VERSION,
    .release = vim_bus_release,
};

static zx_status_t vim_bus_bind(void* ctx, zx_device_t* parent) {
    zx_status_t status;

    vim_bus_t* bus = calloc(1, sizeof(vim_bus_t));
    if (!bus) {
        return ZX_ERR_NO_MEMORY;
    }

    if ((status = device_get_protocol(parent, ZX_PROTOCOL_PLATFORM_BUS, &bus->pbus)) != ZX_OK) {
        goto fail;
    }

/*
    if ((status = a113_gpio_init(&bus->gpio)) != ZX_OK) {
        zxlogf(ERROR, "a113_gpio_init failed: %d\n", status);
        goto fail;
    }

    if ((status = a113_i2c_init(&bus->i2c)) != ZX_OK) {
        zxlogf(ERROR, "a113_i2c_init failed: %d\n", status);
        goto fail;
    }
*/

    bus->usb_mode_switch.ops = &usb_mode_switch_ops;
    bus->usb_mode_switch.ctx = bus;

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "vim-bus",
        .ctx = bus,
        .ops = &vim_bus_device_protocol,
        .flags = DEVICE_ADD_NON_BINDABLE,
    };

    status = device_add(parent, &args, NULL);
    if (status != ZX_OK) {
        goto fail;
    }

    pbus_interface_t intf;
    intf.ops = &vim_bus_ops;
    intf.ctx = bus;
    pbus_set_interface(&bus->pbus, &intf);

printf("call vim_usb_init\n");
    if ((status = vim_usb_init(bus)) != ZX_OK) {
        zxlogf(ERROR, "vim_usb_init failed: %d\n", status);
    }

    return ZX_OK;

fail:
    printf("vim_bus_bind failed %d\n", status);
    vim_bus_release(bus);
    return status;
}

static zx_driver_ops_t vim_bus_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = vim_bus_bind,
};

ZIRCON_DRIVER_BEGIN(vim_bus, vim_bus_driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PLATFORM_BUS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_KHADAS),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_VIM),
ZIRCON_DRIVER_END(vim_bus)
