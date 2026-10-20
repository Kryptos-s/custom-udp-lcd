#include "indexed_stream.h"
#include <stddef.h>

#define INDEXED_STREAM_MAGIC 0x31493855u

static_assert(sizeof(struct indexed_stream) <= 28u * 1024u, "indexed stream exceeds general allocation cap");

static const uint32_t crc_table[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
    0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
    0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
    0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u, 0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
    0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
    0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u, 0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
    0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
    0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu, 0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
    0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
    0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u, 0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
    0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
    0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
    0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
    0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
    0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
    0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u, 0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
    0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
    0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u, 0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
    0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
    0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
    0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
    0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u, 0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

static uint32_t is_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint32_t is_crc_byte(uint32_t crc, uint8_t value)
{
    return (crc >> 8) ^ crc_table[(crc ^ value) & 255u];
}

static uint32_t is_crc(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t index;
    for (index = 0; index < length; ++index)
        crc = is_crc_byte(crc, data[index]);
    return ~crc;
}

static int is_valid(const struct indexed_stream *state)
{
    return state && state->magic == INDEXED_STREAM_MAGIC && state->session &&
        state->phase <= INDEXED_STREAM_FAILED && state->wire_remaining <= state->wire_total &&
        state->header_received <= INDEXED_STREAM_HEADER_BYTES &&
        state->palette_received <= INDEXED_STREAM_PALETTE_BYTES &&
        state->encoded_received <= state->encoded_length &&
        state->pixels_received <= INDEXED_STREAM_PIXELS;
}

static int is_bound(const struct indexed_stream *state)
{
    uint32_t index;
    for (index = 0; index < INDEXED_STREAM_TOP_BLOCKS; ++index)
        if (!state->top_blocks[index])
            return 0;
    return 1;
}

static uint32_t is_fail(struct indexed_stream *state, uint32_t error)
{
    if (!state->error)
        state->error = error;
    state->phase = INDEXED_STREAM_FAILED;
    return state->error;
}

void indexed_stream_initialize(struct indexed_stream *state, uint32_t session)
{
    uint8_t *data = (uint8_t *)state;
    uint32_t index;
    if (!state)
        return;
    for (index = 0; index < sizeof(*state); ++index)
        data[index] = 0;
    state->magic = INDEXED_STREAM_MAGIC;
    state->session = session;
    state->phase = session ? INDEXED_STREAM_IDLE : INDEXED_STREAM_FAILED;
    state->error = session ? INDEXED_STREAM_OK : INDEXED_STREAM_SESSION;
}

uint32_t indexed_stream_bind_top(struct indexed_stream *state, uint32_t index,
                                 uint8_t *pointer, uint32_t length)
{
    uint32_t expected;
    if (!is_valid(state))
        return INDEXED_STREAM_STATE;
    if (index >= INDEXED_STREAM_TOP_BLOCKS || !pointer)
        return INDEXED_STREAM_ARGUMENT;
    expected = index == INDEXED_STREAM_TOP_BLOCKS - 1u ?
        INDEXED_STREAM_TOP_LAST_BYTES : INDEXED_STREAM_TOP_BLOCK_BYTES;
    if (length != expected)
        return INDEXED_STREAM_LENGTH;
    if (state->phase != INDEXED_STREAM_IDLE || state->active_sequence ||
        (state->top_blocks[index] && state->top_blocks[index] != pointer))
        return INDEXED_STREAM_STATE;
    state->top_blocks[index] = pointer;
    return INDEXED_STREAM_OK;
}

uint32_t indexed_stream_arm(struct indexed_stream *state, uint32_t sequence, uint32_t total)
{
    if (!is_valid(state) || !is_bound(state))
        return INDEXED_STREAM_STATE;
    if (state->phase != INDEXED_STREAM_IDLE && state->phase != INDEXED_STREAM_ACTIVE)
        return state->error ? state->error : INDEXED_STREAM_STATE;
    if (!sequence || state->active_sequence == 0xFFFFFFFFu || sequence != state->active_sequence + 1u)
        return INDEXED_STREAM_SEQUENCE;
    if (total <= INDEXED_STREAM_HEADER_BYTES + INDEXED_STREAM_PALETTE_BYTES || total > INDEXED_STREAM_WIRE_CAP)
        return INDEXED_STREAM_LENGTH;
    state->phase = INDEXED_STREAM_RECEIVING;
    state->stage_sequence = sequence;
    state->wire_total = total;
    state->wire_remaining = total;
    state->header_received = 0;
    state->palette_received = 0;
    state->encoded_length = 0;
    state->encoded_received = 0;
    state->pixels_received = 0;
    state->expected_palette_crc = 0;
    state->expected_frame_crc = 0;
    state->frame_crc = 0xFFFFFFFFu;
    state->stage_flags = 0;
    state->codec = 0;
    state->token_remaining = 0;
    state->token_run = 0;
    state->lz4_state = INDEXED_LZ4_TOKEN;
    state->lz4_match_length = 0;
    state->lz4_offset = 0;
    state->lz4_last_literals = 0;
    return INDEXED_STREAM_OK;
}

static uint32_t is_header(struct indexed_stream *state)
{
    const uint8_t *header = state->header;
    uint32_t flags = is_u32(header + 12);
    uint32_t length = is_u32(header + 16);
    if (is_u32(header) != 0x31464955u)
        return is_fail(state, INDEXED_STREAM_HEADER);
    if (is_u32(header + 4) != state->session)
        return is_fail(state, INDEXED_STREAM_SESSION);
    if (is_u32(header + 8) != state->stage_sequence)
        return is_fail(state, INDEXED_STREAM_SEQUENCE);
    if ((flags & ~(INDEXED_STREAM_SWAP_RB | 3u)) || (flags & 3u) > 2u ||
        is_u32(header + 20) != INDEXED_STREAM_PIXELS)
        return is_fail(state, INDEXED_STREAM_HEADER);
    if (!length || length > INDEXED_STREAM_ENCODED_CAP ||
        length != state->wire_total - INDEXED_STREAM_HEADER_BYTES - INDEXED_STREAM_PALETTE_BYTES ||
        (!(flags & 3u) && length != INDEXED_STREAM_PIXELS))
        return is_fail(state, INDEXED_STREAM_LENGTH);
    state->codec = flags & 3u;
    state->stage_flags = flags & INDEXED_STREAM_SWAP_RB;
    state->encoded_length = length;
    state->expected_palette_crc = is_u32(header + 24);
    state->expected_frame_crc = is_u32(header + 28);
    return INDEXED_STREAM_OK;
}

static void is_pixel(struct indexed_stream *state, uint8_t *bottom, uint8_t pixel)
{
    uint32_t position = state->pixels_received++;
    if (position < INDEXED_STREAM_TOP_BYTES)
        state->top_blocks[position / INDEXED_STREAM_TOP_BLOCK_BYTES]
            [position % INDEXED_STREAM_TOP_BLOCK_BYTES] = pixel;
    else
        bottom[position - INDEXED_STREAM_TOP_BYTES] = pixel;
    state->frame_crc = is_crc_byte(state->frame_crc, pixel);
}

static uint32_t is_lz4_match(struct indexed_stream *state, uint8_t *bottom)
{
    if (!state->lz4_offset || state->lz4_offset > state->pixels_received ||
        state->pixels_received > INDEXED_STREAM_PIXELS - 12u ||
        state->lz4_match_length > INDEXED_STREAM_PIXELS - state->pixels_received)
        return is_fail(state, INDEXED_STREAM_FORMAT);
    while (state->lz4_match_length) {
        uint32_t position = state->pixels_received - state->lz4_offset;
        uint8_t pixel = position < INDEXED_STREAM_TOP_BYTES ?
            state->top_blocks[position / INDEXED_STREAM_TOP_BLOCK_BYTES]
                [position % INDEXED_STREAM_TOP_BLOCK_BYTES] :
            bottom[position - INDEXED_STREAM_TOP_BYTES];
        is_pixel(state, bottom, pixel);
        --state->lz4_match_length;
    }
    state->lz4_state = INDEXED_LZ4_TOKEN;
    return INDEXED_STREAM_OK;
}

static uint32_t is_lz4_byte(struct indexed_stream *state, uint8_t *bottom, uint8_t value)
{
    if (state->pixels_received == INDEXED_STREAM_PIXELS)
        return is_fail(state, INDEXED_STREAM_FORMAT);
    if (state->lz4_state == INDEXED_LZ4_TOKEN) {
        state->token_remaining = value >> 4;
        state->lz4_last_literals = state->token_remaining;
        state->lz4_match_length = (value & 15u) + 4u;
        state->lz4_offset = 0;
        if (state->token_remaining > INDEXED_STREAM_PIXELS - state->pixels_received)
            return is_fail(state, INDEXED_STREAM_FORMAT);
        state->lz4_state = state->token_remaining == 15u ? INDEXED_LZ4_LITERAL_EXTENSION :
            state->token_remaining ? INDEXED_LZ4_LITERALS : INDEXED_LZ4_OFFSET_LOW;
    } else if (state->lz4_state == INDEXED_LZ4_LITERAL_EXTENSION) {
        if (value > INDEXED_STREAM_PIXELS - state->pixels_received - state->token_remaining)
            return is_fail(state, INDEXED_STREAM_FORMAT);
        state->token_remaining += value;
        state->lz4_last_literals = state->token_remaining;
        if (value != 255u) state->lz4_state = INDEXED_LZ4_LITERALS;
    } else if (state->lz4_state == INDEXED_LZ4_LITERALS) {
        is_pixel(state, bottom, value);
        if (!--state->token_remaining) state->lz4_state = INDEXED_LZ4_OFFSET_LOW;
    } else if (state->lz4_state == INDEXED_LZ4_OFFSET_LOW) {
        state->lz4_offset = value;
        state->lz4_state = INDEXED_LZ4_OFFSET_HIGH;
    } else if (state->lz4_state == INDEXED_LZ4_OFFSET_HIGH) {
        state->lz4_offset |= (uint32_t)value << 8;
        if (!state->lz4_offset || state->lz4_offset > state->pixels_received)
            return is_fail(state, INDEXED_STREAM_FORMAT);
        if (state->lz4_match_length == 19u) state->lz4_state = INDEXED_LZ4_MATCH_EXTENSION;
        else return is_lz4_match(state, bottom);
    } else if (state->lz4_state == INDEXED_LZ4_MATCH_EXTENSION) {
        if (state->lz4_match_length > INDEXED_STREAM_PIXELS - state->pixels_received ||
            value > INDEXED_STREAM_PIXELS - state->pixels_received - state->lz4_match_length)
            return is_fail(state, INDEXED_STREAM_FORMAT);
        state->lz4_match_length += value;
        if (value != 255u) return is_lz4_match(state, bottom);
    } else return is_fail(state, INDEXED_STREAM_FORMAT);
    return INDEXED_STREAM_OK;
}

static uint32_t is_payload_byte(struct indexed_stream *state, uint8_t *bottom, uint8_t value)
{
    if (state->codec == 2u) return is_lz4_byte(state, bottom, value);
    if (!state->codec) {
        if (state->pixels_received == INDEXED_STREAM_PIXELS)
            return is_fail(state, INDEXED_STREAM_FORMAT);
        is_pixel(state, bottom, value);
        return INDEXED_STREAM_OK;
    }
    if (!state->token_remaining) {
        uint32_t count = (value & 127u) + 1u;
        if (count > INDEXED_STREAM_PIXELS - state->pixels_received)
            return is_fail(state, INDEXED_STREAM_FORMAT);
        state->token_remaining = count;
        state->token_run = value >> 7;
    } else if (state->token_run) {
        while (state->token_remaining) {
            is_pixel(state, bottom, value);
            --state->token_remaining;
        }
        state->token_run = 0;
    } else {
        is_pixel(state, bottom, value);
        --state->token_remaining;
    }
    return INDEXED_STREAM_OK;
}

uint32_t indexed_stream_feed(struct indexed_stream *state, uint8_t *bottom,
                             const uint8_t *data, uint32_t length, uint32_t *consumed)
{
    uint32_t index;
    uint32_t count;
    if (consumed)
        *consumed = 0;
    if (!is_valid(state))
        return INDEXED_STREAM_STATE;
    if (!consumed || !bottom || (!data && length))
        return INDEXED_STREAM_ARGUMENT;
    if ((state->phase != INDEXED_STREAM_RECEIVING && state->phase != INDEXED_STREAM_FAILED) ||
        !state->wire_remaining)
        return state->error ? state->error : INDEXED_STREAM_STATE;
    if (!is_bound(state))
        is_fail(state, INDEXED_STREAM_STATE);
    count = length < state->wire_remaining ? length : state->wire_remaining;
    for (index = 0; index < count; ++index) {
        uint8_t value = data[index];
        --state->wire_remaining;
        if (state->error)
            continue;
        if (state->header_received < INDEXED_STREAM_HEADER_BYTES) {
            state->header[state->header_received++] = value;
            if (state->header_received == INDEXED_STREAM_HEADER_BYTES)
                is_header(state);
        } else if (state->palette_received < INDEXED_STREAM_PALETTE_BYTES) {
            state->stage_palette[state->palette_received++] = value;
            if (state->palette_received == INDEXED_STREAM_PALETTE_BYTES &&
                is_crc(state->stage_palette, INDEXED_STREAM_PALETTE_BYTES) != state->expected_palette_crc)
                is_fail(state, INDEXED_STREAM_PALETTE_CRC);
        } else {
            ++state->encoded_received;
            is_payload_byte(state, bottom, value);
        }
    }
    *consumed = count;
    if (!state->wire_remaining && !state->error) {
        if (state->header_received != INDEXED_STREAM_HEADER_BYTES ||
            state->palette_received != INDEXED_STREAM_PALETTE_BYTES ||
            state->encoded_received != state->encoded_length ||
            state->pixels_received != INDEXED_STREAM_PIXELS || state->token_remaining || state->token_run ||
            (state->codec == 2u && (state->lz4_state != INDEXED_LZ4_OFFSET_LOW || state->lz4_last_literals < 5u)))
            is_fail(state, INDEXED_STREAM_FORMAT);
        else if (~state->frame_crc != state->expected_frame_crc)
            is_fail(state, INDEXED_STREAM_FRAME_CRC);
        else
            state->phase = INDEXED_STREAM_STAGED;
    }
    return state->error;
}

uint32_t indexed_stream_commit(struct indexed_stream *state)
{
    uint32_t index;
    if (!is_valid(state))
        return INDEXED_STREAM_STATE;
    if (state->phase != INDEXED_STREAM_STAGED || state->wire_remaining)
        return state->error ? state->error : INDEXED_STREAM_INCOMPLETE;
    for (index = 0; index < 256u; ++index) {
        uint32_t color = (uint32_t)state->stage_palette[index * 2u] |
            ((uint32_t)state->stage_palette[index * 2u + 1u] << 8);
        if (state->stage_flags & INDEXED_STREAM_SWAP_RB)
            color = (color & 0x07E0u) | ((color & 31u) << 11) | ((color >> 11) & 31u);
        state->active_palette[index] = (uint16_t)color;
    }
    state->active_flags = state->stage_flags;
    state->phase = INDEXED_STREAM_PENDING;
    return INDEXED_STREAM_OK;
}

uint32_t indexed_stream_render_region(struct indexed_stream *state, const uint8_t *bottom,
                                      uint16_t *band, uint32_t selector, indexed_stream_blit_fn blit,
                                      const struct indexed_stream_region *region)
{
    uint32_t top;
    uint32_t swap;
    uint32_t left = 0;
    uint32_t first = 0;
    uint32_t right = INDEXED_STREAM_WIDTH - 1u;
    uint32_t last = INDEXED_STREAM_HEIGHT - 1u;
    uint32_t empty = 0;
    if (!is_valid(state) || !is_bound(state))
        return INDEXED_STREAM_STATE;
    if (!bottom || !band || ((uintptr_t)band & 1u) || !blit || (selector != 8u && selector != 9u))
        return INDEXED_STREAM_ARGUMENT;
    if (!state->active_sequence && state->phase != INDEXED_STREAM_PENDING)
        return INDEXED_STREAM_STATE;
    if (state->error)
        return state->error;
    if (region && region->left <= region->right && region->top <= region->bottom) {
        if (region->right < 0 || region->bottom < 0 ||
            region->left >= (int32_t)INDEXED_STREAM_WIDTH || region->top >= (int32_t)INDEXED_STREAM_HEIGHT)
            empty = 1;
        else {
            left = region->left < 0 ? 0u : (uint32_t)region->left;
            first = region->top < 0 ? 0u : (uint32_t)region->top;
            right = region->right >= (int32_t)INDEXED_STREAM_WIDTH ?
                INDEXED_STREAM_WIDTH - 1u : (uint32_t)region->right;
            last = region->bottom >= (int32_t)INDEXED_STREAM_HEIGHT ?
                INDEXED_STREAM_HEIGHT - 1u : (uint32_t)region->bottom;
        }
    }
    swap = selector == 9u;
    ++state->renders;
    for (top = first; !empty && top <= last;) {
        uint32_t row;
        uint32_t width = right - left + 1u;
        uint32_t height = INDEXED_STREAM_BAND_ROWS - top % INDEXED_STREAM_BAND_ROWS;
        uint32_t destination = 0;
        if (height > last - top + 1u)
            height = last - top + 1u;
        for (row = 0; row < height; ++row) {
            uint32_t column;
            uint32_t position = (top + row) * INDEXED_STREAM_WIDTH + left;
            column = 0;
            while (column < width) {
                uint32_t count = width - column;
                const uint8_t *source;
                uint32_t index;
                if (position < INDEXED_STREAM_TOP_BYTES) {
                    uint32_t available = INDEXED_STREAM_TOP_BLOCK_BYTES - position % INDEXED_STREAM_TOP_BLOCK_BYTES;
                    if (available > INDEXED_STREAM_TOP_BYTES - position)
                        available = INDEXED_STREAM_TOP_BYTES - position;
                    if (count > available) count = available;
                    source = state->top_blocks[position / INDEXED_STREAM_TOP_BLOCK_BYTES] + position % INDEXED_STREAM_TOP_BLOCK_BYTES;
                } else source = bottom + (position - INDEXED_STREAM_TOP_BYTES);
                for (index = 0; index < count; ++index) {
                    uint32_t color = state->active_palette[source[index]];
                    if (swap)
                        color = (color & 0x07E0u) | ((color & 31u) << 11) | ((color >> 11) & 31u);
                    band[destination++] = (uint16_t)color;
                }
                position += count;
                column += count;
            }
        }
        ++state->blits;
        if (!blit(band, width, height, left, top, selector))
            return is_fail(state, INDEXED_STREAM_DRAW);
        top += height;
    }
    if (state->phase == INDEXED_STREAM_PENDING) {
        state->active_sequence = state->stage_sequence;
        state->phase = INDEXED_STREAM_ACTIVE;
        ++state->frames;
    }
    return INDEXED_STREAM_OK;
}

uint32_t indexed_stream_render(struct indexed_stream *state, const uint8_t *bottom,
                               uint16_t *band, uint32_t selector, indexed_stream_blit_fn blit)
{
    return indexed_stream_render_region(state, bottom, band, selector, blit, NULL);
}

static const uint8_t *is_source(const struct indexed_stream *state, const uint8_t *bottom,
                                 uint32_t position, uint32_t *available)
{
    if (position < INDEXED_STREAM_TOP_BYTES) {
        *available = INDEXED_STREAM_TOP_BLOCK_BYTES - position % INDEXED_STREAM_TOP_BLOCK_BYTES;
        if (*available > INDEXED_STREAM_TOP_BYTES - position)
            *available = INDEXED_STREAM_TOP_BYTES - position;
        return state->top_blocks[position / INDEXED_STREAM_TOP_BLOCK_BYTES] +
            position % INDEXED_STREAM_TOP_BLOCK_BYTES;
    }
    *available = INDEXED_STREAM_PIXELS - position;
    return bottom + (position - INDEXED_STREAM_TOP_BYTES);
}

uint32_t indexed_stream_render_indexed(struct indexed_stream *state, const uint8_t *bottom,
                                       void *scratch, uint32_t selector,
                                       indexed_stream_indexed_blit_fn blit,
                                       const struct indexed_stream_region *region)
{
    uint32_t *palette = static_cast<uint32_t *>(scratch);
    uint8_t *row_buffer;
    uint32_t left = 0;
    uint32_t first = 0;
    uint32_t right = INDEXED_STREAM_WIDTH - 1u;
    uint32_t last = INDEXED_STREAM_HEIGHT - 1u;
    uint32_t top;
    uint32_t index;
    uint32_t empty = 0;
    if (!is_valid(state) || !is_bound(state)) return INDEXED_STREAM_STATE;
    if (!bottom || !scratch || ((uintptr_t)scratch & 3u) || !blit ||
        (selector != 8u && selector != 9u)) return INDEXED_STREAM_ARGUMENT;
    if (!state->active_sequence && state->phase != INDEXED_STREAM_PENDING) return INDEXED_STREAM_STATE;
    if (state->error) return state->error;
    if (region && region->left <= region->right && region->top <= region->bottom) {
        if (region->right < 0 || region->bottom < 0 ||
            region->left >= (int32_t)INDEXED_STREAM_WIDTH || region->top >= (int32_t)INDEXED_STREAM_HEIGHT)
            empty = 1;
        else {
            left = region->left < 0 ? 0u : (uint32_t)region->left;
            first = region->top < 0 ? 0u : (uint32_t)region->top;
            right = region->right >= (int32_t)INDEXED_STREAM_WIDTH ?
                INDEXED_STREAM_WIDTH - 1u : (uint32_t)region->right;
            last = region->bottom >= (int32_t)INDEXED_STREAM_HEIGHT ?
                INDEXED_STREAM_HEIGHT - 1u : (uint32_t)region->bottom;
        }
    }
    row_buffer = (uint8_t *)scratch + 256u * sizeof(*palette);
    for (index = 0; index < 256u; ++index) {
        uint32_t color = state->active_palette[index];
        if (selector == 9u)
            color = (color & 0x07E0u) | ((color & 31u) << 11) | ((color >> 11) & 31u);
        palette[index] = color;
    }
    ++state->renders;
    for (top = first; !empty && top <= last;) {
        uint32_t available;
        uint32_t width = right - left + 1u;
        uint32_t position = top * INDEXED_STREAM_WIDTH + left;
        uint32_t height = 1;
        const uint8_t *source = is_source(state, bottom, position, &available);
        if (available >= width) {
            height += (available - width) / INDEXED_STREAM_WIDTH;
            if (height > last - top + 1u) height = last - top + 1u;
        } else {
            uint32_t copied = 0;
            while (copied < width) {
                uint32_t count = width - copied;
                source = is_source(state, bottom, position + copied, &available);
                if (count > available) count = available;
                for (index = 0; index < count; ++index)
                    row_buffer[copied + index] = source[index];
                copied += count;
            }
            source = row_buffer;
        }
        ++state->blits;
        if (!blit(source, width, height, left, top, palette)) return is_fail(state, INDEXED_STREAM_DRAW);
        top += height;
    }
    if (state->phase == INDEXED_STREAM_PENDING) {
        state->active_sequence = state->stage_sequence;
        state->phase = INDEXED_STREAM_ACTIVE;
        ++state->frames;
    }
    return INDEXED_STREAM_OK;
}
