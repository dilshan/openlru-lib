/**
 * @file olru_image.c
 * @brief Export of raw sensor images as BMP files.
 * @author Dilshan R Jayakody (jayakody2000lk@gmail.com)
 *
 * The BMP is an 8 bit palettised bitmap with a 256 entry grey ramp:
 *
 *   14 byte file header | 40 byte DIB header | 1024 byte palette | pixels
 *
 * Like the vendor SDK, the raw rows are copied unchanged, so a viewer shows
 * exactly what the SDK demo shows.
 */

#include "olru_internal.h"

#include <stdio.h>
#include <string.h>

#define FILE_HEADER_SIZE 14
#define DIB_HEADER_SIZE  40
#define PALETTE_SIZE     1024
#define PIXEL_OFFSET     (FILE_HEADER_SIZE + DIB_HEADER_SIZE + PALETTE_SIZE)
#define PPM_500_DPI      19685          /* pixels per metre at 500 dpi */

/** Store @p value as @p bytes little endian bytes. */
static void put_le(uint8_t *p, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; i++)
        p[i] = (uint8_t)(value >> (8 * i));
}

/** "BM" file header: signature, total size, offset of the pixel data. */
static void write_file_header(uint8_t *p)
{
    memset(p, 0, FILE_HEADER_SIZE);
    p[0] = 'B';
    p[1] = 'M';
    put_le(p + 2, OLRU_BMP_SIZE, 4);
    put_le(p + 10, PIXEL_OFFSET, 4);
}

/** BITMAPINFOHEADER describing a 256 x 288, 8 bit, 500 dpi image. */
static void write_dib_header(uint8_t *p)
{
    memset(p, 0, DIB_HEADER_SIZE);
    put_le(p + 0, DIB_HEADER_SIZE, 4);
    put_le(p + 4, OLRU_IMAGE_WIDTH, 4);
    put_le(p + 8, OLRU_IMAGE_HEIGHT, 4);
    put_le(p + 12, 1, 2);                   /* colour planes   */
    put_le(p + 14, 8, 2);                   /* bits per pixel  */
    put_le(p + 20, OLRU_IMAGE_SIZE, 4);     /* pixel data size */
    put_le(p + 24, PPM_500_DPI, 4);
    put_le(p + 28, PPM_500_DPI, 4);
    put_le(p + 32, 256, 4);                 /* palette entries */
}

/** 256 grey levels, stored as blue, green, red, unused. */
static void write_palette(uint8_t *p)
{
    for (int level = 0; level < 256; level++) {
        p[level * 4 + 0] = (uint8_t)level;
        p[level * 4 + 1] = (uint8_t)level;
        p[level * 4 + 2] = (uint8_t)level;
        p[level * 4 + 3] = 0;
    }
}

int olru_image_to_bmp(const uint8_t *raw, uint8_t *out, size_t out_size)
{
    if (!raw || !out || out_size < OLRU_BMP_SIZE)
        return OLRU_ERR_ARG;
    write_file_header(out);
    write_dib_header(out + FILE_HEADER_SIZE);
    write_palette(out + FILE_HEADER_SIZE + DIB_HEADER_SIZE);
    memcpy(out + PIXEL_OFFSET, raw, OLRU_IMAGE_SIZE);
    return OLRU_BMP_SIZE;
}

int olru_image_save_bmp(const uint8_t *raw, const char *path)
{
    uint8_t bmp[OLRU_BMP_SIZE];          /* 74 KB: fine on the stack */
    FILE *f;
    int n = olru_image_to_bmp(raw, bmp, sizeof bmp);

    if (n < 0 || !path)
        return n < 0 ? n : OLRU_ERR_ARG;
    f = fopen(path, "wb");
    if (!f)
        return OLRU_ERR_IO;
    n = fwrite(bmp, 1, OLRU_BMP_SIZE, f) == OLRU_BMP_SIZE ? OLRU_OK : OLRU_ERR_IO;
    if (fclose(f) != 0)
        n = OLRU_ERR_IO;
    return n;
}
