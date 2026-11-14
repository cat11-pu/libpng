/* region_test.c - correctness and memory benchmark for png_set_read_region.
 *
 * Modes:
 *   gen <w> <h> <dir>                      generate test PNGs
 *   check <file> <x> <y> <w> <h>           region decode == full decode + crop
 *   memfull <file>                         memory stats for a full decode
 *   memregion <file> <x> <y> <w> <h>       memory stats for a region decode
 *
 * Build: gcc -O2 -o region_test region_test.c -Ibuild -I. build/libpng16.a -lz
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "png.h"

/* Same computation as the library-internal PNG_ROWBYTES macro. */
#define RT_ROWBYTES(pixel_depth, width) \
   ((pixel_depth) >= 8 ? \
       (size_t)(width) * ((pixel_depth) >> 3) : \
       ((size_t)(width) * (pixel_depth) + 7) >> 3)

/* ------------------------------------------------------------------ */
/* Tracked allocator: counts every byte libpng (and the app image      */
/* buffer) has live, keeping the peak.                                 */
/* ------------------------------------------------------------------ */
static size_t trk_cur, trk_peak;

static png_voidp PNGAPI
trk_malloc(png_structrp png_ptr, png_alloc_size_t size)
{
   size_t *p;
   (void)png_ptr;
   p = (size_t *)malloc(size + sizeof(size_t));
   if (p == NULL)
      return NULL;
   *p = size;
   trk_cur += size;
   if (trk_cur > trk_peak)
      trk_peak = trk_cur;
   return p + 1;
}

static void PNGAPI
trk_free(png_structrp png_ptr, png_voidp ptr)
{
   size_t *p;
   (void)png_ptr;
   if (ptr == NULL)
      return;
   p = (size_t *)ptr - 1;
   trk_cur -= *p;
   free(p);
}

static void *
app_alloc(size_t size)
{
   size_t *p = (size_t *)malloc(size + sizeof(size_t));
   if (p == NULL)
   {
      fprintf(stderr, "out of memory\n");
      exit(2);
   }
   *p = size;
   trk_cur += size;
   if (trk_cur > trk_peak)
      trk_peak = trk_cur;
   return p + 1;
}

static void
app_free(void *ptr)
{
   size_t *p;
   if (ptr == NULL)
      return;
   p = (size_t *)ptr - 1;
   trk_cur -= *p;
   free(p);
}

static long
max_rss_kb(void)
{
   struct rusage ru;
   getrusage(RUSAGE_SELF, &ru);
   return ru.ru_maxrss;
}

/* ------------------------------------------------------------------ */
/* Test image generation                                               */
/* ------------------------------------------------------------------ */
static unsigned int rng_state = 12345;

static unsigned int
next_rand(void)
{
   rng_state = rng_state * 1103515245u + 12345u;
   return (rng_state >> 16) & 0x7fffu;
}

