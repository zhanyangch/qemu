/*
 * QEMU RISC-V IOPMP DMA
 *
 * A simple device to send transaction information to IOPMP when DMA operation
 *
 * Copyright (c) 2023-2025 Andes Tech. Corp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef IOPMP_DMA_H
#define IOPMP_DMA_H

#include "hw/sysbus.h"
#include "qom/object.h"
#include "hw/misc/riscv_iopmp.h"
#include "hw/stream.h"
#define TYPE_IOPMPDMA "iopmpdma"
OBJECT_DECLARE_SIMPLE_TYPE(IOPMPDMAState, IOPMPDMA)

#define IOPMPDMA_STATUS_COMPLETE 1
#define IOPMPDMA_STATUS_ERROR    2

typedef struct IOPMPDMAState {
    /*< private >*/
    SysBusDevice busdev;
    /*< public >*/
    uint32_t rrid;

    qemu_irq irq;
    MemoryRegion mmio;
    uint32_t src_reg;
    uint32_t dst_reg;
    uint32_t sz_reg;
    /* A thread is waiting for start flag */
    volatile uint32_t start_reg;
    uint32_t status_reg;

    StreamSink *target_sink;

    QemuThread thread;
} IOPMPDMAState;

void iopmpdma_setup_sink(DeviceState *dev, StreamSink *sink);
DeviceState *iopmpdma_create(hwaddr addr, qemu_irq irq);

#endif
