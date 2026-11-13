#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "png.h"
#include "pngpriv.h"

#define MAX_DIM 65536
#define MAX_ROWS 30000

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t pos;
} mem_reader;

typedef struct {
    unsigned char type[5];
    png_uint_32 length;
    png_uint_32 data_off;
    png_uint_32 crc_off;
    png_uint_32 total;
} chunk_info;

static mem_reader g_reader;
static chunk_info g_chunks[4096];
static int g_nchunks;
static png_bytep g_rowbuf;
static png_bytepp g_rows;
static png_uint_32 g_alloc_rows;

static void free_pixel_buffers(void)
{
    free(g_rowbuf);
    g_rowbuf = NULL;
    if (g_rows) {
        for (png_uint_32 y = 0; y < g_alloc_rows; y++)
            free(g_rows[y]);
        free(g_rows);
        g_rows = NULL;
        g_alloc_rows = 0;
    }
}

static void read_cb(png_structp png_ptr, png_bytep out, png_size_t len)
{
    mem_reader *r = (mem_reader *)png_get_io_ptr(png_ptr);
    if (r->pos + len > r->size)
        png_error(png_ptr, "read past end of input");
    memcpy(out, r->data + r->pos, len);
    r->pos += len;
}

static int chunk_cb(png_structp png_ptr, png_unknown_chunkp chunk)
{
    (void)png_ptr;
    (void)chunk;
    return 0;
}

static int parse_chunk_table(const unsigned char *d, size_t n)
{
    g_nchunks = 0;
    if (n < 8 || png_sig_cmp(d, 0, 8) != 0)
        return -1;
    size_t p = 8;
    while (p + 12 <= n && g_nchunks < 4096) {
        png_uint_32 len = ((png_uint_32)d[p] << 24) | ((png_uint_32)d[p+1] << 16) |
                          ((png_uint_32)d[p+2] << 8) | d[p+3];
        chunk_info *c = &g_chunks[g_nchunks];
        memcpy(c->type, d + p + 4, 4);
        c->type[4] = 0;
        c->length = len;
        c->data_off = (png_uint_32)(p + 8);
        c->crc_off = (png_uint_32)(p + 8 + len);
        c->total = 12 + len;
        p += (size_t)12 + len;
        if (p > n)
            return g_nchunks + 1;
        g_nchunks++;
        if (memcmp(c->type, "IEND", 4) == 0)
            break;
    }
    return g_nchunks;
}

static void locate(png_uint_32 off, char *buf, size_t n)
{
    int last = -1;
    for (int i = 0; i < g_nchunks; i++) {
        chunk_info *c = &g_chunks[i];
        if (c->data_off <= off)
            last = i;
        if (off >= c->data_off && off < c->data_off + c->length)
            snprintf(buf, n, "%s+0x%x", c->type, off - c->data_off);
        else if (off >= c->crc_off && off < c->crc_off + 4)
            snprintf(buf, n, "%s(CRC)+0x%x", c->type, off - c->crc_off);
        else if (off + 8 >= c->data_off && off < c->data_off)
            snprintf(buf, n, "%s(header)", c->type);
    }
    if (buf[0] == 0)
        if (last >= 0) {
            chunk_info *c = &g_chunks[last];
            snprintf(buf, n, "%s(framing)+0x%x", c->type, off - c->data_off);
        } else if (off < 8) {
            snprintf(buf, n, "signature+0x%x", off);
        } else {
            snprintf(buf, n, "eof+0x%x", off);
        }
}

static void write_ppm_or_pgm(const char *path, png_structp png, png_infop info,
                             png_bytepp rows, png_uint_32 w, png_uint_32 h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    size_t rowbytes = png_get_rowbytes(png, info);
    if (fprintf(f, "P6\n%u %u\n255\n", w, h) < 0) { perror(path); exit(2); }
    for (png_uint_32 y = 0; y < h; y++) {
        const png_byte *p = rows[y];
        for (size_t x = 0; x < w; x++) {
            if (fwrite(p + x * 4, 1, 3, f) != 3) { perror(path); exit(2); }
        }
        (void)rowbytes;
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s decode|ppm <file> [out.ppm]\n", argv[0]);
        return 2;
    }
    int do_ppm = strcmp(argv[1], "ppm") == 0;
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { perror("ftell"); return 2; }
    unsigned char *buf = malloc((size_t)sz + 1);
    if (!buf) { fprintf(stderr, "OOM allocating input\n"); return 2; }
    fseek(f, 0, SEEK_SET);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); return 2; }
    fclose(f);

    g_reader.data = buf;
    g_reader.size = (size_t)sz;
    g_reader.pos = 0;
    parse_chunk_table(buf, (size_t)sz);

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fprintf(stderr, "png_create_read_struct failed\n"); free(buf); return 2; }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        free(buf);
        fprintf(stderr, "png_create_info_struct failed\n");
        return 2;
    }

    if (setjmp(png_jmpbuf(png))) {
        char where[160] = {0};
        png_uint_32 off = (png_uint_32)g_reader.pos;
        locate(off, where, sizeof(where));
        fprintf(stderr, "REJECT file=%s chunk=%s file_offset=0x%x libpng=error\n",
                argv[2], where, off);
        free_pixel_buffers();
        png_destroy_read_struct(&png, &info, NULL);
        free(buf);
        return 1;
    }

    png_set_read_fn(png, &g_reader, read_cb);
    png_set_read_user_chunk_fn(png, NULL, chunk_cb);
    png_set_keep_unknown_chunks(png, PNG_HANDLE_CHUNK_ALWAYS, NULL, -1);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info);
    png_uint_32 h = png_get_image_height(png, info);
    int bit_depth = png_get_bit_depth(png, info);
    int color_type = png_get_color_type(png, info);

    if (w > MAX_DIM || h > MAX_DIM || h > (png_uint_32)MAX_ROWS ||
        (double)w * h > 4.0e8)
        png_error(png, "test safety limit on image dimensions");

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (bit_depth == 16)
        png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);

    size_t rowbytes = png_get_rowbytes(png, info);
    g_rowbuf = malloc(rowbytes);
    if (do_ppm) {
        g_alloc_rows = h;
        g_rows = calloc((size_t)h, sizeof(png_bytep));
        if (!g_rows) png_error(png, "OOM row pointers");
        for (png_uint_32 y = 0; y < h; y++) {
            g_rows[y] = malloc(rowbytes);
            if (!g_rows[y]) png_error(png, "OOM row buffer");
        }
    }
    if (!g_rowbuf) png_error(png, "OOM row buffer");

    if (do_ppm) {
        png_read_image(png, g_rows);
    } else if (passes > 1) {
        for (int pass = 0; pass < passes; pass++)
            for (png_uint_32 y = 0; y < h; y++)
                png_read_row(png, g_rowbuf, NULL);
    } else {
        for (png_uint_32 y = 0; y < h; y++)
            png_read_row(png, g_rowbuf, NULL);
    }
    png_read_end(png, NULL);

    printf("OK file=%s %ux%u depth=%d ctype=%d chunks=%d bytes=%ld\n",
           argv[2], w, h, bit_depth, color_type, g_nchunks, sz);

    if (do_ppm) {
        write_ppm_or_pgm(argv[3], png, info, g_rows, w, h);
    }
    free_pixel_buffers();
    png_destroy_read_struct(&png, &info, NULL);
    free(buf);
    return 0;
}
