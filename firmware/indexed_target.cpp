#include "indexed_target.h"
#include "indexed_stream.h"
#include "indexed_heap.h"
#include "frame_stream.h"
#include <stddef.h>

#define IT_MAGIC 0x31544955u
#define IT_DIRECT 1u
#define IT_OPEN 2u
#define IT_FAULT 4u
#define IT_REPORTED 8u

struct indexed_target {
    uint32_t magic;
    uint32_t bottom_handle;
    uint32_t band_handle;
    uint32_t paint_ms;
    uint32_t receive_ms;
    uint32_t direct_flags;
    uint32_t preheader_used;
    uint8_t preheader[INDEXED_STREAM_HEADER_BYTES];
    struct indexed_stream stream;
};

static_assert(sizeof(struct indexed_target) <= 1536u, "indexed owner exceeds metadata allocation bound");

static uint32_t it_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int it_hex(const char *data, uint32_t *value)
{
    uint32_t result = 0;
    uint32_t index;
    for (index = 0; index < 8u; ++index) {
        uint32_t digit;
        char byte = data[index];
        if (byte >= '0' && byte <= '9') digit = (uint32_t)(byte - '0');
        else if (byte >= 'A' && byte <= 'F') digit = (uint32_t)(byte - 'A') + 10u;
        else if (byte >= 'a' && byte <= 'f') digit = (uint32_t)(byte - 'a') + 10u;
        else return 0;
        result = (result << 4) | digit;
    }
    *value = result;
    return 1;
}

static char *it_write(char *destination, uint32_t value)
{
    uint32_t index;
    for (index = 0; index < 8u; ++index) {
        uint32_t digit = (value >> (28u - 4u * index)) & 15u;
        *destination++ = (char)(digit < 10u ? '0' + digit : 'A' + digit - 10u);
    }
    return destination;
}

static void it_status(const char *kind, uint32_t session, uint32_t sequence, uint32_t error)
{
    char text[64];
    char *next = text;
    *next++ = ' ';
    *next++ = 'r';
    while (*kind) *next++ = *kind++;
    *next++ = ':';
    next = it_write(next, session);
    next = it_write(next, sequence);
    next = it_write(next, error);
    *next = 0;
    fs_emit(text);
}

static struct indexed_target *it_state(void *native)
{
    struct indexed_target *state = native ? static_cast<struct indexed_target *>(fs_state_load(native)) : NULL;
    if (!state || state->magic != IT_MAGIC)
        return NULL;
    return state;
}

static void it_release(void **native)
{
    if (*native) fs_release(native);
    fs_unlock();
}

static void *it_native(void *testlcd)
{
    uint32_t handle;
    fs_lock();
    if (!testlcd) return NULL;
    handle = fs_get_handle((uint8_t *)testlcd + 4);
    return handle ? fs_acquire(handle) : NULL;
}

extern "C" uint32_t indexed_current_handle(void);

static void *it_current(uint32_t *current_handle)
{
    uint32_t handle;
    fs_lock();
    handle = indexed_current_handle();
    if (current_handle) *current_handle = handle;
    return handle ? fs_acquire(handle) : NULL;
}

static void it_destroy(struct indexed_target *state)
{
    if (state->band_handle) fs_gui_free(state->band_handle);
    if (state->bottom_handle) fs_gui_free(state->bottom_handle);
    state->magic = 0;
}

static void it_free_general(struct indexed_target *state)
{
    uint32_t index;
    for (index = 0; index < INDEXED_STREAM_TOP_BLOCKS; ++index)
        if (state->stream.top_blocks[index]) fs_free(state->stream.top_blocks[index]);
    fs_free(state);
}

