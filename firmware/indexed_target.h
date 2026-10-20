#ifndef INDEXED_TARGET_H
#define INDEXED_TARGET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int indexed_target_control(void *, const char *, uint32_t);
int indexed_target_paint(const void *);
int indexed_target_has_canvas(const void *);
int indexed_target_dispose(void *);
int indexed_target_binary_begin(void *, uint32_t);
uint32_t indexed_target_binary_feed(void *, const uint8_t *, uint32_t);
uint32_t indexed_target_idle_ticks(uint32_t);
uint32_t indexed_gui_ready(void);

void indexed_native_invalidate(uint32_t, uint32_t);
int indexed_native_eligible(void);
struct indexed_stream_region;
int indexed_native_clip(struct indexed_stream_region *);
int indexed_native_bitmap(const uint8_t *, uint32_t, uint32_t,
                           uint32_t, uint32_t, const uint32_t *);

#ifdef __cplusplus
}
#endif

#endif
