/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Tests the firmware's binary-only USB handshake core with independent golden
 * protocol frames, without requiring a board or ESP-IDF runtime. */
#include "transport_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char sent[1024];
static size_t sent_len;
static unsigned sends;

/* Decodes one captured frame independently of the transport implementation. */
static size_t decode_sent(unsigned char *raw)
{
    size_t source = 0, target = 0;
    assert(sent_len > 0 && sent[sent_len - 1] == 0);
    while (source < sent_len - 1) {
        unsigned code = sent[source++];
        assert(code != 0 && source + code - 1 <= sent_len - 1);
        for (unsigned i = 1; i < code; i++) raw[target++] = sent[source++];
        if (code != 0xff && source < sent_len - 1) raw[target++] = 0;
    }
    return target;
}

static void capture(void *context, const unsigned char *bytes, size_t length)
{
    (void)context;
    assert(length <= sizeof(sent));
    memcpy(sent, bytes, length);
    sent_len = length;
    sends++;
}

static size_t read_fixture(const char *path, unsigned char *bytes, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    size_t length;
    assert(file != NULL);
    length = fread(bytes, 1, capacity, file);
    assert(!ferror(file));
    assert(feof(file));
    fclose(file);
    return length;
}

int main(int argc, char **argv)
{
    kvm_transport_core_t core;
    unsigned char bytes[1024];
    size_t length;
    assert(argc == 4);
    assert(kvm_transport_core_init(&core, "b", "1", 5, capture, NULL));

    length = read_fixture(argv[1], bytes, sizeof(bytes));
    kvm_transport_core_feed(&core, bytes, length / 2);
    assert(sends == 0);
    kvm_transport_core_feed(&core, bytes + length / 2, length - length / 2);
    assert(sends == 1 && sent_len > 5 && sent[4] == KVM_MSG_CAPS);

    length = read_fixture(argv[2], bytes, sizeof(bytes));
    kvm_transport_core_feed(&core, bytes, length);
    assert(sends == 2 && sent[4] == KVM_MSG_ACK);
    assert(kvm_transport_core_session_open(&core));

    length = read_fixture(argv[3], bytes, sizeof(bytes));
    kvm_transport_core_feed(&core, bytes, length);
    assert(sends == 3 && sent[4] == KVM_MSG_STATUS);

    assert(kvm_transport_core_device_select_request(&core, 1));
    assert(sends == 4 && sent[4] == KVM_MSG_DEVICE_SELECT_REQUEST);
    length = decode_sent(bytes);
    assert(length == KVM_PROTOCOL_HEADER_LEN + 5 + 4);
    assert(bytes[4] == 5 && bytes[5] == 0);
    assert(memcmp(bytes + KVM_PROTOCOL_HEADER_LEN, "\x01\x01\x00\x00\x00", 5) == 0);

    bytes[length - 2] ^= 1; /* Break the golden frame's CRC. */
    kvm_transport_core_feed(&core, bytes, length);
    assert(sends == 4);
    kvm_transport_core_reset(&core);
    assert(!kvm_transport_core_session_open(&core));
    memset(bytes, 0x55, sizeof(bytes));
    bytes[sizeof(bytes) - 1] = 0;
    kvm_transport_core_feed(&core, bytes, sizeof(bytes));
    assert(sends == 4); /* Drain an oversized frame through its delimiter. */
    length = read_fixture(argv[1], bytes, sizeof(bytes));
    kvm_transport_core_feed(&core, bytes, length);
    assert(sends == 5 && sent[4] == KVM_MSG_CAPS);
    assert(!kvm_transport_core_session_open(&core));
    puts("transport core fixture tests passed");
    return 0;
}