static int
write_png(const char *name, png_uint_32 w, png_uint_32 h, int color_type,
    int bit_depth, int interlaced)
{
   FILE *fp = fopen(name, "wb");
   png_structp png_ptr;
   png_infop info_ptr;
   png_bytep row;
   png_uint_32 y;
   size_t rowbytes;
   int channels = (color_type == PNG_COLOR_TYPE_RGBA) ? 4 :
       (color_type == PNG_COLOR_TYPE_RGB) ? 3 : 1;

   if (fp == NULL)
      return 0;

   png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   info_ptr = png_create_info_struct(png_ptr);
   if (setjmp(png_jmpbuf(png_ptr)))
      return 0;

   png_init_io(png_ptr, fp);
   png_set_IHDR(png_ptr, info_ptr, w, h, bit_depth, color_type,
       interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
       PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
   png_write_info(png_ptr, info_ptr);

   rowbytes = RT_ROWBYTES((unsigned int)channels * bit_depth, w);
   row = (png_bytep)malloc(rowbytes);

   if (interlaced)
      png_set_interlace_handling(png_ptr);

   /* Multiple passes for interlaced images: rewrite the same rows. */
   {
      int pass, npasses = interlaced ? 7 : 1;
      png_bytep *rows = (png_bytep *)malloc(h * sizeof(png_bytep));
      png_bytep data = (png_bytep)malloc(rowbytes * h);
      png_uint_32 i;

      for (i = 0; i < h; i++)
      {
         size_t k;
         rows[i] = data + (size_t)i * rowbytes;
         for (k = 0; k < rowbytes; k++)
            /* Semi-random content so all filters get exercised. */
            rows[i][k] = (png_byte)((next_rand() + k / 7 + i / 3) & 0xff);
      }

      for (pass = 0; pass < npasses; pass++)
         for (y = 0; y < h; y++)
            png_write_row(png_ptr, rows[y]);

      free(rows);
      free(data);
   }

   free(row);
   png_write_end(png_ptr, info_ptr);
   png_destroy_write_struct(&png_ptr, &info_ptr);
   fclose(fp);
   return 1;
}

/* ------------------------------------------------------------------ */
/* Decode helpers                                                      */
/* ------------------------------------------------------------------ */
typedef struct
{
   png_bytepp rows;        /* decoded rows */
   png_uint_32 width;      /* row width in pixels */
   png_uint_32 height;     /* number of rows */
   unsigned int pixel_depth;
   size_t rowbytes;
   png_bytep data;         /* backing store */
} decoded_image;

static void
free_decoded(decoded_image *img)
{
   if (img->rows != NULL)
      app_free(img->rows);
   if (img->data != NULL)
      app_free(img->data);
   memset(img, 0, sizeof(*img));
}

/* Decode <file>; when use_region is set, decode only the given rectangle
 * (after clipping by libpng).  Fills *img.  Returns 1 on success.
 */
static int
decode(const char *file, int use_region, png_uint_32 rx, png_uint_32 ry,
    png_uint_32 rw, png_uint_32 rh, decoded_image *img)
{
   FILE *fp = fopen(file, "rb");
   png_structp png_ptr;
   png_infop info_ptr;
   png_uint_32 y;

   memset(img, 0, sizeof(*img));
   if (fp == NULL)
      return 0;

   png_ptr = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL,
       NULL, trk_malloc, trk_free);
   info_ptr = png_create_info_struct(png_ptr);

   if (setjmp(png_jmpbuf(png_ptr)))
   {
      png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
      fclose(fp);
      free_decoded(img);
      return 0;
   }

   png_init_io(png_ptr, fp);
   png_read_info(png_ptr, info_ptr);

   /* Optional transforms for exercising the crop-after-transform path. */
   if (getenv("RT_STRIP16") != NULL)
      png_set_strip_16(png_ptr);
   if (getenv("RT_EXPAND") != NULL)
      png_set_expand(png_ptr);
   if (getenv("RT_PACKING") != NULL)
      png_set_packing(png_ptr);

   {
      png_uint_32 w = png_get_image_width(png_ptr, info_ptr);
      png_uint_32 h = png_get_image_height(png_ptr, info_ptr);
      int bit_depth, channels;

      png_read_update_info(png_ptr, info_ptr);
      bit_depth = png_get_bit_depth(png_ptr, info_ptr);
      channels = png_get_channels(png_ptr, info_ptr);

      img->pixel_depth = (unsigned int)channels * (unsigned int)bit_depth;

      if (use_region)
      {
         if (png_set_read_region(png_ptr, rx, ry, rw, rh) == 0)
         {
            fprintf(stderr, "region is empty\n");
            png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
            fclose(fp);
            return 0;
         }
         png_get_read_region(png_ptr, &rx, &ry, &rw, &rh);
         img->width = rw;
         img->height = rh;
      }
      else
      {
         img->width = w;
         img->height = h;
      }
   }

   img->rowbytes = RT_ROWBYTES(img->pixel_depth, img->width);
   img->rows = (png_bytepp)app_alloc(img->height * sizeof(png_bytep));
   img->data = (png_bytep)app_alloc(img->rowbytes * (size_t)img->height);
   memset(img->data, 0, img->rowbytes * (size_t)img->height);

   for (y = 0; y < img->height; y++)
      img->rows[y] = img->data + (size_t)y * img->rowbytes;

   png_read_image(png_ptr, img->rows);
   png_read_end(png_ptr, NULL);

   png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
   fclose(fp);
   return 1;
}

/* Extract bit <i> from a packed row. */
static unsigned int
get_bit(png_const_bytep row, size_t i)
{
   return (row[i >> 3] >> (7 - (i & 7))) & 1u;
}

/* ------------------------------------------------------------------ */
/* Modes                                                               */
/* ------------------------------------------------------------------ */
static int
mode_gen(int argc, char **argv)
{
   png_uint_32 w, h;
   char path[1024];
   const char *dir;

   if (argc < 5)
      return fprintf(stderr, "gen <w> <h> <dir>\n"), 2;

   w = (png_uint_32)strtoul(argv[2], NULL, 10);
   h = (png_uint_32)strtoul(argv[3], NULL, 10);
   dir = argv[4];

#define GEN(name, ct, bd, il) \
   do { \
      snprintf(path, sizeof path, "%s/%s", dir, name); \
      if (!write_png(path, w, h, ct, bd, il)) \
         return fprintf(stderr, "failed to write %s\n", path), 1; \
      printf("wrote %s\n", path); \
   } while (0)

   GEN("rgba8.png", PNG_COLOR_TYPE_RGBA, 8, 0);
   GEN("rgba8_i.png", PNG_COLOR_TYPE_RGBA, 8, 1);
   GEN("rgb16.png", PNG_COLOR_TYPE_RGB, 16, 0);
   GEN("rgb16_i.png", PNG_COLOR_TYPE_RGB, 16, 1);
   GEN("gray1.png", PNG_COLOR_TYPE_GRAY, 1, 0);
   GEN("gray1_i.png", PNG_COLOR_TYPE_GRAY, 1, 1);
#undef GEN
   return 0;
}

