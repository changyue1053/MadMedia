#ifndef MADMEDIA_IOS_IMAGE_PREVIEW_H
#define MADMEDIA_IOS_IMAGE_PREVIEW_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* iOS ImageIO preview only. JPEG -> oriented, no-upscale JPEG.
 * Input <= 8 MiB / 4,000,000 pixels / 8192 pixels per edge. Output edge
 * 1..2048; quality 1..100. Memory only; no source metadata forwarded.
 * out_size and output storage must not overlap input or each other.
 * NULL output with capacity 0 queries size and returns CAPACITY on success.
 * Only OK writes output bytes; CAPACITY reports required size, others zero.
 * Preview interface, not a stable SDK ABI. */
enum {
  MADMEDIA_IMAGE_V1_OK = 0, MADMEDIA_IMAGE_V1_ARGUMENT = 1,
  MADMEDIA_IMAGE_V1_LIMIT = 2, MADMEDIA_IMAGE_V1_DECODE = 3,
  MADMEDIA_IMAGE_V1_ENCODE = 4, MADMEDIA_IMAGE_V1_CAPACITY = 5
};
int32_t madmedia_ios_jpeg_resize_v1(const uint8_t *input, size_t size,
                                   uint32_t edge, uint32_t quality,
                                   uint8_t *output, size_t capacity, size_t *out_size);
#ifdef __cplusplus
}
#endif
#endif
