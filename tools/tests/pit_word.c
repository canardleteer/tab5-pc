// Host regression for the production word-input adapter, using tiny386's pinned PIT model.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "i8254.h"

typedef uint8_t u8;
typedef uint16_t u16;

static uint32_t now_us;
uint32_t get_uticks(void) { return now_us; }
static void irq(void* opaque, int number, int level) {
    (void)opaque;
    (void)number;
    (void)level;
}

typedef struct {
    PITState* pit;
    unsigned byte_reads, word_reads;
    int ports[64], word_port;
} TestPC;

static u8 pc_io_read(void* opaque, int port) {
    TestPC* pc = opaque;
    assert(pc->byte_reads < sizeof(pc->ports) / sizeof(pc->ports[0]));
    pc->ports[pc->byte_reads++] = port;
    return (u8)i8254_ioport_read(pc->pit, (uint32_t)port);
}

static u16 pc_io_read16(void* opaque, int port) {
    TestPC* pc = opaque;
    ++pc->word_reads;
    pc->word_port = port;
    return port == 0x40 ? 0xffff : 0xbeef;
}

#include "t386_pit_word.h"

static void counter(PITState* pit, int channel, int value, int word) {
    i8254_ioport_write(pit, 0x43, (unsigned)(channel << 6) | (word ? 0x34 : 0x14));
    i8254_ioport_write(pit, 0x40 + (unsigned)channel, (unsigned)value & 0xff);
    if (word) i8254_ioport_write(pit, 0x40 + (unsigned)channel, (unsigned)value >> 8);
}

static void test_byte_order_and_dispatch(void) {
    TestPC pc = {0};
    now_us = 0;
    pc.pit = i8254_init(0, NULL, irq);
    assert(pc.pit);
    counter(pc.pit, 0, 0x34, 0);
    counter(pc.pit, 1, 0xab, 0);
    assert(t386_pit_word_read(&pc, 0x40) == 0xab34);
    assert(pc.byte_reads == 2 && pc.ports[0] == 0x40 && pc.ports[1] == 0x41);
    assert(pc.word_reads == 0);

    // Ordinary byte input still goes through the original timer model.
    assert(pc_io_read(&pc, 0x40) == 0x34);
    assert(pc_io_read(&pc, 0x41) == 0xab);
    const unsigned before = pc.byte_reads;
    const int other_ports[] = {0x1f0, 0x170, 0x1ce, 0xcf8, 0xcfc, 0x310, 0x220, 0x41};
    for (unsigned i = 0; i < sizeof(other_ports) / sizeof(other_ports[0]); ++i) {
        assert(t386_pit_word_read(&pc, other_ports[i]) == 0xbeef);
        assert(pc.word_reads == i + 1 && pc.word_port == other_ports[i]);
        assert(pc.byte_reads == before);
    }
    free(pc.pit);
}

static void test_counter_polling(void) {
    TestPC pc = {0};
    now_us = 0;
    pc.pit = i8254_init(0, NULL, irq);
    assert(pc.pit);
    counter(pc.pit, 0, 0xffff, 1);
    counter(pc.pit, 1, 0x8000, 1);

    // The unsupported word handler cannot complete a wait for a changed counter.
    for (int i = 0; i < 16; ++i) {
        now_us += 100;
        assert(pc_io_read16(&pc, 0x40) == 0xffff);
    }
    pc.word_reads = 0;
    u16 previous = t386_pit_word_read(&pc, 0x40);
    unsigned changed = 0;
    for (int i = 0; i < 16; ++i) {
        now_us += 100;
        const u16 value = t386_pit_word_read(&pc, 0x40);
        changed += value != previous;
        previous = value;
    }
    assert(changed && pc.word_reads == 0);
    for (unsigned i = 0; i < pc.byte_reads; i += 2)
        assert(pc.ports[i] == 0x40 && pc.ports[i + 1] == 0x41);
    free(pc.pit);
}

int main(void) {
    test_byte_order_and_dispatch();
    test_counter_polling();
    puts("PIT word input: byte order, changing counters and unrelated word-port dispatch passed");
    return 0;
}
