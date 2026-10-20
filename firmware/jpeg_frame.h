#ifndef JPEG_FRAME_H
#define JPEG_FRAME_H

#include <stdint.h>
#include "vendor/tjpgd/tjpgd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPEG_FRAME_WIDTH 240u
#define JPEG_FRAME_HEIGHT 240u
#define JPEG_FRAME_MAX_LENGTH 4080u
#define JPEG_FRAME_POOL_BYTES 4096u

enum jpeg_frame_result {
    JPEG_FRAME_OK = 0,
    JPEG_FRAME_ARGUMENT = 1,
    JPEG_FRAME_FORMAT = 2,
    JPEG_FRAME_DECODER = 3,
    JPEG_FRAME_OUTPUT = 4
};

typedef int (*jpeg_frame_block_fn)(void *context, const uint16_t *pixels,
                                   uint32_t left, uint32_t top,
                                   uint32_t width, uint32_t height);

struct jpeg_frame_region {
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;
};

struct jpeg_frame_workspace {
    JDEC decoder;
    uint32_t pool[JPEG_FRAME_POOL_BYTES / sizeof(uint32_t)];
    const uint8_t *source;
    uint32_t length;
    uint32_t offset;
    uint32_t next_x;
    uint32_t next_y;
    uint32_t pixels;
    uint32_t output_failed;
    uint32_t prepared;
    uint32_t work_used;
    JRESULT decoder_result;
    jpeg_frame_block_fn block;
    void *block_context;
};

int jpeg_frame_prepare(struct jpeg_frame_workspace *workspace,
                       const uint8_t *source, uint32_t length);
int jpeg_frame_validate(struct jpeg_frame_workspace *workspace,
                        const uint8_t *source, uint32_t length);
int jpeg_frame_decode(struct jpeg_frame_workspace *workspace,
                      const uint8_t *source, uint32_t length,
                      jpeg_frame_block_fn block, void *context);
int jpeg_frame_decode_region(struct jpeg_frame_workspace *workspace,
                             const uint8_t *source, uint32_t length,
                             jpeg_frame_block_fn block, void *context,
                             const struct jpeg_frame_region *region);

#ifdef __cplusplus
}
#endif

#endif
