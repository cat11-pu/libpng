#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "png.h"

static void read_png(const char* path, int do_gamma, double screen_gamma) {
    FILE* f = fopen(path, "rb");
    unsigned char hdr[8];
    fread(hdr, 1, 8, f);
    png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    png_infop info = png_create_info_struct(p);
    if (setjmp(png_jmpbuf(p))) { png_error(p, "fail"); }
    png_init_io(p, f);
    png_read_info(p, info);
    png_uint_32 w, h; int bd, ct, it;
    png_get_IHDR(p, info, &w, &h, &bd, &ct, &it, 0, 0);
    double fg;
    if (png_get_gAMA(p, info, &fg)) printf("%s: file gamma=%g\n", path, fg);
    if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(p);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(p);
    if (png_get_valid(p, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(p);
    if (bd == 16) png_set_strip_16(p);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(p);
    if (do_gamma) png_set_gamma(p, screen_gamma, fg > 0 ? fg : 0.45455);
    png_set_filler(p, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(p, info);
    size_t rb = png_get_rowbytes(p, info);
    unsigned char* row = malloc(rb);
    png_read_image(p, &row);
    printf("  gamma=%d screen=%g ->", do_gamma, screen_gamma);
    for (png_uint_32 x = 0; x < w && x < 6; x++) printf(" (%d,%d,%d,%d)", row[4*x],row[4*x+1],row[4*x+2],row[4*x+3]);
    printf("\n");
    free(row); fclose(f);
    png_destroy_read_struct(&p, &info, 0);
}

int main(int argc, char** argv) {
    read_png(argv[1], 0, 0.0);
    read_png(argv[1], 1, 2.2);
    read_png(argv[1], 1, 1.0);
    return 0;
}