static uint32_t it_begin(void *testlcd, uint32_t session)
{
    struct indexed_target *fresh;
    struct indexed_target *current;
    void *native;
    if (!session) return INDEXED_STREAM_SESSION;
    native = it_native(testlcd);
    current = it_state(native);
    if (!native || fs_state_load(native)) {
        uint32_t result = current && current->stream.session == session && !(current->direct_flags & IT_OPEN) ?
            0u : INDEXED_STREAM_STATE;
        it_release(&native);
        return result;
    }
    it_release(&native);
    fresh = static_cast<struct indexed_target *>(indexed_heap_alloc((uint32_t)sizeof(*fresh)));
    if (!fresh) return FRAME_STREAM_ALLOCATION;
    fresh->magic = IT_MAGIC;
    fresh->bottom_handle = 0;
    fresh->band_handle = 0;
    fresh->paint_ms = 0;
    fresh->receive_ms = 0;
    fresh->direct_flags = 0;
    fresh->preheader_used = 0;
    indexed_stream_initialize(&fresh->stream, session);
    native = it_native(testlcd);
    if (!native || fs_state_load(native)) {
        it_destroy(fresh);
        it_release(&native);
        it_free_general(fresh);
        return INDEXED_STREAM_STATE;
    }
    fs_state_store(native, fresh);
    it_release(&native);
    return 0;
}

static uint32_t it_allocate(void *testlcd, uint32_t session, const char **status, uint32_t *step)
{
    void *native = it_native(testlcd);
    struct indexed_target *state = it_state(native);
    uint32_t error = 0;
    uint32_t handle = 0;
    void *memory = NULL;
    if (!state || state->stream.session != session) error = INDEXED_STREAM_SESSION;
    else if (state->stream.phase != INDEXED_STREAM_IDLE || (state->direct_flags & IT_OPEN))
        error = INDEXED_STREAM_STATE;
    else {
        uint32_t index;
        for (index = 0; index < INDEXED_STREAM_TOP_BLOCKS; ++index) {
            if (!state->stream.top_blocks[index]) {
                uint32_t length = index + 1u == INDEXED_STREAM_TOP_BLOCKS ?
                    INDEXED_STREAM_TOP_LAST_BYTES : INDEXED_STREAM_TOP_BLOCK_BYTES;
                struct indexed_target *expected = state;
                uint8_t *fresh;
                it_release(&native);
                fresh = static_cast<uint8_t *>(indexed_heap_alloc(length));
                if (!fresh) return FRAME_STREAM_ALLOCATION;
                native = it_native(testlcd);
                state = it_state(native);
                if (!state || state != expected || state->stream.session != session ||
                    state->stream.phase != INDEXED_STREAM_IDLE || state->stream.top_blocks[index])
                    error = INDEXED_STREAM_STATE;
                else error = indexed_stream_bind_top(&state->stream, index, fresh, length);
                it_release(&native);
                if (error) fs_free(fresh);
                else {
                    *status = "top";
                    *step = index + 1u;
                }
                return error;
            }
        }
    }
    if (!error && state && !state->bottom_handle) {
        handle = fs_gui_alloc(INDEXED_STREAM_BOTTOM_BYTES);
        memory = handle ? fs_acquire(handle) : NULL;
        if (!memory) error = FRAME_STREAM_ALLOCATION;
        else {
            state->bottom_handle = handle;
            *status = "bottom";
        }
    } else if (!error && state && !state->band_handle) {
        handle = fs_gui_alloc(INDEXED_STREAM_BAND_PIXELS * 2u);
        memory = handle ? fs_acquire(handle) : NULL;
        if (!memory || ((uintptr_t)memory & 1u)) error = FRAME_STREAM_ALLOCATION;
        else {
            state->band_handle = handle;
            *status = "ready";
        }
    } else if (!error) *status = "ready";
    if (memory) fs_release(&memory);
    if (error && handle) fs_gui_free(handle);
    it_release(&native);
    return error;
}

static int it_complete(const struct indexed_target *state)
{
    uint32_t index;
    if (!state->bottom_handle || !state->band_handle) return 0;
    for (index = 0; index < INDEXED_STREAM_TOP_BLOCKS; ++index)
        if (!state->stream.top_blocks[index]) return 0;
    return 1;
}

