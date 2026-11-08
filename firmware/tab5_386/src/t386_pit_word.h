// Word input for the PIT's byte-wide ports. Included after tiny386's pc.c, which supplies the byte and word handlers.
#pragma once

enum { T386_PIT_COUNTER0_PORT = 0x40 };

static u16 t386_pit_word_read(void* opaque, int port) {
    if (port == T386_PIT_COUNTER0_PORT) {
        const u16 low = pc_io_read(opaque, port);
        const u16 high = pc_io_read(opaque, port + 1);
        return low | (high << 8);
    }
    return pc_io_read16(opaque, port);
}
