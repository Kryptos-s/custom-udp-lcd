#ifndef INDEXED_STREAM_H
#define INDEXED_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INDEXED_STREAM_WIDTH 240u
#define INDEXED_STREAM_HEIGHT 240u
#define INDEXED_STREAM_TOP_ROWS 100u
#define INDEXED_STREAM_TOP_BYTES 24000u
#define INDEXED_STREAM_TOP_BLOCKS 6u
#define INDEXED_STREAM_TOP_BLOCK_BYTES 4096u
#define INDEXED_STREAM_TOP_LAST_BYTES 3520u
#define INDEXED_STREAM_BOTTOM_BYTES 33600u
#define INDEXED_STREAM_BAND_ROWS 4u
#define INDEXED_STREAM_BAND_PIXELS 960u
#define INDEXED_STREAM_HEADER_BYTES 32u
#define INDEXED_STREAM_PALETTE_BYTES 512u
#define INDEXED_STREAM_PIXELS 57600u
#define INDEXED_STREAM_ENCODED_CAP 58050u
#define INDEXED_STREAM_WIRE_CAP 58594u
#define INDEXED_STREAM_SWAP_RB 0x100u

enum indexed_stream_lz4_phase {
    INDEXED_LZ4_TOKEN = 0,
    INDEXED_LZ4_LITERAL_EXTENSION = 1,
    INDEXED_LZ4_LITERALS = 2,
    INDEXED_LZ4_OFFSET_LOW = 3,
    INDEXED_LZ4_OFFSET_HIGH = 4,
    INDEXED_LZ4_MATCH_EXTENSION = 5
};

enum indexed_stream_phase {
    INDEXED_STREAM_IDLE = 0,
    INDEXED_STREAM_RECEIVING = 1,
    INDEXED_STREAM_STAGED = 2,
    INDEXED_STREAM_PENDING = 3,
    INDEXED_STREAM_ACTIVE = 4,
    INDEXED_STREAM_FAILED = 5
};

enum indexed_stream_error {
    INDEXED_STREAM_OK = 0,
    INDEXED_STREAM_ARGUMENT = 1,
    INDEXED_STREAM_STATE = 2,
    INDEXED_STREAM_SESSION = 3,
    INDEXED_STREAM_SEQUENCE = 4,
    INDEXED_STREAM_LENGTH = 5,
    INDEXED_STREAM_HEADER = 6,
    INDEXED_STREAM_PALETTE_CRC = 7,
    INDEXED_STREAM_FORMAT = 8,
    INDEXED_STREAM_FRAME_CRC = 9,
    INDEXED_STREAM_INCOMPLETE = 10,
    INDEXED_STREAM_DRAW = 11
};

struct indexed_stream {
    uint32_t magic;
    uint32_t session;
    uint32_t phase;
    uint32_t error;
    uint32_t active_sequence;
    uint32_t stage_sequence;
    uint32_t wire_total;
    uint32_t wire_remaining;
    uint32_t header_received;
    uint32_t palette_received;
    uint32_t encoded_length;
    uint32_t encoded_received;
    uint32_t pixels_received;
    uint32_t expected_palette_crc;
    uint32_t expected_frame_crc;
    uint32_t frame_crc;
    uint32_t stage_flags;
    uint32_t active_flags;
    uint32_t codec;
    uint32_t token_remaining;
    uint32_t token_run;
    uint32_t lz4_state;
    uint32_t lz4_match_length;
    uint32_t lz4_offset;
    uint32_t lz4_last_literals;
    uint32_t renders;
    uint32_t blits;
    uint32_t frames;
    uint8_t header[INDEXED_STREAM_HEADER_BYTES];
    uint16_t active_palette[256];
    uint8_t stage_palette[INDEXED_STREAM_PALETTE_BYTES];
    uint8_t *top_blocks[INDEXED_STREAM_TOP_BLOCKS];
};

struct indexed_stream_region {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

typedef int (*indexed_stream_blit_fn)(const uint16_t *, uint32_t, uint32_t,
                                     uint32_t, uint32_t, uint32_t);
typedef int (*indexed_stream_indexed_blit_fn)(const uint8_t *, uint32_t, uint32_t,
                                             uint32_t, uint32_t, const uint32_t *);

void indexed_stream_initialize(struct indexed_stream *state, uint32_t session);
uint32_t indexed_stream_bind_top(struct indexed_stream *state, uint32_t index,
                                 uint8_t *pointer, uint32_t length);
uint32_t indexed_stream_arm(struct indexed_stream *state, uint32_t sequence, uint32_t total);
uint32_t indexed_stream_feed(struct indexed_stream *state, uint8_t *bottom,
                             const uint8_t *data, uint32_t length, uint32_t *consumed);
uint32_t indexed_stream_commit(struct indexed_stream *state);
uint32_t indexed_stream_render(struct indexed_stream *state, const uint8_t *bottom,
                               uint16_t *band, uint32_t selector, indexed_stream_blit_fn blit);

uint32_t indexed_stream_render_region(struct indexed_stream *state, const uint8_t *bottom,
                                      uint16_t *band, uint32_t selector, indexed_stream_blit_fn blit,
                                      const struct indexed_stream_region *region);
uint32_t indexed_stream_render_indexed(struct indexed_stream *state, const uint8_t *bottom,
                                       void *scratch, uint32_t selector,
                                       indexed_stream_indexed_blit_fn blit,
                                       const struct indexed_stream_region *region);

#ifdef __cplusplus
}
#endif

#endif