uint32_t indexed_target_idle_ticks(uint32_t ticks)
{
    void *native;
    struct indexed_target *state;
    uint32_t phase;
    if (ticks != 10u || indexed_gui_ready() != 1u) return ticks;
    native = it_current(NULL);
    state = it_state(native);
    if (state && it_complete(state) && (state->direct_flags & IT_DIRECT) &&
        !(state->direct_flags & IT_FAULT) && state->stream.session && !state->stream.error) {
        phase = state->stream.phase;
        if (phase == INDEXED_STREAM_IDLE || phase == INDEXED_STREAM_RECEIVING ||
            phase == INDEXED_STREAM_PENDING || phase == INDEXED_STREAM_ACTIVE)
            ticks = 1u;
    }
    it_release(&native);
    return ticks;
}

static uint32_t it_direct(void *testlcd, uint32_t session)
{
    void *native = it_native(testlcd);
    struct indexed_target *state = it_state(native);
    uint32_t error = 0;
    if (!state || state->stream.session != session) error = INDEXED_STREAM_SESSION;
    else if (!it_complete(state) || (state->direct_flags & (IT_OPEN | IT_FAULT)) ||
             state->stream.error || (state->stream.phase != INDEXED_STREAM_IDLE &&
             state->stream.phase != INDEXED_STREAM_ACTIVE)) error = INDEXED_STREAM_STATE;
    else state->direct_flags |= IT_DIRECT;
    it_release(&native);
    return error;
}

int indexed_target_dispose(void *testlcd)
{
    void *native = it_native(testlcd);
    struct indexed_target *state = it_state(native);
    if (!state) {
        it_release(&native);
        return 0;
    }
    fs_state_store(native, NULL);
    it_destroy(state);
    it_release(&native);
    it_free_general(state);
    return 1;
}

int indexed_target_control(void *testlcd, const char *data, uint32_t length)
{
    uint32_t session = 0;
    uint32_t sequence = 0;
    uint32_t total = 0;
    uint32_t error = 0;
    uint32_t invalidate = 0;
    char kind;
    const char *status = "error";
    void *native;
    struct indexed_target *state;
    if (!data || length < 2u || data[0] != 'R' || data[1] != ':') return 0;
    kind = length > 2u ? data[2] : 0;
    if (length < 12u || data[3] != ':' || !it_hex(data + 4, &session))
        error = INDEXED_STREAM_ARGUMENT;
    else if (kind == 'B' && length == 12u) {
        error = it_begin(testlcd, session);
        it_status(error ? "error" : "allocated", session, 0, error);
        return 1;
    } else if (kind == 'A' && length == 12u) {
        error = it_allocate(testlcd, session, &status, &sequence);
        it_status(error ? "error" : status, session, sequence, error);
        return 1;
    } else if (kind == 'D' && length == 12u) {
        error = it_direct(testlcd, session);
        it_status(error ? "error" : "direct", session, 0, error);
        return 1;
    } else if (kind == 'S' && length == 12u) {
        native = it_native(testlcd);
        state = it_state(native);
        error = state && state->stream.session != session ? INDEXED_STREAM_SESSION :
            state && (state->direct_flags & IT_OPEN) ? INDEXED_STREAM_STATE : 0u;
        it_release(&native);
        if (!error) indexed_target_dispose(testlcd);
        it_status(error ? "error" : "stopped", session, 0, error);
        return 1;
    } else if (length < 21u || data[12] != ':' || !it_hex(data + 13, &sequence))
        error = INDEXED_STREAM_ARGUMENT;
    else if (kind == 'F') {
        if (length != 30u || data[21] != ':' || !it_hex(data + 22, &total))
            error = INDEXED_STREAM_ARGUMENT;
    } else if ((kind != 'C' && kind != 'Q' && kind != 'T') || length != 21u)
        error = INDEXED_STREAM_ARGUMENT;
    native = it_native(testlcd);
    state = it_state(native);
    if (!error && (!state || state->stream.session != session)) error = INDEXED_STREAM_SESSION;
    if (!error && ((state->direct_flags & IT_OPEN) ||
        ((state->direct_flags & IT_DIRECT) && (kind == 'F' || kind == 'C'))))
        error = INDEXED_STREAM_STATE;
    if (!error && kind == 'F') {
        if (!state->bottom_handle || !state->band_handle) error = INDEXED_STREAM_STATE;
        else error = indexed_stream_arm(&state->stream, sequence, total);
        status = "armed";
    } else if (!error && kind == 'C') {
        if (sequence != state->stream.stage_sequence) error = INDEXED_STREAM_SEQUENCE;
        else error = indexed_stream_commit(&state->stream);
        invalidate = !error;
        status = "pending";
    } else if (!error) {
        if (!sequence && state->stream.phase == INDEXED_STREAM_IDLE) status = "ready";
        else if (sequence == state->stream.active_sequence && state->stream.phase == INDEXED_STREAM_ACTIVE)
            status = "painted";
        else if (sequence == state->stream.stage_sequence)
            status = state->stream.phase == INDEXED_STREAM_PENDING ? "pending" : "receiving";
        else error = INDEXED_STREAM_SEQUENCE;
        if (state->stream.error) error = state->stream.error;
        if (kind == 'T') {
            it_status("paints", session, state->stream.renders, state->paint_ms);
            it_status("bytes", session, state->stream.frames, state->receive_ms);
        }
    }
    it_release(&native);
    it_status(error ? "error" : status, session, sequence, error);
    if (invalidate) fs_invalidate((uint8_t *)testlcd + 4, FRAME_STREAM_COLOR);
    return 1;
}