static int
mode_check(int argc, char **argv)
{
   decoded_image full, region;
   png_uint_32 rx, ry, rw, rh, crx, cry, crw, crh;
   png_uint_32 j, i;
   unsigned int pd;
   size_t mismatches = 0;

   if (argc < 7)
      return fprintf(stderr, "check <file> <x> <y> <w> <h>\n"), 2;

   rx = (png_uint_32)strtoul(argv[3], NULL, 10);
   ry = (png_uint_32)strtoul(argv[4], NULL, 10);
   rw = (png_uint_32)strtoul(argv[5], NULL, 10);
   rh = (png_uint_32)strtoul(argv[6], NULL, 10);

   if (!decode(argv[2], 0, 0, 0, 0, 0, &full))
      return fprintf(stderr, "full decode failed\n"), 1;
   if (!decode(argv[2], 1, rx, ry, rw, rh, &region))
      return fprintf(stderr, "region decode failed\n"), 1;

   /* The clipped region actually decoded: */
   crx = rx; cry = ry; crw = region.width; crh = region.height;
   if (crx >= full.width) crw = 0;
   else if (crx + crw > full.width) crw = full.width - crx;
   if (cry >= full.height) crh = 0;
   else if (cry + crh > full.height) crh = full.height - cry;

   pd = full.pixel_depth;
   if (pd != region.pixel_depth)
      return fprintf(stderr, "pixel depth mismatch\n"), 1;

   for (j = 0; j < crh; j++)
   {
      png_const_bytep frow = full.rows[cry + j];
      png_const_bytep rrow = region.rows[j];
      size_t fbit = (size_t)crx * pd;

      for (i = 0; i < (size_t)crw * pd; i++)
      {
         if (get_bit(frow, fbit + i) != get_bit(rrow, i))
         {
            if (mismatches < 5)
               fprintf(stderr,
                   "mismatch at region pixel (%lu,%lu) bit %lu\n",
                   (unsigned long)(i / pd), (unsigned long)j,
                   (unsigned long)(i % pd));
            mismatches++;
         }
      }
   }

   printf("%s: region (%u,%u %ux%u) %s (%zu mismatches)\n", argv[2],
       crx, cry, crw, crh, mismatches == 0 ? "IDENTICAL" : "DIFFERS",
       mismatches);

   free_decoded(&full);
   free_decoded(&region);
   return mismatches == 0 ? 0 : 1;
}

static int
mode_mem(int argc, char **argv, int use_region)
{
   decoded_image img;
   png_uint_32 rx = 0, ry = 0, rw = 0, rh = 0;
   int ok;

   if (use_region)
   {
      if (argc < 7)
         return fprintf(stderr, "memregion <file> <x> <y> <w> <h>\n"), 2;
      rx = (png_uint_32)strtoul(argv[3], NULL, 10);
      ry = (png_uint_32)strtoul(argv[4], NULL, 10);
      rw = (png_uint_32)strtoul(argv[5], NULL, 10);
      rh = (png_uint_32)strtoul(argv[6], NULL, 10);
   }
   else if (argc < 3)
      return fprintf(stderr, "memfull <file>\n"), 2;

   trk_cur = trk_peak = 0;
   ok = decode(argv[2], use_region, rx, ry, rw, rh, &img);
   if (!ok)
      return fprintf(stderr, "decode failed\n"), 1;

   printf("%s %s: decoded %ux%u (pixel_depth %u)\n",
       use_region ? "region" : "full  ", argv[2], img.width, img.height,
       img.pixel_depth);
   printf("  app image buffer : %10zu bytes\n",
       img.rowbytes * (size_t)img.height + img.height * sizeof(png_bytep));
   printf("  peak tracked     : %10zu bytes (libpng + app buffer)\n", trk_peak);

   free_decoded(&img);
   printf("  peak RSS         : %10ld KB\n", max_rss_kb());
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr,
          "usage: region_test <gen|check|memfull|memregion> ...\n");
      return 2;
   }

   if (strcmp(argv[1], "gen") == 0)
      return mode_gen(argc, argv);
   if (strcmp(argv[1], "check") == 0)
      return mode_check(argc, argv);
   if (strcmp(argv[1], "memfull") == 0)
      return mode_mem(argc, argv, 0);
   if (strcmp(argv[1], "memregion") == 0)
      return mode_mem(argc, argv, 1);

   fprintf(stderr, "unknown mode %s\n", argv[1]);
   return 2;
}
