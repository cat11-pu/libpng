/*
 * png_decode.c - RGBA8 decode driver for the gamma / transparency study.
 *
 * Two build modes share the exact same libpng call skeleton; only the two
 * behaviours under study differ:
 *
 *   DECIDE_GAMMA (defined => correct build)
 *     Use the file gamma from the gAMA chunk (sRGB chunk implies the sRGB
 *     transfer function) and ask libpng for an sRGB-encoded output.  When no
 *     gamma information is present the PNG spec default (1/0.45455) is used.
 *
 *     When undefined (buggy build) the driver ignores the file gamma and feeds
 *     a hard-wired exponent to png_set_gamma, which double-applies / inverts
 *     the transfer function and yields dark output.
 *
 *   ZERO_TRANSPARENT (defined => correct build)
 *     After decoding, every fully transparent pixel (alpha == 0) gets its
 *     R,G,B components cleared to zero so no stale palette/tRNS color can leak
 *     through the transparent edge.
 *
 *     When undefined (buggy build) the original component bytes are retained,
 *     so transparent pixels keep arbitrary "fringe" colors.
 *
 * Output: a flat RGBA8 raster (width*height*4 bytes) written to the path given
 * as the second argument, plus one summary line on stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "png.h"

struct jmp_state {
    jmp_buf jb;
};

static void
fail_cb(png_structp png_ptr, png_const_charp msg)
{
    struct jmp_state *js = png_get_error_ptr(png_ptr);
    fprintf(stderr, "libpng error: %s\n", msg);
    longjmp(js->jb, 1);
}

static void
warn_cb(png_structp png_ptr, png_const_charp msg)
{
    (void)png_ptr;
    fprintf(stderr, "libpng warning: %s\n", msg);
}

/* PNG spec default gAMA exponent for images that carry no gAMA/sRGB chunk. */
#define DEFAULT_GAMA_EXPONENT 0.45455
/* sRGB output encoding requested from libpng (screen gamma). */
#define OUTPUT_SRGB_GAMMA  2.2

int
main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s input.png output.rgba\n", argv[0]);
        return 2;
    }

    FILE *in = fopen(argv[1], "rb");
    if (in == NULL) {
        perror(argv[1]);
        return 2;
    }

    struct jmp_state js;
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                                 &js, fail_cb, warn_cb);
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (png_ptr == NULL || info_ptr == NULL) {
        fprintf(stderr, "png_create_* failed\n");
        fclose(in);
        return 2;
    }

    if (setjmp(js.jb)) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(in);
        return 1;
    }

    png_init_io(png_ptr, in);
    png_read_info(png_ptr, info_ptr);

    png_uint_32 width, height;
    int bit_depth, color_type, interlace_type;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth,
                 &color_type, &interlace_type, NULL, NULL);

    /* Normalise every input to 8-bit RGB(A). */
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    if (bit_depth == 16)
        png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    /*
     * Gamma handling.
     */
#ifdef DECIDE_GAMMA
    /* Correct: honour the file's own transfer function. */
    double gama_exponent;
    if (png_get_gAMA(png_ptr, info_ptr, &gama_exponent)) {
        /* png_get_gAMA returns the raw gAMA exponent (0.45455 for an sRGB
         * encoded file).  Pass it straight through; libpng derives the table
         * exponent screen/gAMA internally.
         */
        png_set_gamma(png_ptr, OUTPUT_SRGB_GAMMA, gama_exponent);
    } else {
        /* No gAMA chunk: assume the PNG spec default sRGB encoding. */
        png_set_gamma(png_ptr, OUTPUT_SRGB_GAMMA, DEFAULT_GAMA_EXPONENT);
    }
#else
    /*
     * Buggy: ignore gAMA entirely and ask for a linear (screen gamma 1.0)
     * result while still treating the samples as if they were sRGB.  libpng
     * therefore runs the full linearisation table on already-encoded values,
     * which is the classic "decoded image is too dark" failure.
     */
    png_set_gamma(png_ptr, 1.0, DEFAULT_GAMA_EXPONENT);
#endif

    /* Guarantee a trailing alpha byte so every pixel is a fixed RGBA quad. */
    png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    if (interlace_type != PNG_INTERLACE_NONE)
        png_set_interlace_handling(png_ptr);
    png_read_update_info(png_ptr, info_ptr);

    size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    png_bytep *rows = malloc(sizeof(png_bytep) * (size_t)height);
    if (rows == NULL) {
        fprintf(stderr, "out of memory\n");
        longjmp(js.jb, 1);
    }
    for (png_uint_32 y = 0; y < height; y++) {
        rows[y] = malloc(rowbytes);
        if (rows[y] == NULL) {
            fprintf(stderr, "out of memory\n");
            longjmp(js.jb, 1);
        }
    }

    png_read_image(png_ptr, rows);
    png_read_end(png_ptr, NULL);

#ifdef ZERO_TRANSPARENT
    /* Clear the color channels of every fully transparent pixel. */
    for (png_uint_32 y = 0; y < height; y++) {
        png_bytep q = rows[y];
        for (png_uint_32 x = 0; x < width; x++, q += 4) {
            if (q[3] == 0) {
                q[0] = 0;
                q[1] = 0;
                q[2] = 0;
            }
        }
    }
#endif

    FILE *out = fopen(argv[2], "wb");
    if (out == NULL) {
        perror(argv[2]);
        longjmp(js.jb, 1);
    }
    for (png_uint_32 y = 0; y < height; y++) {
        if (fwrite(rows[y], 1, rowbytes, out) != rowbytes) {
            fprintf(stderr, "short write\n");
            fclose(out);
            longjmp(js.jb, 1);
        }
    }
    fclose(out);

    printf("OK %s %ux%u gamma=%s zero=%s -> %s\n", argv[1], width, height,
#ifdef DECIDE_GAMMA
           "correct",
#else
           "buggy",
#endif
#ifdef ZERO_TRANSPARENT
           "on",
#else
           "off",
#endif
           argv[2]);

    for (png_uint_32 y = 0; y < height; y++)
        free(rows[y]);
    free(rows);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(in);
    return 0;
}
