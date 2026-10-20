#include "jpeg_frame.h"

static uint32_t be16(const uint8_t *data)
{
    return ((uint32_t)data[0] << 8) | data[1];
}

static int quantizers(const uint8_t *data, uint32_t length, uint32_t *seen)
{
    uint32_t index, mask;
    while (length) {
        if (length < 65u || data[0] > 3u) return 0;
        mask = 1u << data[0];
        if (*seen & mask) return 0;
        *seen |= mask;
        for (index = 1; index <= 64u; index++) {
            if (!data[index]) return 0;
        }
        data += 65;
        length -= 65;
    }
    return 1;
}

static int huffman(const uint8_t *data, uint32_t length, uint32_t *seen)
{
    uint32_t kind, mask, index, count, room, symbol, bits;
    uint32_t symbols[8];
    while (length) {
        if (length < 17u || (data[0] & 0xeeu)) return 0;
        kind = data[0] >> 4;
        mask = 1u << ((data[0] & 1u) + 2u * kind);
        if (*seen & mask) return 0;
        *seen |= mask;
        count = 0;
        room = 1;
        for (index = 1; index <= 16u; index++) {
            room <<= 1;
            if (data[index] >= room) return 0;
            room -= data[index];
            count += data[index];
        }
        if (!count || count > (kind ? 162u : 12u) || count > length - 17u) return 0;
        for (index = 0; index < 8u; index++) symbols[index] = 0;
        for (index = 0; index < count; index++) {
            symbol = data[17u + index];
            bits = symbol & 15u;
            if (kind) {
                if (bits > 10u || (!bits && symbol != 0u && symbol != 0xf0u)) return 0;
            } else if (symbol > 11u) {
                return 0;
            }
            mask = 1u << (symbol & 31u);
            if (symbols[symbol >> 5] & mask) return 0;
            symbols[symbol >> 5] |= mask;
        }
        data += 17u + count;
        length -= 17u + count;
    }
    return 1;
}

static int envelope(const uint8_t *source, uint32_t length)
{
    uint32_t offset = 2, marker, size, index, scan, tables = 0, quants = 0;
    uint32_t sof = 0, restart = 0, has_restart = 0, restart_index = 0;
    uint32_t required_quants = 0;
    const uint8_t *segment;
    if (length < 4u || be16(source) != 0xffd8u || be16(source + length - 2u) != 0xffd9u) return 0;
    while (offset < length - 2u) {
        if (length - offset < 4u || source[offset] != 0xffu) return 0;
        marker = source[offset + 1u];
        size = be16(source + offset + 2u);
        offset += 4u;
        if (size < 2u || size - 2u > length - offset) return 0;
        size -= 2u;
        segment = source + offset;
        offset += size;
        if (marker == 0xdbu) {
            if (!quantizers(segment, size, &quants)) return 0;
        } else if (marker == 0xc4u) {
            if (!huffman(segment, size, &tables)) return 0;
        } else if (marker == 0xc0u) {
            if (sof || size != 15u || segment[0] != 8u || segment[5] != 3u ||
                be16(segment + 1u) != JPEG_FRAME_HEIGHT || be16(segment + 3u) != JPEG_FRAME_WIDTH) return 0;
            if (segment[7] != 0x11u && segment[7] != 0x21u && segment[7] != 0x22u) return 0;
            for (index = 0; index < 3u; index++) {
                scan = index * 3u;
                if (segment[6u + scan] != index + 1u || segment[8u + scan] > 3u ||
                    (index && segment[7u + scan] != 0x11u)) return 0;
                required_quants |= 1u << segment[8u + scan];
            }
            sof = 1;
        } else if (marker == 0xddu) {
            if (has_restart || size != 2u) return 0;
            has_restart = 1;
            restart = be16(segment);
        } else if (marker == 0xdau) {
            if (!sof || size != 10u || segment[0] != 3u || tables != 15u ||
                (quants & required_quants) != required_quants ||
                segment[7] != 0u || segment[8] != 63u || segment[9] != 0u) return 0;
            for (index = 0; index < 3u; index++) {
                if (segment[1u + index * 2u] != index + 1u ||
                    segment[2u + index * 2u] != (index ? 0x11u : 0u)) return 0;
            }
            while (offset < length - 2u) {
                if (source[offset++] != 0xffu) continue;
                if (offset >= length - 2u) return 0;
                marker = source[offset++];
                if (!marker) continue;
                if (!restart || marker != 0xd0u + restart_index) return 0;
                restart_index = (restart_index + 1u) & 7u;
            }
            return offset == length - 2u;
        } else if ((marker < 0xe0u || marker > 0xefu) && marker != 0xfeu) {
            return 0;
        }
    }
    return 0;
}

