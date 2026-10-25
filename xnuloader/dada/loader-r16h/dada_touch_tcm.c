/* See sys/dada_touch.h. No controller firmware is uploaded or flashed here. */
#include <sys/dada_touch.h>

/* All slots, not only active slots: explicit lift reports cannot leave fingers down.
 * 4-bit slot, 4-bit classification, 16-bit X, 16-bit Y, LSB-first fields.
 * Use this decoder ONLY after SET_TOUCH_REPORT_CONFIG has returned STATUS_OK.
 */
const uint8_t dada_tcm_report_config[12] = {
    0x02, 0x06, 4, 0x07, 4, 0x08, 16, 0x09, 16, 0x03, 0x00, 0x00
};

static unsigned
le16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

int
dada_tcm_read_message(const struct dada_tcm_bus *bus, struct dada_tcm_message *msg)
{
    uint8_t header[4], tail[DADA_TCM_PAYLOAD + 3];
    unsigned length, i;
    int error;

    msg->length = 0;
    msg->code = 0xff;
    error = bus->read(bus->context, header, sizeof(header));
    if (error) return DADA_TCM_IO;
    /* TCM v2 has different framing and CRC. Never treat it as v1. */
    if (header[0] != 0xa5 || header[1] == 0x03 || header[1] == 0xff)
        return DADA_TCM_PROTOCOL;
    length = le16(header + 2);
    if (length > DADA_TCM_PAYLOAD) return DADA_TCM_SIZE;
    if (length != 0) {
        bus->delay_us(bus->context, 100);
        error = bus->read(bus->context, tail, length + 3);
        if (error) return DADA_TCM_IO;
        if (tail[0] != 0xa5 || tail[1] != 0x03 || tail[length + 2] != 0x5a)
            return DADA_TCM_PROTOCOL;
        for (i = 0; i < length; i++) msg->data[i] = tail[i + 2];
    }
    msg->code = header[1];
    msg->length = length;
    return 0;
}

int
dada_tcm_command(const struct dada_tcm_bus *bus, uint8_t command,
    const uint8_t *payload, unsigned length, struct dada_tcm_message *msg)
{
    uint8_t tx[131];
    unsigned i;
    int error;

    if (length > sizeof(tx) - 3 || (length && !payload)) return DADA_TCM_SIZE;
    tx[0] = command;
    tx[1] = (uint8_t)length;
    tx[2] = (uint8_t)(length >> 8);
    for (i = 0; i < length; i++) tx[i + 3] = payload[i];
    if (bus->write(bus->context, tx, length + 3)) return DADA_TCM_IO;
    for (i = 0; i < 100; i++) {
        bus->delay_us(bus->context, 5000);
        error = dada_tcm_read_message(bus, msg);
        if (error) return error;
        if (msg->code == DADA_TCM_IDENTIFY)
            return command == 0x02 ? 0 : DADA_TCM_RESTARTED;
        if (msg->code == DADA_TCM_OK) return 0;
        /* idle/no-report/ACK are transient. Touch reports may precede a reply. */
        if (msg->code == 0 || msg->code == 4 || msg->code == 7 || msg->code >= 0x10)
            continue;
        return DADA_TCM_REJECTED;
    }
    return DADA_TCM_TIMEOUT;
}

int
dada_tcm_identify(struct dada_tcm_state *state, const uint8_t *p, unsigned length)
{
    unsigned i;
    if (length < 24) return DADA_TCM_SIZE;
    state->version = p[0];
    state->mode = p[1];
    state->max_write = (uint16_t)le16(p + 22);
    for (i = 0; i < 16; i++) state->part[i] = p[i + 2] >= 32 && p[i + 2] < 127 ? p[i + 2] : '.';
    state->part[16] = 0;
    state->build_id = (uint32_t)p[18] | (uint32_t)p[19] << 8 |
        (uint32_t)p[20] << 16 | (uint32_t)p[21] << 24;
    return state->mode == 1 ? 0 : DADA_TCM_UNSUPPORTED;
}

int
dada_tcm_application(struct dada_tcm_state *state, const uint8_t *p, unsigned length)
{
    unsigned max_x, max_y, objects, config;
    if (length < 38) return DADA_TCM_SIZE;
    if (le16(p + 2) != 0) return DADA_TCM_UNSUPPORTED;
    config = le16(p + 12);
    max_x = le16(p + 32);
    max_y = le16(p + 34);
    objects = le16(p + 36);
    if (!max_x || !max_y || !objects || objects > DADA_TCM_CONTACTS ||
        config < sizeof(dada_tcm_report_config) || config > 128 ||
        le16(p + 14) < objects * 5 ||
        (state->max_write && state->max_write < config + 3)) return DADA_TCM_UNSUPPORTED;
    state->max_x = (uint16_t)max_x;
    state->max_y = (uint16_t)max_y;
    state->objects = (uint8_t)objects;
    state->max_config = (uint16_t)config;
    return 0;
}

static unsigned
normalized(unsigned value, unsigned max)
{
    return (value * 65535u) / max;
}

void
dada_tcm_release(struct dada_tcm_state *state, dada_touch_emit emit, void *context)
{
    unsigned i;
    for (i = 0; i < DADA_TCM_CONTACTS; i++) {
        if (state->contacts[i].down) {
            emit(context, 3, i, state->contacts[i].x, state->contacts[i].y);
            state->contacts[i].down = 0;
        }
    }
}

int
dada_tcm_frame(struct dada_tcm_state *state, const uint8_t *p, unsigned length,
    dada_touch_emit emit, void *context)
{
    struct dada_tcm_contact next[DADA_TCM_CONTACTS] = { { 0, 0, 0 } };
    unsigned seen = 0, i;

    if (!state->max_x || !state->max_y || !state->objects ||
        state->objects > DADA_TCM_CONTACTS || length != state->objects * 5u)
        return DADA_TCM_SIZE;
    /* Validate the entire frame before changing state or dispatching any sample. */
    for (i = 0; i < state->objects; i++) {
        unsigned id = p[5 * i] & 15, kind = p[5 * i] >> 4;
        unsigned x = le16(p + 5 * i + 1), y = le16(p + 5 * i + 3);
        if (id >= state->objects || (seen & (1u << id))) return DADA_TCM_PROTOCOL;
        seen |= 1u << id;
        /* Only contacts, never palms, hover, pen or unknown objects. */
        if (kind == 1 || kind == 2) {
            if (x > state->max_x || y > state->max_y) return DADA_TCM_PROTOCOL;
            next[id].down = 1;
            next[id].x = (uint16_t)normalized(x, state->max_x);
            next[id].y = (uint16_t)normalized(y, state->max_y);
        }
    }
    for (i = 0; i < DADA_TCM_CONTACTS; i++) {
        struct dada_tcm_contact *old = &state->contacts[i], *n = &next[i];
        if (n->down && !old->down) emit(context, 1, i, n->x, n->y);
        else if (n->down && (old->x != n->x || old->y != n->y)) emit(context, 2, i, n->x, n->y);
        else if (!n->down && old->down) emit(context, 3, i, old->x, old->y);
        *old = *n;
    }
    return 0;
}
