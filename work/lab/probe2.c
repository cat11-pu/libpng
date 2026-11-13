#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "png.h"

struct user_err {
    jmp_buf jb;
};

static void err_exit(png_structp p, png_const_charp msg)
{
    struct user_err *e = png_get_error_ptr(p);
    fprintf(stderr, "libpng error: %s\n", msg);
    longjmp(e->jb, 1);
}

static void warn_cb(png_structp p, png_const_charp msg)
{
    (void)p;
    fprintf(stderr, "libpng warning: %s\n", msg);
}

static void decode(const char *path, int do_gamma, double screen_gamma)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }

    struct user_err err;
    png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, &err,
                                           err_exit, warn_cb);
    png_infop info = png_create_info_struct(p);
    if (setjmp(err.jb)) {
        png_destroy_read_struct(&p, &info, NULL);
        fclose(f);
        exit(1);
    }

    png_init_io(p, f);
    png_read_info(p, info);

    png_uint_32 w, h;
    int bd, ct, it;
    png_get_IHDR(p, info, &w, &h, &bd, &ct, &it, NULL, NULL);

    double file_gamma = 0.0;
    int have_gamma = png_get_gAMA(p, info, &file_gamma);

    if (ct == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(p);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8)
        png_set_expand_gray_1_2_4_to_8(p);
    if (png_get_valid(p, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(p);
    if (bd == 16)
        png_set_strip_16(p);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(p);
    if (do_gamma)
        png_set_gamma(p, screen_gamma, have_gamma ? file_gamma : 0.45455);
    png_set_filler(p, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(p, info);

    size_t rowbytes = png_get_rowbytes(p, info);
    png_bytep *rows = malloc(sizeof(png_bytep) * h);
    for (png_uint_32 y = 0; y < h; y++)
        rows[y] = malloc(rowbytes);
    png_read_image(p, rows);
    png_read_end(p, NULL);

    printf("%s gamma=%d screen=%g file_gamma=%g ->",
           path, do_gamma, screen_gamma, have_gamma ? file_gamma : -1.0);
    for (png_uint_32 x = 0; x < w && x < 8; x++) {
        png_bytep q = rows[0] + 4 * x;
        printf(" (%d,%d,%d,%d)", q[0], q[1], q[2], q[3]);
    }
    printf("\n");

    for (png_uint_32 y = 0; y < h; y++)
        free(rows[y]);
    free(rows);
    png_destroy_read_struct(&p, &info, NULL);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.png\n", argv[0]);
        return 2;
    }
    decode(argv[1], 0, 0.0);
    decode(argv[1], 1, 2.2);
    decode(argv[1], 1, 1.0);
    return 0;
}