static size_t input(JDEC *decoder, uint8_t *buffer, size_t requested)
{
    struct jpeg_frame_workspace *workspace = static_cast<struct jpeg_frame_workspace *>(decoder->device);
    size_t available = workspace->length - workspace->offset;
    size_t index;
    if (requested > available) requested = available;
    if (buffer) {
        for (index = 0; index < requested; index++) {
            buffer[index] = workspace->source[workspace->offset + index];
        }
    }
    workspace->offset += (uint32_t)requested;
    return requested;
}

static int output(JDEC *decoder, void *bitmap, JRECT *rectangle)
{
    struct jpeg_frame_workspace *workspace = static_cast<struct jpeg_frame_workspace *>(decoder->device);
    uint32_t width = (uint32_t)decoder->msx * 8u;
    uint32_t height = (uint32_t)decoder->msy * 8u;
    uint32_t count = width * height;
    if (workspace->next_y >= JPEG_FRAME_HEIGHT || bitmap != decoder->workbuf ||
        rectangle->left != workspace->next_x || rectangle->top != workspace->next_y ||
        rectangle->right != workspace->next_x + width - 1u ||
        rectangle->bottom != workspace->next_y + height - 1u ||
        rectangle->right >= JPEG_FRAME_WIDTH || rectangle->bottom >= JPEG_FRAME_HEIGHT ||
        count > 256u || workspace->pixels > JPEG_FRAME_WIDTH * JPEG_FRAME_HEIGHT - count) {
        workspace->output_failed = 1;
        return 0;
    }
    if (workspace->block && !workspace->block(workspace->block_context, static_cast<const uint16_t *>(bitmap),
                                             rectangle->left, rectangle->top, width, height)) {
        workspace->output_failed = 1;
        return 0;
    }
    workspace->pixels += count;
    workspace->next_x += width;
    if (workspace->next_x == JPEG_FRAME_WIDTH) {
        workspace->next_x = 0;
        workspace->next_y += height;
    }
    return 1;
}

int jpeg_frame_prepare(struct jpeg_frame_workspace *workspace,
                       const uint8_t *source, uint32_t length)
{
    if (!workspace) return JPEG_FRAME_ARGUMENT;
    workspace->prepared = 0;
    workspace->work_used = 0;
    workspace->decoder_result = JDR_PAR;
    if (!source || length > JPEG_FRAME_MAX_LENGTH || length < 4u) return JPEG_FRAME_ARGUMENT;
    if (!envelope(source, length)) return JPEG_FRAME_FORMAT;
    workspace->source = source;
    workspace->length = length;
    workspace->offset = 0;
    workspace->next_x = 0;
    workspace->next_y = 0;
    workspace->pixels = 0;
    workspace->output_failed = 0;
    workspace->block = 0;
    workspace->block_context = 0;
    workspace->decoder_result = jd_prepare(&workspace->decoder, input, workspace->pool,
                                           sizeof(workspace->pool), workspace);
    if (workspace->decoder_result != JDR_OK) return JPEG_FRAME_DECODER;
    workspace->work_used = JPEG_FRAME_POOL_BYTES - (uint32_t)workspace->decoder.sz_pool;
    if (workspace->decoder.width != JPEG_FRAME_WIDTH || workspace->decoder.height != JPEG_FRAME_HEIGHT ||
        workspace->decoder.ncomp != 3u) return JPEG_FRAME_FORMAT;
    workspace->prepared = 1;
    return JPEG_FRAME_OK;
}

