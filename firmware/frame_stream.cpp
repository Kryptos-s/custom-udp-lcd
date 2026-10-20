#include "frame_stream.h"
#include "jpeg_frame.h"
#ifdef FRAME_STREAM_INDEXED
#include "indexed_target.h"
#endif
#include <stddef.h>

#define FS_MAGIC 0x31534655u
#define FS_VERSION 4u
#define FS_RECEIVING 1u
#define FS_FAILED 2u
#define FS_PENDING 3u

struct fs_stats {
    uint32_t pending_paints;
    uint32_t active_paints;
    uint32_t pending_ms;
    uint32_t active_ms;
    uint32_t native_calls;
    uint32_t native_ms;
    uint32_t validations;
    uint32_t validation_ms;
    uint32_t blits;
};

struct fs_clip_sample {
    uint32_t sequence;
    uint32_t roi;
    uint32_t work;
};

struct fs_clip_capture {
    uint32_t count;
    struct fs_clip_sample samples[5];
};

static_assert(sizeof(struct fs_clip_capture) == 64u, "clip capture must stay bounded");

struct fs_state {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t session;
    uint32_t highest_sequence;
    uint32_t active_index;
    uint32_t active_sequence;
    uint32_t active_length;
    uint32_t stage_sequence;
    uint32_t stage_length;
    uint32_t stage_received;
    uint32_t stage_crc;
    uint32_t stage_status;
    uint32_t stage_error;
    uint32_t error_sequence;
    uint32_t error_code;
    struct fs_stats stats;
    uint8_t slots[2][FRAME_STREAM_CAPACITY];
    struct jpeg_frame_workspace jpeg;
    uint32_t band_handle;
    struct fs_clip_capture clip_capture;
};

static_assert(sizeof(struct fs_state) <= 12800u, "frame stream state exceeds its bounded allocation");

struct fs_jpeg_output {
    struct fs_state *state;
    uint16_t *band;
    uint32_t flags;
    uint32_t selector;
    uint32_t next_x;
    uint32_t next_y;
    uint32_t band_height;
    uint32_t band_visible;
    uint32_t visible_blocks;
    uint32_t skipped_blocks;
};

struct fs_record {
    uint32_t session;
    uint32_t sequence;
    uint32_t total;
    uint32_t crc;
    uint32_t offset;
    uint32_t count;
    char kind;
};

static uint32_t fs_u16(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8);
}

static uint32_t fs_u32(const uint8_t *data)
{
    return fs_u16(data) | (fs_u16(data + 2) << 16);
}