int indexed_target_has_canvas(const void *message)
{
    void *native;
    struct indexed_target *state;
    int result;
    if (!message || it_u32(static_cast<const uint8_t *>(message)) != 15u) return 0;
    fs_lock();
    native = fs_acquire(it_u32((const uint8_t *)message + 4));
    state = it_state(native);
    result = state && (state->stream.active_sequence || state->stream.phase == INDEXED_STREAM_PENDING);
    it_release(&native);
    return result;
}

int indexed_target_paint(const void *message)
{
    void *native;
    void *bottom;
    void *band;
    struct indexed_target *state;
    uint32_t session;
    uint32_t sequence;
    uint32_t started;
    uint32_t error = 0;
    int pending;
    struct fs_paint_clip clip;
    struct indexed_stream_region region;
    const struct indexed_stream_region *visible = NULL;
    if (!message || it_u32(static_cast<const uint8_t *>(message)) != 15u) return 0;
    fs_lock();
    native = fs_acquire(it_u32((const uint8_t *)message + 4));
    state = it_state(native);
    if (!state) {
        it_release(&native);
        return 0;
    }
    if ((state->direct_flags & IT_DIRECT) &&
        ((state->direct_flags & IT_FAULT) || state->stream.error)) {
        it_release(&native);
        return 1;
    }
    session = state->stream.session;
    sequence = state->stream.stage_sequence;
    pending = state->stream.phase == INDEXED_STREAM_PENDING;
    if (state->stream.active_sequence || pending) {
        uint32_t selector = fs_raw_bitmap_selector();
        bottom = fs_acquire(state->bottom_handle);
        band = fs_acquire(state->band_handle);
        started = fs_clock_ms();
        if (!bottom || !band) error = INDEXED_STREAM_STATE;
        else {
            int clip_result = fs_read_paint_clip(&clip);
            if (clip_result == FS_CLIP_READ_OK || clip_result == FS_CLIP_READ_TARGET) {
                region.left = clip.left;
                region.top = clip.top;
                region.right = clip.right;
                region.bottom = clip.bottom;
                visible = &region;
            }
            if ((selector == 8u || selector == 9u) && !((uintptr_t)band & 3u) && indexed_native_eligible()) {
                struct indexed_stream_region native_clip;
                if (indexed_native_clip(&native_clip)) {
                    if (visible && region.left <= region.right && region.top <= region.bottom) {
                        if (region.left < native_clip.left) region.left = native_clip.left;
                        if (region.top < native_clip.top) region.top = native_clip.top;
                        if (region.right > native_clip.right) region.right = native_clip.right;
                        if (region.bottom > native_clip.bottom) region.bottom = native_clip.bottom;
                        if (region.left > region.right || region.top > region.bottom) {
                            region.left = region.right = 240;
                            region.top = region.bottom = 0;
                        }
                    } else region = native_clip;
                    visible = &region;
                }
                error = indexed_stream_render_indexed(&state->stream, static_cast<const uint8_t *>(bottom), band, selector, indexed_native_bitmap, visible);
            } else error = indexed_stream_render_region(&state->stream, static_cast<const uint8_t *>(bottom),
                static_cast<uint16_t *>(band),
                selector == 9u ? 9u : 8u, fs_blit_rgb565, visible);
        }
        state->paint_ms += fs_clock_ms() - started;
        if (band) fs_release(&band);
        if (bottom) fs_release(&bottom);
    }
    it_release(&native);
    if (pending || error) it_status(error ? "error" : "painted", session, sequence, error);
    return 1;
}

