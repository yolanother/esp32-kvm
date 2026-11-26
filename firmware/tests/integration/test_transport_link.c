/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Verifies the byte-link write loop the USB worker uses for every response:
 * partial writes continue, a try that moves no byte counts against the
 * budget, an error gives up at once, and the given wait reaches the link. */
#include "transport_link.h"
#include <assert.h>
#include <string.h>

typedef struct {
    uint8_t sink[256];
    size_t sink_length;
    size_t chunk;          /* bytes accepted per call (0 = refuse) */
    int stall_after;       /* calls that accept bytes before refusing; -1 = never stall */
    int error_at;          /* call index returning -1; -1 = none */
    int calls;
    uint32_t last_wait;
    size_t last_len;
} fake_link_t;

static bool fake_connected(void *context) { (void)context; return true; }
static int fake_read(void *context, uint8_t *buf, size_t cap, uint32_t wait_ticks)
{ (void)context; (void)buf; (void)cap; (void)wait_ticks; return 0; }
static int fake_write(void *context, const uint8_t *buf, size_t len, uint32_t wait_ticks)
{
    fake_link_t *fake = context;
    int call = fake->calls++;
    fake->last_wait = wait_ticks;
    fake->last_len = len;
    assert(len > 0); /* write_all never asks a link for zero bytes */
    if (call == fake->error_at) return -1;
    if (fake->stall_after >= 0 && call >= fake->stall_after) return 0;
    size_t take = fake->chunk < len ? fake->chunk : len;
    memcpy(fake->sink + fake->sink_length, buf, take);
    fake->sink_length += take;
    return (int)take;
}

static int liar_write(void *context, const uint8_t *buf, size_t len, uint32_t wait_ticks)
{
    fake_link_t *fake = context;
    (void)buf; (void)wait_ticks;
    fake->calls++;
    return (int)len + 1;
}

static kvm_transport_link_t make_link(fake_link_t *fake, size_t chunk, int stall_after, int error_at)
{
    memset(fake, 0, sizeof(*fake));
    fake->chunk = chunk;
    fake->stall_after = stall_after;
    fake->error_at = error_at;
    return (kvm_transport_link_t){fake_connected, fake_read, fake_write, fake};
}

int main(void)
{
    uint8_t frame[200];
    for (size_t i = 0; i < sizeof(frame); ++i) frame[i] = (uint8_t)(i * 7u + 1u);
    fake_link_t fake;
    kvm_transport_link_t link;

    /* Whole-frame link (USB Serial/JTAG accepts all or nothing): one call. */
    link = make_link(&fake, sizeof(frame), -1, -1);
    assert(kvm_transport_link_write_all(&link, frame, sizeof(frame), 5, 1));
    assert(fake.calls == 1 && fake.last_wait == 5 && fake.last_len == sizeof(frame));
    assert(fake.sink_length == sizeof(frame) && memcmp(fake.sink, frame, sizeof(frame)) == 0);

    /* Partial writes of every size 1..n: each try that moves bytes continues,
       even with max_tries 1, and the remainder is offered from the right byte. */
    for (size_t chunk = 1; chunk <= sizeof(frame); ++chunk) {
        link = make_link(&fake, chunk, -1, -1);
        assert(kvm_transport_link_write_all(&link, frame, sizeof(frame), 5, 1));
        assert(fake.sink_length == sizeof(frame) && memcmp(fake.sink, frame, sizeof(frame)) == 0);
        assert(fake.calls == (int)((sizeof(frame) + chunk - 1) / chunk));
    }

    /* A link that never accepts a byte: false after exactly max_tries tries. */
    for (uint32_t tries = 1; tries <= 5; ++tries) {
        link = make_link(&fake, 8, 0, -1);
        assert(!kvm_transport_link_write_all(&link, frame, sizeof(frame), 1, tries));
        assert(fake.calls == (int)tries && fake.sink_length == 0 && fake.last_wait == 1);
    }

    /* Progress, then a stall: the stalled tries alone spend the budget. */
    link = make_link(&fake, 10, 3, -1);
    assert(!kvm_transport_link_write_all(&link, frame, sizeof(frame), 1, 4));
    assert(fake.calls == 3 + 4 && fake.sink_length == 30);

    /* An error gives up at once, whatever budget is left. */
    link = make_link(&fake, 8, -1, 0);
    assert(!kvm_transport_link_write_all(&link, frame, sizeof(frame), 5, 100));
    assert(fake.calls == 1 && fake.sink_length == 0);
    link = make_link(&fake, 8, -1, 2);
    assert(!kvm_transport_link_write_all(&link, frame, sizeof(frame), 5, 100));
    assert(fake.calls == 3 && fake.sink_length == 16);

    /* Zero length is done without calling the link (bytes may be NULL). */
    link = make_link(&fake, 8, -1, -1);
    assert(kvm_transport_link_write_all(&link, frame, 0, 5, 1));
    assert(kvm_transport_link_write_all(&link, NULL, 0, 5, 1));
    assert(fake.calls == 0);

    /* Invalid arguments never reach the link and report failure. */
    assert(!kvm_transport_link_write_all(NULL, frame, sizeof(frame), 5, 1));
    assert(!kvm_transport_link_write_all(NULL, frame, 0, 5, 1));
    assert(!kvm_transport_link_write_all(&link, NULL, sizeof(frame), 5, 1));
    kvm_transport_link_t no_write = {fake_connected, fake_read, NULL, &fake};
    assert(!kvm_transport_link_write_all(&no_write, frame, sizeof(frame), 5, 1));
    assert(!kvm_transport_link_write_all(&link, frame, sizeof(frame), 5, 0)); /* no budget */
    assert(fake.calls == 0);

    /* A link claiming more than it was offered is a broken link: false. */
    kvm_transport_link_t liar = {fake_connected, fake_read, liar_write, &fake};
    fake.calls = 0;
    assert(!kvm_transport_link_write_all(&liar, frame, 4, 5, 3));
    assert(fake.calls == 1);
    return 0;
}
