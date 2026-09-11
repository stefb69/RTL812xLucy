/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8127_TX_BUFFER_H
#define RTL8127_TX_BUFFER_H

#include <stdint.h>
#include <stddef.h>

struct RTL8127TxBufferView {
    uint8_t *frame;
    uint64_t iova;
};

/* NDK returns buffer addresses separately from the packet's data offset.
 * Resolve both addresses together so parsing and DMA use the same bytes. */
static inline bool rtl8127TxBufferView(uint64_t baseVA, uint64_t baseIOVA,
                                      size_t offset, uint32_t length,
                                      uint32_t capacity, RTL8127TxBufferView &view)
{
    view = {};
    if (!baseVA || !length || offset > capacity || length > capacity - offset ||
        baseVA > UINT64_MAX - offset || baseIOVA > UINT64_MAX - offset)
        return false;

    view.frame = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(baseVA + offset));
    view.iova = baseIOVA + offset;
    return true;
}

#endif