int indexed_target_binary_begin(void *receiver, uint32_t members)
{
    void *native;
    struct indexed_target *state;
    int ready = 0;
    if (members > 1u) return 0;
    native = it_current(NULL);
    state = it_state(native);
    if (receiver && state && it_complete(state)) {
        if (members == 1u) {
            ready = (state->direct_flags & IT_DIRECT) &&
                !(state->direct_flags & (IT_OPEN | IT_FAULT)) && !state->stream.error &&
                state->stream.phase == INDEXED_STREAM_ACTIVE && state->stream.active_sequence &&
                state->stream.active_sequence == state->stream.stage_sequence &&
                !state->stream.wire_remaining;
        } else if (state->direct_flags & IT_DIRECT) {
            ready = !(state->direct_flags & (IT_OPEN | IT_FAULT)) && !state->stream.error &&
                (state->stream.phase == INDEXED_STREAM_IDLE || state->stream.phase == INDEXED_STREAM_ACTIVE);
            if (ready) {
                state->direct_flags = IT_DIRECT | IT_OPEN;
                state->preheader_used = 0;
            }
        } else ready = state->stream.phase == INDEXED_STREAM_RECEIVING &&
            state->stream.wire_remaining == state->stream.wire_total && state->stream.wire_remaining;
    }
    if (ready && !members) ((uint8_t *)receiver)[0x30] = 1;
    it_release(&native);
    return ready;
}

static uint32_t it_preheader(struct indexed_target *state)
{
    const uint8_t *header = state->preheader;
    uint32_t codec = it_u32(header + 12);
    uint32_t length = it_u32(header + 16);
    const bool header_ok = it_u32(header) == 0x31464955u;
    const bool codec_ok = !(codec & ~(INDEXED_STREAM_SWAP_RB | 3u)) && (codec & 3u) <= 2u;
    const bool pixels_ok = it_u32(header + 20) == INDEXED_STREAM_PIXELS;
    if (!(header_ok && codec_ok && pixels_ok)) return INDEXED_STREAM_HEADER;
    if (it_u32(header + 4) != state->stream.session) return INDEXED_STREAM_SESSION;
    if (!it_u32(header + 8) || state->stream.active_sequence == 0xFFFFFFFFu ||
        it_u32(header + 8) != state->stream.active_sequence + 1u) return INDEXED_STREAM_SEQUENCE;
    if (!length || length > INDEXED_STREAM_ENCODED_CAP ||
        (!(codec & 3u) && length != INDEXED_STREAM_PIXELS)) return INDEXED_STREAM_LENGTH;
    return indexed_stream_arm(&state->stream, it_u32(header + 8),
        INDEXED_STREAM_HEADER_BYTES + INDEXED_STREAM_PALETTE_BYTES + length);
}

