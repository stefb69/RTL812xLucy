/* SPDX-License-Identifier: GPL-2.0-only */
#include "../RTL8127Dext/RTL8127Dext/RTL8127TxBuffer.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main()
{
    // A complete Ethernet frame, excluding FCS. Distinct padding reveals a
    // descriptor that starts at the allocation rather than at the frame.
    const uint8_t ethernet[60] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
        0x88, 0xc9, 0xb3, 0xbc, 0x52, 0xc0, 0x08, 0x00, 0x45
    };
    uint8_t buffer[128];
    const uint64_t dmaBase = 0x123400000ULL;
    RTL8127TxBufferView view;
    const uint32_t offsets[] = {0, 2, 8, 18};
    for (uint32_t offset : offsets) {
        memset(buffer, 0xa5, sizeof(buffer));
        memcpy(buffer + offset, ethernet, sizeof(ethernet));
        assert(rtl8127TxBufferView(reinterpret_cast<uintptr_t>(buffer), dmaBase,
            offset, sizeof(ethernet), sizeof(buffer), view));
        assert(memcmp(view.frame, ethernet, sizeof(ethernet)) == 0);
        assert(view.iova == dmaBase + offset);
        // Simulate the NIC reading the descriptor's IOVA and byte count.
        assert(memcmp(buffer + (view.iova - dmaBase), ethernet, sizeof(ethernet)) == 0);
        assert(view.frame[12] == 0x08 && view.frame[13] == 0x00);
        // Offload header writes must touch that same DMA-visible frame.
        view.frame[14] = 0x46;
        assert(buffer[(view.iova - dmaBase) + 14] == 0x46);
        for (uint32_t i = 0; i < offset; ++i) assert(buffer[i] == 0xa5);
    }

    const uint64_t va = reinterpret_cast<uintptr_t>(buffer);
    assert(rtl8127TxBufferView(va, dmaBase, 2, 126, 128, view));
    assert(!rtl8127TxBufferView(va, dmaBase, 2, 127, 128, view));
    assert(view.frame == nullptr && view.iova == 0);
    assert(!rtl8127TxBufferView(va, dmaBase, 129, 1, 128, view));
    assert(!rtl8127TxBufferView(va, dmaBase, UINT32_MAX, 2, 128, view));
    assert(!rtl8127TxBufferView(va, dmaBase, SIZE_MAX, 2, 128, view));
    assert(!rtl8127TxBufferView(va, dmaBase, 0, UINT32_MAX, 128, view));
    assert(!rtl8127TxBufferView(va, dmaBase, 0, 0, 128, view));
    assert(!rtl8127TxBufferView(0, dmaBase, 2, 60, 128, view));
    assert(!rtl8127TxBufferView(UINT64_MAX, dmaBase, 2, 60, 128, view));
    assert(!rtl8127TxBufferView(va, UINT64_MAX, 2, 60, 128, view));
    puts("TX buffer tests passed: BSD/native offsets, DMA bytes, offload writes, bounds");
}
