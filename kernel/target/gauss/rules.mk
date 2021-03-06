# Copyright 2017 The Fuchsia Authors
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT

LOCAL_DIR := $(GET_LOCAL_DIR)

PLATFORM := generic-arm

DEVICE_TREE := $(GET_LOCAL_DIR)/device-tree.dtb

MEMBASE := 0x00000000
MEMSIZE ?= 0x40000000   # 1GB
KERNEL_LOAD_OFFSET := 0x1080000

PERIPH_BASE_PHYS := 0xf9800000UL
PERIPH_SIZE := 0x06800000UL
PERIPH_BASE_VIRT := 0xfffffffff9800000ULL
MEMORY_APERTURE_SIZE  := 0x40000000UL

KERNEL_DEFINES += \
    PERIPH_BASE_PHYS=$(PERIPH_BASE_PHYS) \
    PERIPH_BASE_VIRT=$(PERIPH_BASE_VIRT) \
    PERIPH_SIZE=$(PERIPH_SIZE) \
    MEMORY_APERTURE_SIZE=$(MEMORY_APERTURE_SIZE) \
    PLATFORM_SUPPORTS_PANIC_SHELL=1 \

include make/kernel-images.mk

PLATFORM_VID := 3   # PDEV_VID_GOOGLE
PLATFORM_PID := 1   # PDEV_PID_GAUSS
PLATFORM_BOARD_NAME := gauss

# build MDI
MDI_SRCS := \
    $(LOCAL_DIR)/gauss.mdi \