static int fs_nibble(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

static int fs_hex(const char *data, uint32_t digits, uint32_t *value)
{
    uint32_t result = 0;
    uint32_t index;
    for (index = 0; index < digits; ++index) {
        int nibble = fs_nibble(data[index]);
        if (nibble < 0)
            return 0;
        result = (result << 4) | (uint32_t)nibble;
    }
    *value = result;
    return 1;
}

static uint32_t fs_parse(const char *data, uint32_t length, struct fs_record *record)
{
    uint32_t index;
    record->session = 0;
    record->sequence = 0;
    record->total = 0;
    record->crc = 0;
    record->offset = 0;
    record->count = 0;
    record->kind = length > 2 ? data[2] : 0;
    if (length < 12 || data[3] != ':' || !fs_hex(data + 4, 8, &record->session))
        return FRAME_STREAM_SYNTAX;
    if (record->kind == 'B' || record->kind == 'S')
        return length == 12 ? FRAME_STREAM_OK : FRAME_STREAM_SYNTAX;
    if (length < 21 || data[12] != ':' || !fs_hex(data + 13, 8, &record->sequence))
        return FRAME_STREAM_SYNTAX;
    if (record->kind == 'C' || record->kind == 'Q' || record->kind == 'T' || record->kind == 'U')
        return length == 21 ? FRAME_STREAM_OK : FRAME_STREAM_SYNTAX;
    if (record->kind == 'F') {
        if (length != 35 || data[21] != ':' || data[26] != ':' ||
            !fs_hex(data + 22, 4, &record->total) ||
            !fs_hex(data + 27, 8, &record->crc))
            return FRAME_STREAM_SYNTAX;
        return FRAME_STREAM_OK;
    }
    if (record->kind != 'D' || length <= 27 || length > 27 + FRAME_STREAM_CHUNK_CAPACITY * 2 ||
        data[21] != ':' || data[26] != ':' || ((length - 27) & 1u) ||
        !fs_hex(data + 22, 4, &record->offset))
        return FRAME_STREAM_SYNTAX;
    record->count = (length - 27) >> 1;
    for (index = 27; index < length; ++index)
        if (fs_nibble(data[index]) < 0)
            return FRAME_STREAM_SYNTAX;
    return FRAME_STREAM_OK;
}

static char *fs_write_hex(char *destination, uint32_t value)
{
    uint32_t index;
    for (index = 0; index < 8; ++index) {
        uint32_t digit = (value >> (28 - index * 4)) & 15u;
        *destination++ = (char)(digit < 10 ? '0' + digit : 'A' + digit - 10);
    }
    return destination;
}

static void fs_status(const char *kind, uint32_t session, uint32_t sequence, uint32_t error)
{
    char text[64];
    char *destination = text;
    *destination++ = ' ';
    while (*kind)
        *destination++ = *kind++;
    *destination++ = ' ';
    *destination++ = 's';
    *destination++ = '=';
    destination = fs_write_hex(destination, session);
    *destination++ = ' ';
    *destination++ = 'q';
    *destination++ = '=';
    destination = fs_write_hex(destination, sequence);
    *destination++ = ' ';
    *destination++ = 'e';
    *destination++ = '=';
    destination = fs_write_hex(destination, error);
    *destination = 0;
    fs_emit(text);
}

static void fs_stats_status(uint32_t session, uint32_t sequence, const struct fs_stats *stats)
{
    char text[64];
    const uint32_t values[] = {stats->pending_paints, stats->active_paints,
        stats->pending_ms, stats->active_ms, stats->native_calls, stats->native_ms,
        stats->validations, stats->validation_ms, stats->blits};
    uint32_t part, index;
    for (part = 0; part < 3; ++part) {
        char *destination = text;
        *destination++ = ' ';
        *destination++ = 't';
        *destination++ = (char)('0' + part);
        *destination++ = ':';
        destination = fs_write_hex(destination, session);
        destination = fs_write_hex(destination, sequence);
        for (index = 0; index < 3; ++index)
            destination = fs_write_hex(destination, values[part * 3u + index]);
        *destination = 0;
        fs_emit(text);
    }
}

static void fs_clip_status(uint32_t session, uint32_t sequence, const struct fs_clip_capture *capture)
{
    uint32_t part;
    for (part = 0; part < 6u; ++part) {
        char text[45];
        char *destination = text;
        uint32_t first = 0, second = 0, third = 0;
        if (part == 5u)
            first = 0x00010000u | capture->count;
        else if (part < capture->count) {
            first = capture->samples[part].sequence;
            second = capture->samples[part].roi;
            third = capture->samples[part].work;
        }
        *destination++ = ' ';
        *destination++ = 'c';
        *destination++ = (char)('0' + part);
        *destination++ = ':';
        destination = fs_write_hex(destination, session);
        destination = fs_write_hex(destination, sequence);
        destination = fs_write_hex(destination, first);
        destination = fs_write_hex(destination, second);
        destination = fs_write_hex(destination, third);
        *destination = 0;
        fs_emit(text);
    }
}

static int fs_valid_state(const struct fs_state *state)
{
    return state->magic == FS_MAGIC && state->version == FS_VERSION &&
        state->capacity == FRAME_STREAM_CAPACITY && state->session != 0 && state->band_handle != 0 &&
        state->active_index < 2 && state->clip_capture.count <= 5u && state->active_length <= FRAME_STREAM_CAPACITY &&
        state->stage_length <= FRAME_STREAM_CAPACITY &&
        state->stage_received <= state->stage_length && state->stage_status <= FS_PENDING &&
        (!state->active_length || state->active_sequence != 0) &&
        (!state->stage_status || state->stage_sequence != 0);
}

static void fs_initialize(struct fs_state *state, uint32_t session)
{
    uint32_t *words = (uint32_t *)state;
    uint32_t index;
    for (index = 0; index < offsetof(struct fs_state, slots) / sizeof(uint32_t); ++index)
        words[index] = 0;
    state->magic = FS_MAGIC;
    state->version = FS_VERSION;
    state->capacity = FRAME_STREAM_CAPACITY;
    state->session = session;
    state->band_handle = 0;
    state->clip_capture.count = 0;
    for (index = 0; index < 5u; ++index) {
        state->clip_capture.samples[index].sequence = 0;
        state->clip_capture.samples[index].roi = 0;
        state->clip_capture.samples[index].work = 0;
    }
}

static int fs_create_band(struct fs_state *state)
{
    uint32_t handle;
    void *band;
    int valid;
    fs_lock();
    handle = fs_gui_alloc(FRAME_STREAM_JPEG_BAND_BYTES);
    band = handle ? fs_acquire(handle) : NULL;
    valid = band && !((uintptr_t)band & 1u);
    if (band)
        fs_release(&band);
    if (handle && !valid)
        fs_gui_free(handle);
    fs_unlock();
    if (valid)
        state->band_handle = handle;
    return valid;
}

static void fs_destroy_state(struct fs_state *state)
{
    uint32_t handle = state->band_handle;
    state->band_handle = 0;
    if (handle) {
        fs_lock();
        fs_gui_free(handle);
        fs_unlock();
    }
    fs_free(state);
}

static void fs_drop_reference(void **native)
{
    if (*native)
        fs_release(native);
    fs_unlock();
}

static void *fs_native(void *testlcd)
{
    uint32_t handle;
    fs_lock();
    if (!testlcd)
        return NULL;
    handle = fs_get_handle((uint8_t *)testlcd + 4);
    return handle ? fs_acquire(handle) : NULL;
}

static int fs_record_error(struct fs_state *state, const struct fs_record *record, uint32_t error)
{
    int emit = state->error_code == 0 || state->error_sequence != record->sequence;
    if (state->stage_status == FS_FAILED && state->stage_sequence == record->sequence)
        return 0;
    if (state->stage_status == FS_RECEIVING && state->stage_sequence == record->sequence) {
        state->stage_status = FS_FAILED;
        state->stage_error = error;
    }
    if (emit) {
        state->error_sequence = record->sequence;
        state->error_code = error;
    }
    return emit;
}

static uint32_t fs_crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t index;
    for (index = 0; index < length; ++index) {
        uint32_t bit;
        crc ^= data[index];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int fs_valid_native_frame(const uint8_t *data, uint32_t length)
{
    uint32_t offset = 16;
    uint32_t remaining = FRAME_STREAM_WIDTH * FRAME_STREAM_HEIGHT;
    if (length < 16 || fs_u32(data) != 0x000C4D42u ||
        fs_u16(data + 4) != FRAME_STREAM_WIDTH ||
        fs_u16(data + 6) != FRAME_STREAM_HEIGHT ||
        fs_u32(data + 8) != 0x00100000u || fs_u32(data + 12) != 0)
        return 0;
    while (remaining) {
        uint32_t count;
        uint32_t bytes;
        if (offset >= length)
            return 0;
        count = data[offset++];
        if (count)
            bytes = 2;
        else {
            if (offset >= length)
                return 0;
            count = data[offset++];
            if (!count)
                count = 256;
            bytes = count * 2;
        }
        if (count > remaining || bytes > length - offset)
            return 0;
        remaining -= count;
        offset += bytes;
    }
    return offset == length;
}

static int fs_valid_palette_frame(const uint8_t *data, uint32_t length)
{
    return length == FRAME_STREAM_PALETTE_LENGTH && fs_u32(data) == 0x32505346u &&
        fs_u16(data + 4) == FRAME_STREAM_PALETTE_WIDTH &&
        fs_u16(data + 6) == FRAME_STREAM_PALETTE_HEIGHT &&
        fs_u16(data + 8) == FRAME_STREAM_PALETTE_COLORS &&
        fs_u16(data + 10) == 4 && fs_u32(data + 12) == 0;
}

static int fs_valid_jpeg_header(const uint8_t *data, uint32_t length)
{
    return length > FRAME_STREAM_JPEG_HEADER && length <= FRAME_STREAM_CAPACITY &&
        fs_u32(data) == 0x334A5346u && fs_u16(data + 4) == JPEG_FRAME_WIDTH &&
        fs_u16(data + 6) == JPEG_FRAME_HEIGHT &&
        fs_u32(data + 8) == length - FRAME_STREAM_JPEG_HEADER &&
        !(fs_u32(data + 12) & ~FRAME_STREAM_JPEG_SWAP_RB);
}

static int fs_valid_frame(struct fs_state *state, const uint8_t *data, uint32_t length,
                           uint32_t validate_jpeg)
{
    if (length >= FRAME_STREAM_JPEG_HEADER && fs_u32(data) == 0x334A5346u) {
        uint32_t started;
        int valid;
        if (!fs_valid_jpeg_header(data, length))
            return 0;
        if (!validate_jpeg)
            return 1;
        started = fs_clock_ms();
        ++state->stats.validations;
        valid = jpeg_frame_validate(&state->jpeg, data + FRAME_STREAM_JPEG_HEADER,
            length - FRAME_STREAM_JPEG_HEADER) == JPEG_FRAME_OK;
        state->stats.validation_ms += fs_clock_ms() - started;
        return valid;
    }
    return fs_valid_palette_frame(data, length) || fs_valid_native_frame(data, length);
}

static void fs_draw_native_frame(const uint8_t *data)
{
    uint32_t offset = 16;
    uint32_t remaining = FRAME_STREAM_WIDTH * FRAME_STREAM_HEIGHT;
    uint32_t x = 0;
    uint32_t y = 0;
    while (remaining) {
        uint32_t count = data[offset++];
        if (count) {
            fs_set_color(fs_rgb565(fs_u16(data + offset)));
            offset += 2;
            while (count) {
                uint32_t span = FRAME_STREAM_WIDTH - x;
                if (span > count)
                    span = count;
                fs_fill_rect(x * FRAME_STREAM_SCALE, y * FRAME_STREAM_SCALE,
                    (x + span) * FRAME_STREAM_SCALE - 1,
                    (y + 1) * FRAME_STREAM_SCALE - 1);
                count -= span;
                remaining -= span;
                x += span;
                if (x == FRAME_STREAM_WIDTH) {
                    x = 0;
                    ++y;
                }
            }
        } else {
            count = data[offset++];
            if (!count)
                count = 256;
            while (count--) {
                fs_set_color(fs_rgb565(fs_u16(data + offset)));
                offset += 2;
                fs_fill_rect(x * FRAME_STREAM_SCALE, y * FRAME_STREAM_SCALE,
                    (x + 1) * FRAME_STREAM_SCALE - 1,
                    (y + 1) * FRAME_STREAM_SCALE - 1);
                --remaining;
                if (++x == FRAME_STREAM_WIDTH) {
                    x = 0;
                    ++y;
                }
            }
        }
    }
}

static uint32_t fs_palette_index(const uint8_t *data, uint32_t position)
{
    uint32_t packed = data[48 + (position >> 1)];
    return (position & 1u) ? packed & 15u : packed >> 4;
}

static void fs_draw_palette_frame(const uint8_t *data)
{
    uint32_t palette[FRAME_STREAM_PALETTE_COLORS];
    uint32_t previous = FRAME_STREAM_PALETTE_COLORS;
    uint32_t index;
    uint32_t y;
    for (index = 0; index < FRAME_STREAM_PALETTE_COLORS; ++index)
        palette[index] = fs_rgb565(fs_u16(data + 16 + index * 2));
    for (y = 0; y < FRAME_STREAM_PALETTE_HEIGHT; ++y) {
        uint32_t x = 0;
        while (x < FRAME_STREAM_PALETTE_WIDTH) {
            uint32_t start = x;
            index = fs_palette_index(data, y * FRAME_STREAM_PALETTE_WIDTH + x);
            while (++x < FRAME_STREAM_PALETTE_WIDTH &&
                   fs_palette_index(data, y * FRAME_STREAM_PALETTE_WIDTH + x) == index)
                ;
            if (previous == FRAME_STREAM_PALETTE_COLORS || palette[index] != palette[previous])
                fs_set_color(palette[index]);
            previous = index;
            fs_fill_rect(start * FRAME_STREAM_PALETTE_SCALE, y * FRAME_STREAM_PALETTE_SCALE,
                x * FRAME_STREAM_PALETTE_SCALE - 1,
                (y + 1) * FRAME_STREAM_PALETTE_SCALE - 1);
        }
    }
}

static int fs_draw_jpeg_block(void *context, const uint16_t *pixels,
                              uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
    struct fs_jpeg_output *output = static_cast<struct fs_jpeg_output *>(context);
    uint32_t row, column;
    uint32_t count = width * height;
    uint32_t swap = ((output->flags & FRAME_STREAM_JPEG_SWAP_RB) != 0u) ^ (output->selector == 9u);
    if (!output->band || !width || !height || width > 16u ||
        height > FRAME_STREAM_JPEG_BAND_HEIGHT || count > FRAME_STREAM_JPEG_BLOCK_PIXELS ||
        left != output->next_x || top != output->next_y ||
        left > FRAME_STREAM_JPEG_BAND_WIDTH - width || top > JPEG_FRAME_HEIGHT - height)
        return 0;
    if (!left) {
        output->band_height = height;
        output->band_visible = 0;
    } else if (height != output->band_height)
        return 0;
    if (pixels) {
        ++output->visible_blocks;
        output->band_visible = 1;
        for (row = 0; row < height; ++row) {
            for (column = 0; column < width; ++column) {
                uint32_t word = pixels[row * width + column];
                if (swap)
                    word = (word & 0x07e0u) | ((word & 31u) << 11) | ((word >> 11) & 31u);
                output->band[row * FRAME_STREAM_JPEG_BAND_WIDTH + left + column] = (uint16_t)word;
            }
        }
    } else
        ++output->skipped_blocks;
    output->next_x += width;
    if (output->next_x == FRAME_STREAM_JPEG_BAND_WIDTH) {
        if (output->band_visible) {
            ++output->state->stats.blits;
            if (!fs_blit_rgb565(output->band, FRAME_STREAM_JPEG_BAND_WIDTH, height, 0, top, output->selector))
                return 0;
        }
        output->next_x = 0;
        output->next_y += height;
    }
    return 1;
}

static int fs_draw_frame(struct fs_state *state, const uint8_t *data, uint32_t length)
{
    if (fs_u32(data) == 0x334A5346u) {
        struct fs_jpeg_output output;
        struct fs_paint_clip clip;
        struct jpeg_frame_region region = {0, 0, JPEG_FRAME_WIDTH - 1u, JPEG_FRAME_HEIGHT - 1u};
        void *band = fs_acquire(state->band_handle);
        int result;
        int clip_result;
        uint32_t clip_reason;
        if (!band)
            return 0;
        if ((uintptr_t)band & 1u) {
            fs_release(&band);
            return 0;
        }
        output.band = static_cast<uint16_t *>(band);
        output.state = state;
        output.flags = fs_u32(data + 12);
        output.selector = fs_raw_bitmap_selector();
        if (output.selector != 9u) output.selector = 8u;
        output.next_x = 0;
        output.next_y = 0;
        output.band_height = 0;
        output.band_visible = 0;
        output.visible_blocks = 0;
        output.skipped_blocks = 0;
        clip_result = fs_read_paint_clip(&clip);
        clip_reason = clip_result == FS_CLIP_READ_TARGET ? 6u :
            clip_result >= FS_CLIP_READ_OK && clip_result <= FS_CLIP_READ_ORIGIN_RANGE ?
            (uint32_t)(clip_result - 1) : 1u;
        if ((clip_result == FS_CLIP_READ_OK || clip_result == FS_CLIP_READ_TARGET) &&
            (clip.left > clip.right || clip.top > clip.bottom))
            clip_reason = 4u;
        else if ((clip_result == FS_CLIP_READ_OK || clip_result == FS_CLIP_READ_TARGET) &&
            (clip.right < 0 || clip.bottom < 0 ||
            clip.left >= (int32_t)JPEG_FRAME_WIDTH || clip.top >= (int32_t)JPEG_FRAME_HEIGHT))
            clip_reason = 5u;
        else if (clip_result == FS_CLIP_READ_OK || clip_result == FS_CLIP_READ_TARGET) {
            region.left = clip.left < 0 ? 0u : (uint32_t)clip.left;
            region.top = clip.top < 0 ? 0u : (uint32_t)clip.top;
            region.right = clip.right >= (int32_t)JPEG_FRAME_WIDTH ? JPEG_FRAME_WIDTH - 1u : (uint32_t)clip.right;
            region.bottom = clip.bottom >= (int32_t)JPEG_FRAME_HEIGHT ? JPEG_FRAME_HEIGHT - 1u : (uint32_t)clip.bottom;
        }
        result = jpeg_frame_decode_region(&state->jpeg, data + FRAME_STREAM_JPEG_HEADER,
            length - FRAME_STREAM_JPEG_HEADER, fs_draw_jpeg_block, &output, &region) == JPEG_FRAME_OK &&
            output.next_x == 0 && output.next_y == JPEG_FRAME_HEIGHT;
        if (result && state->clip_capture.count < 5u) {
            struct fs_clip_sample *sample = &state->clip_capture.samples[state->clip_capture.count++];
            uint32_t pending = state->stage_status == FS_PENDING;
            sample->sequence = pending ? state->stage_sequence : state->active_sequence;
            sample->roi = region.left | (region.top << 8) | (region.right << 16) | (region.bottom << 24);
            sample->work = output.visible_blocks | (output.skipped_blocks << 12) |
                (clip_reason << 28) | (pending << 31);
        }
        fs_release(&band);
        return result;
    } else if (fs_u32(data) == 0x32505346u)
        fs_draw_palette_frame(data);
    else
        fs_draw_native_frame(data);
    return 1;
}

static void fs_begin(void *testlcd, const struct fs_record *record)
{
    struct fs_state *fresh = NULL;
    struct fs_state *state;
    uint32_t error = FRAME_STREAM_OK;
    void *native;
    if (!record->session) {
        fs_status("error", 0, 0, FRAME_STREAM_SESSION);
        return;
    }
    native = fs_native(testlcd);
    if (!native) {
        fs_drop_reference(&native);
        fs_status("error", record->session, 0, FRAME_STREAM_NATIVE);
        return;
    }
    state = static_cast<struct fs_state *>(fs_state_load(native));
    if (state) {
        if (!fs_valid_state(state))
            error = FRAME_STREAM_STATE;
        else if (state->session != record->session)
            error = FRAME_STREAM_BUSY;
        fs_drop_reference(&native);
        fs_status(error ? "error" : "ready", record->session, 0, error);
        return;
    }
    fs_drop_reference(&native);
    fresh = static_cast<struct fs_state *>(fs_alloc((uint32_t)sizeof(*fresh)));
    if (!fresh) {
        fs_status("error", record->session, 0, FRAME_STREAM_ALLOCATION);
        return;
    }
    fs_initialize(fresh, record->session);
    if (!fs_create_band(fresh)) {
        fs_destroy_state(fresh);
        fs_status("error", record->session, 0, FRAME_STREAM_ALLOCATION);
        return;
    }
    native = fs_native(testlcd);
    if (!native)
        error = FRAME_STREAM_NATIVE;
    else {
        state = static_cast<struct fs_state *>(fs_state_load(native));
        if (!state) {
            fs_state_store(native, fresh);
            fresh = NULL;
        } else if (!fs_valid_state(state))
            error = FRAME_STREAM_STATE;
        else if (state->session != record->session)
            error = FRAME_STREAM_BUSY;
    }
    fs_drop_reference(&native);
    if (fresh)
        fs_destroy_state(fresh);
    fs_status(error ? "error" : "ready", record->session, 0, error);
}

static void fs_stop(void *testlcd, uint32_t session, int emit)
{
    void *native = fs_native(testlcd);
    struct fs_state *state = native ? static_cast<struct fs_state *>(fs_state_load(native)) : NULL;
    struct fs_state *detached = NULL;
    uint32_t error = native ? FRAME_STREAM_OK : FRAME_STREAM_NATIVE;
    if (state) {
        if (!session || (fs_valid_state(state) && state->session == session)) {
            detached = state;
            fs_state_store(native, NULL);
        } else
            error = fs_valid_state(state) ? FRAME_STREAM_SESSION : FRAME_STREAM_STATE;
    }
    fs_drop_reference(&native);
    if (detached)
        fs_destroy_state(detached);
    if (emit)
        fs_status(error ? "error" : "stopped", session, 0, error);
}

static void fs_query(const struct fs_state *state, const struct fs_record *record,
                     const char **kind, uint32_t *error)
{
    *error = FRAME_STREAM_OK;
    if (!record->sequence)
        *kind = "ready";
    else if (record->sequence == state->stage_sequence && state->stage_status) {
        if (state->stage_status == FS_RECEIVING)
            *kind = "receiving";
        else if (state->stage_status == FS_PENDING)
            *kind = "pending";
        else {
            *kind = "error";
            *error = state->stage_error;
        }
    } else if (state->active_length && record->sequence == state->active_sequence)
        *kind = "painted";
    else {
        *kind = "error";
        *error = state->error_code && record->sequence == state->error_sequence ?
            state->error_code : FRAME_STREAM_SEQUENCE;
    }
}

int frame_stream_control(void *testlcd, const char *data, uint32_t length)
{
    struct fs_record record;
    struct fs_state *state;
    const char *kind = "error";
    uint32_t error;
    uint32_t emit = 0;
    uint32_t invalidate = 0;
    uint32_t emit_stats = 0;
    union {
        struct fs_stats stats;
        struct fs_clip_capture clips;
    } snapshot;
    void *native;
#ifdef FRAME_STREAM_INDEXED
    if (indexed_target_control(testlcd, data, length)) return 1;
#endif
    if (!data || length < 2 || data[0] != 'F' || data[1] != ':')
        return 0;
    error = fs_parse(data, length, &record);
    if (!error && record.kind == 'B') {
        fs_begin(testlcd, &record);
        return 1;
    }
    if (!error && record.kind == 'S') {
        fs_stop(testlcd, record.session, 1);
        return 1;
    }
    native = fs_native(testlcd);
    state = native ? static_cast<struct fs_state *>(fs_state_load(native)) : NULL;
    if (!error && !native)
        error = FRAME_STREAM_NATIVE;
    if (!error && !state)
        error = FRAME_STREAM_SESSION;
    if (!error && !fs_valid_state(state))
        error = FRAME_STREAM_STATE;
    if (!error && (!record.session || record.session != state->session))
        error = FRAME_STREAM_SESSION;
    if (!error && (record.kind == 'Q' || record.kind == 'T' || record.kind == 'U')) {
        fs_query(state, &record, &kind, &error);
        emit = 1;
        if (!error && record.kind == 'T') {
            snapshot.stats = state->stats;
            emit_stats = 1;
        } else if (!error && record.kind == 'U') {
            snapshot.clips = state->clip_capture;
            emit_stats = 2;
        }
    } else if (!error && record.kind == 'F') {
        if (state->stage_status == FS_PENDING)
            error = FRAME_STREAM_PENDING;
        else if (state->stage_status == FS_RECEIVING)
            error = FRAME_STREAM_BUSY;
        else if (!record.sequence || state->active_sequence == 0xFFFFFFFFu ||
                 record.sequence != state->active_sequence + 1u ||
                 record.sequence <= state->highest_sequence)
            error = FRAME_STREAM_SEQUENCE;
        else if (record.total < 16 || record.total > FRAME_STREAM_CAPACITY)
            error = FRAME_STREAM_LENGTH;
        else {
            state->highest_sequence = record.sequence;
            state->stage_sequence = record.sequence;
            state->stage_length = record.total;
            state->stage_received = 0;
            state->stage_crc = record.crc;
            state->stage_status = FS_RECEIVING;
            state->stage_error = 0;
            state->error_sequence = 0;
            state->error_code = 0;
        }
    } else if (!error) {
        if (!record.sequence || record.sequence != state->stage_sequence)
            error = FRAME_STREAM_SEQUENCE;
        else if (state->stage_status == FS_PENDING)
            error = FRAME_STREAM_PENDING;
        else if (state->stage_status == FS_FAILED)
            error = state->stage_error;
        else if (state->stage_status != FS_RECEIVING)
            error = FRAME_STREAM_SEQUENCE;
        else if (record.kind == 'D') {
            uint32_t index;
            if (record.offset != state->stage_received)
                error = FRAME_STREAM_OFFSET;
            else if (record.count > state->stage_length - state->stage_received)
                error = FRAME_STREAM_LENGTH;
            else {
                uint8_t *destination = state->slots[state->active_index ^ 1u] + state->stage_received;
                for (index = 0; index < record.count; ++index)
                    destination[index] = (uint8_t)((fs_nibble(data[27 + index * 2]) << 4) |
                        fs_nibble(data[28 + index * 2]));
                state->stage_received += record.count;
            }
        } else if (record.kind == 'C') {
            const uint8_t *frame = state->slots[state->active_index ^ 1u];
            if (state->stage_received != state->stage_length)
                error = FRAME_STREAM_INCOMPLETE;
            else if (fs_crc32(frame, state->stage_length) != state->stage_crc)
                error = FRAME_STREAM_CRC;
            else if (!fs_valid_frame(state, frame, state->stage_length, 1))
                error = FRAME_STREAM_FORMAT;
            else {
                state->stage_status = FS_PENDING;
                invalidate = 1;
            }
        } else
            error = FRAME_STREAM_SYNTAX;
    }
    if (error && !emit) {
        emit = 1;
        if (state && fs_valid_state(state) && record.session == state->session)
            emit = (uint32_t)fs_record_error(state, &record, error);
    }
    fs_drop_reference(&native);
    if (invalidate)
        fs_invalidate((uint8_t *)testlcd + 4, FRAME_STREAM_COLOR);
    if (emit_stats == 1u)
        fs_stats_status(record.session, record.sequence, &snapshot.stats);
    else if (emit_stats == 2u)
        fs_clip_status(record.session, record.sequence, &snapshot.clips);
    else if (emit)
        fs_status(kind, record.session, record.sequence, error);
    return 1;
}

void frame_stream_paint(const void *message)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(message);
    struct fs_state *state;
    void *native;
    uint32_t session = 0;
    uint32_t sequence = 0;
    uint32_t error = 0;
    uint32_t emit = 0;
    uint32_t pending;
    uint32_t started;
    const uint8_t *frame;
#ifdef FRAME_STREAM_INDEXED
    if (indexed_target_paint(message)) return;
#endif
    if (!message || fs_u32(bytes) != 15)
        return;
    fs_lock();
    native = fs_acquire(fs_u32(bytes + 4));
    state = native ? static_cast<struct fs_state *>(fs_state_load(native)) : NULL;
    if (!state || !fs_valid_state(state)) {
        fs_drop_reference(&native);
        return;
    }
    pending = state->stage_status == FS_PENDING;
    if (!pending && !state->active_length) {
        fs_drop_reference(&native);
        return;
    }
    session = state->session;
    started = fs_clock_ms();
    if (pending)
        ++state->stats.pending_paints;
    else
        ++state->stats.active_paints;
    sequence = pending ? state->stage_sequence : state->active_sequence;
    frame = state->slots[state->active_index ^ pending];
    if (!fs_valid_frame(state, frame, pending ? state->stage_length : state->active_length, 0))
        error = FRAME_STREAM_FORMAT;
    else if (!fs_draw_frame(state, frame, pending ? state->stage_length : state->active_length))
        error = FRAME_STREAM_DRAW;
    if (error) {
        struct fs_record record;
        record.sequence = sequence;
        emit = (uint32_t)fs_record_error(state, &record, error);
        if (pending) {
            state->stage_status = FS_FAILED;
            state->stage_error = error;
        }
    } else {
        if (pending) {
            state->active_index ^= 1u;
            state->active_sequence = sequence;
            state->active_length = state->stage_length;
            state->stage_status = 0;
            emit = 1;
        }
    }
    if (pending)
        state->stats.pending_ms += fs_clock_ms() - started;
    else
        state->stats.active_ms += fs_clock_ms() - started;
    fs_drop_reference(&native);
    if (emit)
        fs_status(error ? "error" : "painted", session, sequence, error);
}

void frame_stream_note_native(const void *message, uint32_t started)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(message);
    uint32_t elapsed = fs_clock_ms() - started;
    struct fs_state *state;
    void *native;
    if (!message || fs_u32(bytes) != 15u)
        return;
    fs_lock();
    native = fs_acquire(fs_u32(bytes + 4));
    state = native ? static_cast<struct fs_state *>(fs_state_load(native)) : NULL;
    if (state && fs_valid_state(state)) {
        ++state->stats.native_calls;
        state->stats.native_ms += elapsed;
    }
    fs_drop_reference(&native);
}

int frame_stream_has_full_frame(const void *message)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(message);
    struct fs_state *state;
    void *native;
    uint32_t pending;
    int result = 0;
#ifdef FRAME_STREAM_INDEXED
    if (indexed_target_has_canvas(message)) return 1;
#endif
    if (!message || fs_u32(bytes) != 15u) return 0;
    fs_lock();
    native = fs_acquire(fs_u32(bytes + 4));
    state = native ? static_cast<struct fs_state *>(fs_state_load(native)) : NULL;
    if (state && fs_valid_state(state)) {
        pending = state->stage_status == FS_PENDING;
        if (pending || state->active_length)
            result = fs_valid_jpeg_header(state->slots[state->active_index ^ pending],
                pending ? state->stage_length : state->active_length);
    }
    fs_drop_reference(&native);
    return result;
}

void frame_stream_dispose(void *testlcd)
{
#ifdef FRAME_STREAM_INDEXED
    if (indexed_target_dispose(testlcd)) return;
#endif
    fs_stop(testlcd, 0, 0);
}