uint32_t indexed_target_binary_feed(void *receiver, const uint8_t *data, uint32_t length)
{
    void *native;
    void *bottom;
    struct indexed_target *state;
    uint32_t handle = 0;
    uint32_t consumed = length;
    uint32_t started;
    uint32_t emit = 0;
    uint32_t session = 0;
    uint32_t sequence = 0;
    uint32_t error = 0;
    if (!receiver || !data || length > 64u) return 0;
    native = it_current(&handle);
    state = it_state(native);
    if (state) {
        uint32_t direct = state->direct_flags & IT_DIRECT;
        uint32_t used = 0;
        started = fs_clock_ms();
        if ((!direct && (!state->stream.wire_remaining ||
            (state->stream.phase != INDEXED_STREAM_RECEIVING && state->stream.phase != INDEXED_STREAM_FAILED))) ||
            (direct && (!(state->direct_flags & IT_OPEN) || (state->direct_flags & IT_FAULT)))) {
            it_release(&native);
            return length;
        }
        if (direct && state->preheader_used < INDEXED_STREAM_HEADER_BYTES) {
            while (used < length && state->preheader_used < INDEXED_STREAM_HEADER_BYTES)
                state->preheader[state->preheader_used++] = data[used++];
            if (state->preheader_used < INDEXED_STREAM_HEADER_BYTES) {
                state->receive_ms += fs_clock_ms() - started;
                it_release(&native);
                return used;
            }
            error = it_preheader(state);
            if (error) state->direct_flags |= IT_FAULT;
        }
        bottom = !(state->direct_flags & IT_FAULT) ? fs_acquire(state->bottom_handle) : NULL;
        if (direct && (state->direct_flags & IT_FAULT)) consumed = length;
        else if (bottom) {
            uint32_t taken = 0;
            if (direct && state->stream.header_received == 0)
                indexed_stream_feed(&state->stream, static_cast<uint8_t *>(bottom), state->preheader, INDEXED_STREAM_HEADER_BYTES, &taken);
            indexed_stream_feed(&state->stream, static_cast<uint8_t *>(bottom), data + used, length - used, &taken);
            consumed = used + taken;
            fs_release(&bottom);
        } else {
            state->stream.error = INDEXED_STREAM_STATE;
            state->stream.phase = INDEXED_STREAM_FAILED;
            if (direct && state->stream.header_received == 0) {
                state->stream.wire_remaining -= INDEXED_STREAM_HEADER_BYTES;
                state->stream.header_received = INDEXED_STREAM_HEADER_BYTES;
            }
            consumed = length - used < state->stream.wire_remaining ? length - used : state->stream.wire_remaining;
            state->stream.wire_remaining -= consumed;
            consumed += used;
        }
        state->receive_ms += fs_clock_ms() - started;
        if (direct && (state->direct_flags & IT_FAULT)) {
            session = state->stream.session;
            sequence = it_u32(state->preheader + 8);
            if (!(state->direct_flags & IT_REPORTED)) {
                state->direct_flags |= IT_REPORTED;
                emit = 1;
            }
        } else if (!state->stream.wire_remaining) {
            ((uint8_t *)receiver)[0x30] = 0;
            if (direct) {
                state->direct_flags &= ~IT_OPEN;
                error = state->stream.error;
                if (!error) error = indexed_stream_commit(&state->stream);
                if (!error) indexed_native_invalidate(handle, FRAME_STREAM_COLOR);
                else {
                    state->direct_flags |= IT_REPORTED;
                    emit = 1;
                    session = state->stream.session;
                    sequence = state->stream.stage_sequence;
                }
            }
        }
    }
    it_release(&native);
    if (emit) it_status("error", session, sequence, error);
    return consumed;
}
