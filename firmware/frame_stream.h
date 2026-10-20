#ifndef FRAME_STREAM_H
#define FRAME_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_STREAM_CAPACITY 4096u
#define FRAME_STREAM_CHUNK_CAPACITY 448u
#define FRAME_STREAM_WIDTH 40u
#define FRAME_STREAM_HEIGHT 40u
#define FRAME_STREAM_SCALE 6u
#define FRAME_STREAM_PALETTE_WIDTH 80u
#define FRAME_STREAM_PALETTE_HEIGHT 80u
#define FRAME_STREAM_PALETTE_SCALE 3u
#define FRAME_STREAM_PALETTE_COLORS 16u
#define FRAME_STREAM_PALETTE_LENGTH 3248u
#define FRAME_STREAM_JPEG_HEADER 16u
#define FRAME_STREAM_JPEG_SWAP_RB 1u
#define FRAME_STREAM_JPEG_BLOCK_PIXELS 256u
#define FRAME_STREAM_JPEG_BAND_WIDTH 240u
#define FRAME_STREAM_JPEG_BAND_HEIGHT 16u
#define FRAME_STREAM_JPEG_BAND_BYTES (FRAME_STREAM_JPEG_BAND_WIDTH * FRAME_STREAM_JPEG_BAND_HEIGHT * 2u)
#define FRAME_STREAM_COLOR 0xFF554A18u

enum frame_stream_error {
    FRAME_STREAM_OK = 0,
    FRAME_STREAM_SYNTAX = 1,
    FRAME_STREAM_SESSION = 2,
    FRAME_STREAM_BUSY = 3,
    FRAME_STREAM_ALLOCATION = 4,
    FRAME_STREAM_NATIVE = 5,
    FRAME_STREAM_STATE = 6,
    FRAME_STREAM_SEQUENCE = 7,
    FRAME_STREAM_LENGTH = 8,
    FRAME_STREAM_OFFSET = 9,
    FRAME_STREAM_CRC = 10,
    FRAME_STREAM_FORMAT = 11,
    FRAME_STREAM_PENDING = 12,
    FRAME_STREAM_INCOMPLETE = 13,
    FRAME_STREAM_DRAW = 14
};

struct fs_paint_clip {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

enum fs_clip_read_result {
    FS_CLIP_READ_OK = 1,
    FS_CLIP_READ_NO_CONTEXT = 2,
    FS_CLIP_READ_INACTIVE = 3,
    FS_CLIP_READ_ORIGIN_RANGE = 4,
    FS_CLIP_READ_TARGET = 5
};

void *fs_alloc(uint32_t size);
void fs_free(void *pointer);
uint32_t fs_gui_alloc(uint32_t size);
void fs_gui_free(uint32_t handle);
void fs_lock(void);
void fs_unlock(void);
uint32_t fs_get_handle(void *wrapper);
void *fs_acquire(uint32_t handle);
void fs_release(void **native);
void *fs_state_load(void *native);
void fs_state_store(void *native, void *state);
void fs_invalidate(void *wrapper, uint32_t color);
uint32_t fs_rgb565(uint32_t word);
void fs_set_color(uint32_t color);
void fs_fill_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
uint32_t fs_raw_bitmap_selector(void);
int fs_read_paint_clip(struct fs_paint_clip *clip);
int fs_blit_rgb565(const uint16_t *pixels, uint32_t width, uint32_t height,
                   uint32_t left, uint32_t top, uint32_t selector);
void fs_emit(const char *text);
uint32_t fs_clock_ms(void);

int frame_stream_control(void *testlcd, const char *data, uint32_t length);
void frame_stream_note_native(const void *message, uint32_t started);
void frame_stream_paint(const void *message);
int frame_stream_has_full_frame(const void *message);
void frame_stream_dispose(void *testlcd);

#ifdef __cplusplus
}
#endif

#endif
