/*
 * imgcodec.h -- image files other than GIF (GIF is gif.c's, Icon's own):
 * PNG, JPEG and BMP by stb_image and stb_image_write (deps/stb, public
 * domain), QOI by its specification (qoiformat.org). In memory: the
 * caller reads and writes the files.
 */
#include <stddef.h>
#include <stdint.h>

/* img_decode decodes a PNG, JPEG, BMP or QOI file's bytes into w * h
 * pixels 0xAARRGGBB (straight alpha), malloc'd; NULL if the bytes are
 * none of these, or broken. */
uint32_t *img_decode(const unsigned char *data, size_t n, int *w, int *h);

/* img_encode encodes w * h pixels 0x00RRGGBB as the format the file
 * extension ext names (png, jpg or jpeg, bmp, qoi; any case): 1 encoded
 * (*out malloc'd, *len bytes), 0 failed, -1 not a format it writes. */
int img_encode(const char *ext, const uint32_t *px, int w, int h, unsigned char **out, size_t *len);

/* img_writes reports whether img_encode writes the format ext names. */
int img_writes(const char *ext);