static int finish(struct jpeg_frame_workspace *workspace, int validate_only,
                   const JRECT *region)
{
    uint32_t consumed, bits, mask;
    workspace->decoder_result = validate_only ? jd_validate(&workspace->decoder, output) :
        region ? jd_decomp_region(&workspace->decoder, output, region) :
            jd_decomp(&workspace->decoder, output, 0);
    workspace->prepared = 0;
    if (workspace->output_failed) return JPEG_FRAME_OUTPUT;
    if (workspace->decoder_result != JDR_OK) return JPEG_FRAME_DECODER;
    if (workspace->pixels != JPEG_FRAME_WIDTH * JPEG_FRAME_HEIGHT || workspace->next_x ||
        workspace->next_y != JPEG_FRAME_HEIGHT) return JPEG_FRAME_FORMAT;
    consumed = workspace->offset - (uint32_t)workspace->decoder.dctr;
    bits = workspace->decoder.dbit;
    if (bits > 31u) return JPEG_FRAME_FORMAT;
    mask = bits ? ((1u << bits) - 1u) : 0u;
    if ((workspace->decoder.wreg & mask) != mask) return JPEG_FRAME_FORMAT;
    if (workspace->decoder.marker == 0xd9u) {
        if (consumed != workspace->length) return JPEG_FRAME_FORMAT;
    } else if (workspace->decoder.marker || consumed != workspace->length - 2u || bits > 7u) {
        return JPEG_FRAME_FORMAT;
    }
    return JPEG_FRAME_OK;
}

int jpeg_frame_validate(struct jpeg_frame_workspace *workspace,
                        const uint8_t *source, uint32_t length)
{
    int result = jpeg_frame_prepare(workspace, source, length);
    return result == JPEG_FRAME_OK ? finish(workspace, 1, NULL) : result;
}

int jpeg_frame_decode(struct jpeg_frame_workspace *workspace,
                      const uint8_t *source, uint32_t length,
                      jpeg_frame_block_fn block, void *context)
{
    int result;
    if (!block) return JPEG_FRAME_ARGUMENT;
    result = jpeg_frame_prepare(workspace, source, length);
    if (result != JPEG_FRAME_OK) return result;
    workspace->block = block;
    workspace->block_context = context;
    return finish(workspace, 0, NULL);
}

struct region_output {
    jpeg_frame_block_fn block;
    void *context;
    struct jpeg_frame_region region;
};

static int output_region(void *context, const uint16_t *pixels,
                          uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
    struct region_output *output = static_cast<struct region_output *>(context);
    if (left > output->region.right || left + width <= output->region.left ||
        top > output->region.bottom || top + height <= output->region.top)
        pixels = NULL;
    return output->block(output->context, pixels, left, top, width, height);
}

int jpeg_frame_decode_region(struct jpeg_frame_workspace *workspace,
                             const uint8_t *source, uint32_t length,
                             jpeg_frame_block_fn block, void *context,
                             const struct jpeg_frame_region *region)
{
    struct region_output output;
    JRECT rectangle;
    int result;
    if (!block || !region || region->left > region->right || region->top > region->bottom ||
        region->right >= JPEG_FRAME_WIDTH || region->bottom >= JPEG_FRAME_HEIGHT)
        return JPEG_FRAME_ARGUMENT;
    output.block = block;
    output.context = context;
    output.region = *region;
    rectangle.left = (uint16_t)region->left;
    rectangle.top = (uint16_t)region->top;
    rectangle.right = (uint16_t)region->right;
    rectangle.bottom = (uint16_t)region->bottom;
    result = jpeg_frame_prepare(workspace, source, length);
    if (result != JPEG_FRAME_OK) return result;
    workspace->block = output_region;
    workspace->block_context = &output;
    result = finish(workspace, 0, &rectangle);
    workspace->block = block;
    workspace->block_context = context;
    return result;
}
