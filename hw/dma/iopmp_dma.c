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

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/dma/iopmp_dma.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "include/exec/address-spaces.h"
#include "qemu/main-loop.h"
#include "hw/misc/riscv_iopmp_txn_info.h"

#define IOPMPDMA_SRCREG     0x0
#define IOPMPDMA_DSTREG     0x4
#define IOPMPDMA_SZREG      0x8
#define IOPMPDMA_STARTREG   0xc
#define IOPMPDMA_STATUSREG  0x10

#define LOGGE(x...) qemu_log_mask(LOG_GUEST_ERROR, x)
#define xLOG(x...)
#define yLOG(x...) qemu_log(x)
#ifdef DEBUG_IOPMPDMA
  #define LOG(x...) yLOG(x)
#else
  #define LOG(x...) xLOG(x)
#endif

static uint64_t iopmp_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    IOPMPDMAState *s = opaque;
    uint64_t result = 0;

    switch (offset) {
    case IOPMPDMA_SRCREG:
        result = s->src_reg;
        break;
    case IOPMPDMA_DSTREG:
        result = s->dst_reg;
        break;
    case IOPMPDMA_SZREG:
        result = s->sz_reg;
        break;
    case IOPMPDMA_STARTREG:
        result = s->start_reg;
        break;
    case IOPMPDMA_STATUSREG:
        result = s->status_reg;
        break;
    default:
        LOGGE("%s: Bad offset 0x%" HWADDR_PRIX "\n",
              __func__, offset);
        break;
    }
    LOG("### iopmp_dma_read()=0x%lx, val=0x%lx\n", offset, result);
    return result;
}

static void iopmp_dma_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IOPMPDMAState *s = opaque;
    switch (offset) {
    case IOPMPDMA_SRCREG:
        s->src_reg = value;
        break;
    case IOPMPDMA_DSTREG:
        s->dst_reg = value;
        break;
    case IOPMPDMA_SZREG:
        s->sz_reg = value;
        break;
    case IOPMPDMA_STARTREG:
        s->start_reg = value;
        break;
    case IOPMPDMA_STATUSREG:
        /* clear */
        s->status_reg = 0;
        break;
    default:
        LOGGE("%s: Bad offset 0x%" HWADDR_PRIX "\n",
              __func__, offset);
        break;
    }
}

static void dma_txn_info_push(StreamSink *sink, uint8_t *buf, bool eop)
{
    if (sink == NULL) {
        /* Do nothing if streamsink is not connected */
        return;
    }
    if (eop) {
        while (stream_push(sink, buf, sizeof(riscv_iopmp_txn_info),
               true) == 0) {
            ;
        }
    } else {
        while (stream_push(sink, buf, sizeof(riscv_iopmp_txn_info),
               false) == 0) {
            ;
        }
    }
}

static void *iopmp_dma_thread_run(void *opaque)
{
    uint8_t buf[1024 * 32];
    IOPMPDMAState *s = opaque;
    riscv_iopmp_txn_info info;
    MemTxAttrs attrs;
    attrs.requester_id = s->rrid;

    while (1) {
        while (s->start_reg == 1) {
            bql_lock();
            if (s->start_reg != 1) {
                bql_unlock();
            } else if (s->target_sink) {
                info.rrid = s->rrid;
                info.start_addr = s->src_reg;
                info.end_addr = info.start_addr + s->sz_reg;
                info.stage = 0;
                dma_txn_info_push(s->target_sink, (uint8_t *)&info, 0);
                if (address_space_rw(&address_space_memory, s->src_reg,
                                     attrs, buf, s->sz_reg, 0) == MEMTX_OK) {
                    dma_txn_info_push(s->target_sink, (uint8_t *)&info, 1);
                    info.start_addr = s->dst_reg;
                    info.end_addr = info.start_addr + s->sz_reg;
                    dma_txn_info_push(s->target_sink, (uint8_t *)&info, 0);
                    if (address_space_rw(&address_space_memory, s->dst_reg,
                                         attrs, buf, s->sz_reg, 1) ==
                                         MEMTX_OK) {
                        dma_txn_info_push(s->target_sink, (uint8_t *)&info, 1);
                        s->status_reg = IOPMPDMA_STATUS_COMPLETE;
                        qemu_irq_raise(s->irq);
                        s->start_reg = 0;
                        bql_unlock();
                        break;
                    }
                }
                s->status_reg = IOPMPDMA_STATUS_ERROR;
                qemu_irq_raise(s->irq);
                s->start_reg = 0;
                bql_unlock();
            }
        }
    }
    return NULL;
}

static const MemoryRegionOps iopmp_dma_ops = {
    .read = iopmp_dma_read,
    .write = iopmp_dma_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8
    }
};

static void iopmp_dma_realize(DeviceState *dev, Error **errp)
{
    IOPMPDMAState *s = IOPMPDMA(dev);
    SysBusDevice *sbus = SYS_BUS_DEVICE(dev);
    memory_region_init_io(&s->mmio, OBJECT(dev), &iopmp_dma_ops, s,
                          TYPE_IOPMPDMA, 0x100);
    sysbus_init_mmio(sbus, &s->mmio);
    qemu_thread_create(&s->thread, "iopmpdma_thread", iopmp_dma_thread_run,
                       s, QEMU_THREAD_JOINABLE);
}

static const Property dma_property[] = {
    DEFINE_PROP_UINT32("rrid", IOPMPDMAState, rrid, 1),
};

static void iopmp_dma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *k = DEVICE_CLASS(klass);
    device_class_set_props(k, dma_property);
    k->realize = iopmp_dma_realize;
}

static void iopmp_dma_init(Object *obj)
{
    IOPMPDMAState *s = IOPMPDMA(obj);
    SysBusDevice *sbus = SYS_BUS_DEVICE(obj);

    sysbus_init_irq(sbus, &s->irq);
}

static const TypeInfo iopmp_dma_info = {
    .name          = TYPE_IOPMPDMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IOPMPDMAState),
    .class_init    = iopmp_dma_class_init,
    .instance_init = iopmp_dma_init,
};

static void iopmp_dma_register_types(void)
{
    type_register_static(&iopmp_dma_info);
}

DeviceState *iopmpdma_create(hwaddr addr, qemu_irq irq)
{
    DeviceState *dev;
    dev = qdev_new(TYPE_IOPMPDMA);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    return dev;
}

void iopmpdma_setup_sink(DeviceState *dev, StreamSink * ss)
{
    IOPMPDMAState *s = IOPMPDMA(dev);
    s->target_sink = ss;
}

type_init(iopmp_dma_register_types)
