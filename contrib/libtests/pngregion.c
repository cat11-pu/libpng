/* pngregion.c - consistency tests for the streaming region reader
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"

#define RT_ROWBYTES(pixel_bits, width) \
   ((pixel_bits) >= 8 ? \
    (size_t)(width) * ((size_t)(pixel_bits) >> 3) : \
    (((size_t)(width) * (size_t)(pixel_bits) + 7) >> 3))

typedef struct
{
   png_bytep data;
   size_t size;
   size_t capacity;
} memory_buffer;

typedef struct
{
   png_const_bytep data;
   size_t size;
   size_t offset;
} memory_reader;

typedef struct
{
   png_bytepp rows;
   png_bytep data;
   png_uint_32 width;
   png_uint_32 height;
   unsigned int pixel_depth;
   size_t rowbytes;
} decoded_image;

typedef struct
{
   png_uint_32 x;
   png_uint_32 y;
   png_uint_32 width;
   png_uint_32 height;
} region;

typedef struct
{
   const char *name;
   png_uint_32 width;
   png_uint_32 height;
   int color_type;
   int bit_depth;
   int interlace;
   int use_trns;
   int filter;
} image_spec;

typedef struct
{
   const decoded_image *full;
   png_uint_32 x;
   png_uint_32 y;
   png_uint_32 width;
   png_uint_32 height;
   png_uint_32 region_width;
   png_uint_32 region_height;
   size_t rowbytes;
   png_uint_32 next_y;
   size_t rows_seen;
   size_t pixels_seen;
   size_t mismatches;
   size_t first_bad_x;
   size_t first_bad_y;
   int info_seen;
   int profile;
} stream_check;

static unsigned int rng_state = 0x12345678U;
static size_t tracked_live;
static size_t tracked_peak;

static png_voidp PNGAPI
tracked_malloc(png_structp png_ptr, png_alloc_size_t size)
{
   size_t *block;

   (void)png_ptr;

   if (size > (size_t)-1 - sizeof *block)
      return NULL;

   block = (size_t*)malloc((size_t)size + sizeof *block);
   if (block == NULL)
      return NULL;

   *block = (size_t)size;
   tracked_live += (size_t)size;
   if (tracked_live > tracked_peak)
      tracked_peak = tracked_live;

   return block + 1;
}

static void PNGAPI
tracked_free(png_structp png_ptr, png_voidp ptr)
{
   size_t *block;

   (void)png_ptr;

   if (ptr == NULL)
      return;

   block = (size_t*)ptr - 1;
   tracked_live -= *block;
   free(block);
}

static unsigned int
next_random(void)
{
   rng_state = rng_state * 1664525U + 1013904223U;
   return rng_state;
}

static void PNGCBAPI
memory_write(png_structp png_ptr, png_bytep data, size_t size)
{
   memory_buffer *buffer = (memory_buffer*)png_get_io_ptr(png_ptr);
   size_t required;

   if (size > (size_t)-1 - buffer->size)
      png_error(png_ptr, "test buffer overflow");

   required = buffer->size + size;

   if (required > buffer->capacity)
   {
      size_t capacity = buffer->capacity != 0 ? buffer->capacity : 1024;

      while (capacity < required)
      {
         if (capacity > (size_t)-1 / 2)
         {
            capacity = required;
            break;
         }

         capacity *= 2;
      }

      {
         png_bytep new_data = (png_bytep)realloc(buffer->data, capacity);

         if (new_data == NULL)
            png_error(png_ptr, "out of memory");

         buffer->data = new_data;
         buffer->capacity = capacity;
      }
   }

   memcpy(buffer->data + buffer->size, data, size);
   buffer->size = required;
}

static void PNGCBAPI
memory_read(png_structp png_ptr, png_bytep data, size_t size)
{
   memory_reader *reader = (memory_reader*)png_get_io_ptr(png_ptr);

   if (size > reader->size - reader->offset)
      png_error(png_ptr, "unexpected end of PNG data");

   memcpy(data, reader->data + reader->offset, size);
   reader->offset += size;
}

static void
set_bits(png_bytep row, size_t bit, unsigned int count, unsigned int value)
{
   unsigned int i;

   for (i = 0; i < count; ++i)
   {
      size_t pos = bit + i;
      unsigned int mask = 0x80U >> (pos & 7);
      unsigned int bit_value = (value >> (count - 1 - i)) & 1U;

      if (bit_value != 0)
         row[pos >> 3] |= (png_byte)mask;
      else
         row[pos >> 3] &= (png_byte)~mask;
   }
}

static unsigned int
random_sample(int bit_depth)
{
   unsigned int value = next_random();

   if (bit_depth == 16)
      return value & 0xffffU;

   if (bit_depth == 8)
      return value & 0xffU;

   return value & ((1U << bit_depth) - 1U);
}

static void
fill_row(png_bytep row, png_uint_32 y, const image_spec *spec)
{
   png_uint_32 x;
   int channels = spec->color_type == PNG_COLOR_TYPE_PALETTE ? 1 :
       (spec->color_type & PNG_COLOR_MASK_COLOR) != 0 ?
       ((spec->color_type & PNG_COLOR_MASK_ALPHA) != 0 ? 4 : 3) :
       ((spec->color_type & PNG_COLOR_MASK_ALPHA) != 0 ? 2 : 1);
   unsigned int pixel_depth = (unsigned int)spec->bit_depth * channels;
   unsigned int palette_size =
       spec->bit_depth < 8 ? 1U << spec->bit_depth : 16U;

   for (x = 0; x < spec->width; ++x)
   {
      size_t bit = (size_t)x * pixel_depth;

      if (spec->color_type == PNG_COLOR_TYPE_PALETTE)
      {
         unsigned int index =
             (x * 7U + y * 13U + next_random()) % palette_size;

         set_bits(row, bit, (unsigned int)spec->bit_depth, index);
      }

      else
      {
         int channel;

         for (channel = 0; channel < channels; ++channel)
            set_bits(row, bit + (size_t)channel * spec->bit_depth,
                (unsigned int)spec->bit_depth,
                random_sample(spec->bit_depth));
      }
   }
}

static int
write_png(memory_buffer *output, const image_spec *spec)
{
   png_structp png_ptr;
   png_infop info_ptr;
   png_bytepp rows = NULL;
   png_bytep data = NULL;
   size_t rowbytes;
   png_uint_32 y;
   int channels = spec->color_type == PNG_COLOR_TYPE_PALETTE ? 1 :
       (spec->color_type & PNG_COLOR_MASK_COLOR) != 0 ?
       ((spec->color_type & PNG_COLOR_MASK_ALPHA) != 0 ? 4 : 3) :
       ((spec->color_type & PNG_COLOR_MASK_ALPHA) != 0 ? 2 : 1);
   unsigned int pixel_depth = (unsigned int)spec->bit_depth * channels;
   int success = 0;

   png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   if (png_ptr == NULL)
      return 0;

   info_ptr = png_create_info_struct(png_ptr);
   if (info_ptr == NULL)
   {
      png_destroy_write_struct(&png_ptr, NULL);
      return 0;
   }

   if (setjmp(png_jmpbuf(png_ptr)))
      goto done;

   png_set_write_fn(png_ptr, output, memory_write, NULL);
   png_set_IHDR(png_ptr, info_ptr, spec->width, spec->height,
       spec->bit_depth, spec->color_type,
       spec->interlace != 0 ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
       PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

   if (spec->color_type == PNG_COLOR_TYPE_PALETTE)
   {
      png_color palette[16];
      png_byte alpha[16];
      int i;
      int count = spec->bit_depth < 8 ? 1 << spec->bit_depth : 16;

      for (i = 0; i < count; ++i)
      {
         palette[i].red = (png_byte)(i * 17);
         palette[i].green = (png_byte)(255 - i * 11);
         palette[i].blue = (png_byte)(i * 7 + 33);
         alpha[i] = (png_byte)(i * 13 + 7);
      }

      png_set_PLTE(png_ptr, info_ptr, palette, count);

      if (spec->use_trns != 0)
         png_set_tRNS(png_ptr, info_ptr, alpha, count, NULL);
   }

   else if (spec->use_trns != 0)
   {
      png_color_16 trans;

      memset(&trans, 0, sizeof trans);
      trans.gray = spec->bit_depth == 16 ? 0x0101 : 1;
      trans.red = trans.green = trans.blue =
          spec->bit_depth == 16 ? 0x0101 : 1;
      png_set_tRNS(png_ptr, info_ptr, NULL, 0, &trans);
   }

   png_write_info(png_ptr, info_ptr);
   png_set_filter(png_ptr, PNG_FILTER_TYPE_BASE,
       spec->filter != 0 ? spec->filter : PNG_ALL_FILTERS);

   rowbytes = RT_ROWBYTES(pixel_depth, spec->width);
   if (rowbytes == 0 || spec->height > (size_t)-1 / rowbytes)
      goto done;

   data = (png_bytep)malloc(rowbytes * spec->height);
   rows = (png_bytepp)malloc(spec->height * sizeof (png_bytep));

   if (data == NULL || rows == NULL)
      goto done;

   for (y = 0; y < spec->height; ++y)
   {
      rows[y] = data + (size_t)y * rowbytes;
      memset(rows[y], 0, rowbytes);
      fill_row(rows[y], y, spec);
   }

   {
      int passes = png_set_interlace_handling(png_ptr);
      int pass;

      for (pass = 0; pass < passes; ++pass)
         for (y = 0; y < spec->height; ++y)
            png_write_row(png_ptr, rows[y]);
   }

   png_write_end(png_ptr, info_ptr);
   success = 1;

done:
   free(rows);
   free(data);
   png_destroy_write_struct(&png_ptr, &info_ptr);
   return success;
}

static void
apply_profile(png_structp png_ptr, int profile)
{
   if (profile == 0)
      return;

#ifdef PNG_READ_EXPAND_SUPPORTED
   if (profile == 1 || profile == 3)
      png_set_expand(png_ptr);
#endif

#ifdef PNG_READ_SCALE_16_TO_8_SUPPORTED
   if (profile == 1 || profile == 3)
      png_set_scale_16(png_ptr);
#endif

#ifdef PNG_READ_PACK_SUPPORTED
   if (profile == 2)
      png_set_packing(png_ptr);
#endif

#ifdef PNG_READ_GRAY_TO_RGB_SUPPORTED
   if (profile == 3)
      png_set_gray_to_rgb(png_ptr);
#endif

#ifdef PNG_READ_FILLER_SUPPORTED
   if (profile == 3)
      png_set_add_alpha(png_ptr, 0xff, PNG_FILLER_AFTER);
#endif
}

static void
free_decoded(decoded_image *image)
{
   free(image->rows);
   free(image->data);
   memset(image, 0, sizeof *image);
}

static int
decode_full(const memory_buffer *input, int profile, decoded_image *image)
{
   memory_reader reader;
   png_structp png_ptr;
   png_infop info_ptr;
   png_uint_32 y;
   int success = 0;

   memset(image, 0, sizeof *image);
   reader.data = input->data;
   reader.size = input->size;
   reader.offset = 0;

   png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   if (png_ptr == NULL)
      return 0;

   info_ptr = png_create_info_struct(png_ptr);
   if (info_ptr == NULL)
   {
      png_destroy_read_struct(&png_ptr, NULL, NULL);
      return 0;
   }

   if (setjmp(png_jmpbuf(png_ptr)))
      goto done;

   png_set_read_fn(png_ptr, &reader, memory_read);
   png_read_info(png_ptr, info_ptr);
   apply_profile(png_ptr, profile);
#ifdef PNG_READ_INTERLACING_SUPPORTED
   (void)png_set_interlace_handling(png_ptr);
#endif
   png_read_update_info(png_ptr, info_ptr);

   image->width = png_get_image_width(png_ptr, info_ptr);
   image->height = png_get_image_height(png_ptr, info_ptr);
   image->pixel_depth = (unsigned int)png_get_bit_depth(png_ptr, info_ptr) *
       png_get_channels(png_ptr, info_ptr);
   image->rowbytes = png_get_rowbytes(png_ptr, info_ptr);

   if (image->rowbytes == 0 ||
       image->height > (size_t)-1 / image->rowbytes)
      goto done;

   image->data = (png_bytep)malloc(image->rowbytes * image->height);
   image->rows = (png_bytepp)malloc(image->height * sizeof (png_bytep));

   if (image->data == NULL || image->rows == NULL)
      goto done;

   for (y = 0; y < image->height; ++y)
      image->rows[y] = image->data + (size_t)y * image->rowbytes;

   png_read_image(png_ptr, image->rows);
   png_read_end(png_ptr, NULL);
   success = 1;

done:
   if (success == 0)
      free_decoded(image);

   png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
   return success;
}

static unsigned int
get_bit(png_const_bytep row, size_t bit)
{
   return (row[bit >> 3] >> (7 - (bit & 7))) & 1U;
}

static void PNGCBAPI
stream_info(png_structp png_ptr, png_infop info_ptr, png_voidp user_data)
{
   stream_check *check = (stream_check*)user_data;
   png_region_readerp reader = (png_region_readerp)
       png_get_progressive_ptr(png_ptr);
   png_uint_32 x, y, width, height;

   (void)info_ptr;

   ++check->info_seen;

   if (reader == NULL || png_region_reader_get_region(reader, &x, &y,
       &width, &height) == 0 || x != check->x || y != check->y ||
       width != check->region_width || height != check->region_height)
      ++check->mismatches;

   apply_profile(png_ptr, check->profile);
}

static void PNGCBAPI
stream_row(png_structp png_ptr, png_uint_32 y, png_const_bytep row,
    size_t rowbytes, png_voidp user_data)
{
   stream_check *check = (stream_check*)user_data;
   size_t bit_count = (size_t)check->width * check->full->pixel_depth;
   size_t i;

   (void)png_ptr;

   if (y != check->y + check->next_y)
   {
      ++check->mismatches;
   }

   if (rowbytes != check->rowbytes)
   {
      ++check->mismatches;
      return;
   }

   for (i = 0; i < bit_count; ++i)
   {
      size_t full_bit = (size_t)check->x * check->full->pixel_depth + i;

      if (get_bit(row, i) != get_bit(check->full->rows[y], full_bit))
      {
         if (check->mismatches == 0)
         {
            check->first_bad_x =
                (size_t)check->x + i / check->full->pixel_depth;
            check->first_bad_y = y;
         }

         ++check->mismatches;
      }
   }

   ++check->next_y;
   ++check->rows_seen;
   check->pixels_seen += check->width;
}

static void
clip_region(region *result, region requested, png_uint_32 width,
    png_uint_32 height)
{
   *result = requested;

   if (requested.x >= width)
      result->width = 0;
   else if (requested.width > width - requested.x)
      result->width = width - requested.x;

   if (requested.y >= height)
      result->height = 0;
   else if (requested.height > height - requested.y)
      result->height = height - requested.y;
}

static int
stream_compare(const memory_buffer *input, int profile, region requested,
    const decoded_image *full, size_t chunk_size, stream_check *check,
    region *clipped)
{
   png_structp png_ptr;
   png_infop info_ptr;
   png_region_readerp reader;
   memory_reader source;
   int success = 0;
   int done = 0;

   memset(check, 0, sizeof *check);
   check->full = full;
   check->profile = profile;
   clip_region(clipped, requested, full->width, full->height);
   check->x = clipped->x;
   check->y = clipped->y;
   check->width = clipped->width;
   check->height = clipped->width == 0 ? 0 : clipped->height;
   check->region_width = clipped->width;
   check->region_height = clipped->height;
   check->rowbytes = RT_ROWBYTES(full->pixel_depth, clipped->width);

   source.data = input->data;
   source.size = input->size;
   source.offset = 0;

   png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   if (png_ptr == NULL)
      return 0;

   info_ptr = png_create_info_struct(png_ptr);
   if (info_ptr == NULL)
   {
      png_destroy_read_struct(&png_ptr, NULL, NULL);
      return 0;
   }

   if (setjmp(png_jmpbuf(png_ptr)))
      goto done;

   png_set_read_fn(png_ptr, &source, memory_read);
   reader = png_region_reader_create(png_ptr, info_ptr,
       requested.x, requested.y, requested.width, requested.height,
       stream_info, stream_row, check);

   if (reader == NULL)
      goto done;

   while (source.offset < source.size && done == 0)
   {
      size_t amount = source.size - source.offset;

      if (chunk_size != 0 && amount > chunk_size)
         amount = chunk_size;

      done = png_region_reader_feed(reader, source.data + source.offset,
          amount);
      source.offset += amount;
   }

   if (done == 0)
      done = png_region_reader_end(reader);

   success = done != 0 && check->info_seen == 1 &&
       check->rows_seen == check->height && check->mismatches == 0;

   png_region_reader_destroy(reader);

done:
   png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

   if (success == 0 && check->mismatches == 0)
      ++check->mismatches;

   return success;
}

static size_t
make_regions(const image_spec *spec, region *regions)
{
   size_t count = 0;
   png_uint_32 mid_x = spec->width / 3;
   png_uint_32 mid_y = spec->height / 3;
   png_uint_32 mid_w = spec->width / 3;
   png_uint_32 mid_h = spec->height / 3;

   if (mid_w == 0)
      mid_w = 1;

   if (mid_h == 0)
      mid_h = 1;

   regions[count].x = 0;
   regions[count].y = 0;
   regions[count].width = spec->width;
   regions[count].height = spec->height;
   ++count;

   regions[count].x = 0;
   regions[count].y = 0;
   regions[count].width = 1;
   regions[count].height = 1;
   ++count;

   regions[count].x = spec->width - 1;
   regions[count].y = spec->height - 1;
   regions[count].width = 1;
   regions[count].height = 1;
   ++count;

   regions[count].x = 0;
   regions[count].y = spec->height - 1;
   regions[count].width = spec->width;
   regions[count].height = 1;
   ++count;

   regions[count].x = spec->width - 1;
   regions[count].y = 0;
   regions[count].width = 1;
   regions[count].height = spec->height;
   ++count;

   regions[count].x = 1;
   regions[count].y = 0;
   regions[count].width = spec->width - 1;
   regions[count].height = 1;
   ++count;

   regions[count].x = 0;
   regions[count].y = 1;
   regions[count].width = 1;
   regions[count].height = spec->height - 1;
   ++count;

   regions[count].x = mid_x;
   regions[count].y = mid_y;
   regions[count].width = mid_w;
   regions[count].height = mid_h;
   ++count;

   regions[count].x = spec->width - 2;
   regions[count].y = spec->height - 2;
   regions[count].width = 10;
   regions[count].height = 10;
   ++count;

   regions[count].x = spec->width + 3;
   regions[count].y = spec->height / 2;
   regions[count].width = 2;
   regions[count].height = 2;
   ++count;

   regions[count].x = 0;
   regions[count].y = spec->height + 4;
   regions[count].width = spec->width;
   regions[count].height = 3;
   ++count;

   regions[count].x = 1;
   regions[count].y = 1;
   regions[count].width = 0;
   regions[count].height = 0;
   ++count;

   return count;
}

static int
self_test(void)
{
   static const image_spec specs[] =
   {
      { "gray1",       1,  1, PNG_COLOR_TYPE_GRAY,        1, 0, 0,
          PNG_FILTER_NONE },
      { "gray2-i",     2,  2, PNG_COLOR_TYPE_GRAY,        2, 1, 0,
          PNG_FILTER_SUB },
      { "gray4",       7,  5, PNG_COLOR_TYPE_GRAY,        4, 0, 0,
          PNG_FILTER_UP },
      { "pal4-i",      9,  7, PNG_COLOR_TYPE_PALETTE,     4, 1, 1,
          PNG_FILTER_AVG },
      { "gray8",      13, 11, PNG_COLOR_TYPE_GRAY,        8, 0, 0,
          PNG_FILTER_PAETH },
      { "ga8-i",      17, 13, PNG_COLOR_TYPE_GRAY_ALPHA,  8, 1, 0, 0 },
      { "rgb8",       19, 17, PNG_COLOR_TYPE_RGB,         8, 0, 1,
          PNG_FILTER_NONE },
      { "rgba8-i",    23, 19, PNG_COLOR_TYPE_RGB_ALPHA,   8, 1, 0,
          PNG_FILTER_SUB },
      { "rgb16",      29, 21, PNG_COLOR_TYPE_RGB,        16, 0, 0,
          PNG_FILTER_UP },
      { "rgba16-i",   31, 23, PNG_COLOR_TYPE_RGB_ALPHA,  16, 1, 0,
          PNG_FILTER_AVG },
      { "pal8",       37, 29, PNG_COLOR_TYPE_PALETTE,     8, 0, 1,
          PNG_FILTER_PAETH },
      { "gray16-i",   37, 29, PNG_COLOR_TYPE_GRAY,       16, 1, 0, 0 }
   };
   static const size_t chunks[] =
   {
      0, 1, 2, 3, 4, 7, 8, 13, 31, 251
   };
   size_t total_comparisons = 0;
   size_t total_rows = 0;
   size_t total_pixels = 0;
   size_t total_bits = 0;
   size_t total_mismatches = 0;
   size_t images = 0;
   size_t s;

   for (s = 0; s < sizeof specs / sizeof specs[0]; ++s)
   {
      memory_buffer encoded;
      region regions[16];
      int profiles[4];
      size_t profile_count = 0;
      size_t region_count;
      size_t image_comparisons = 0;
      size_t image_rows = 0;
      size_t image_pixels = 0;
      size_t image_bits = 0;
      size_t image_mismatches = 0;
      size_t profile_index;
      size_t p;

      setvbuf(stdout, NULL, _IONBF, 0);
      memset(&encoded, 0, sizeof encoded);
      rng_state = 0x31415926U + (unsigned int)s * 7919U;

      if (!write_png(&encoded, &specs[s]))
      {
         fprintf(stderr, "pngregion: failed to generate %s\n", specs[s].name);
         free(encoded.data);
         return 1;
      }

      region_count = make_regions(&specs[s], regions);
      ++images;

      profiles[profile_count++] = 0;
      profiles[profile_count++] = 1;
      if (specs[s].bit_depth < 8)
         profiles[profile_count++] = 2;
      profiles[profile_count++] = 3;

      for (profile_index = 0; profile_index < profile_count; ++profile_index)
      {
         decoded_image full;

         if (!decode_full(&encoded, profiles[profile_index], &full))
         {
            fprintf(stderr, "pngregion: failed to decode %s profile=%d\n",
                specs[s].name, profiles[profile_index]);
            free(encoded.data);
            return 1;
         }

         for (p = 0; p < region_count; ++p)
         {
            size_t chunk_index;

            for (chunk_index = 0;
                chunk_index < sizeof chunks / sizeof chunks[0];
                ++chunk_index)
            {
               stream_check check;
               region clipped;
               int ok;

               ok = stream_compare(&encoded, profiles[profile_index],
                   regions[p], &full, chunks[chunk_index], &check, &clipped);

               if (!ok)
               {
                  fprintf(stderr,
                      "pngregion: FAIL %s profile=%d region=(%u,%u %ux%u) "
                      "clipped=(%u,%u %ux%u) chunk=%lu rows=%lu/%lu "
                      "mismatches=%lu first=(%lu,%lu)\n",
                      specs[s].name, profiles[profile_index],
                      regions[p].x, regions[p].y, regions[p].width,
                      regions[p].height, clipped.x, clipped.y, clipped.width,
                      clipped.height, (unsigned long)chunks[chunk_index],
                      (unsigned long)check.rows_seen,
                      (unsigned long)check.height,
                      (unsigned long)check.mismatches,
                      (unsigned long)check.first_bad_x,
                      (unsigned long)check.first_bad_y);
               }

               ++image_comparisons;
               image_rows += check.rows_seen;
               image_pixels += check.pixels_seen;
               image_bits += check.pixels_seen * full.pixel_depth;
               image_mismatches += check.mismatches;
               total_mismatches += check.mismatches;
            }
         }

         free_decoded(&full);
      }

      total_comparisons += image_comparisons;
      total_rows += image_rows;
      total_pixels += image_pixels;
      total_bits += image_bits;

      printf("%-10s %2ux%-2u %-8s rows=%-6lu pixels=%-7lu "
          "chunk-runs=%-4lu mismatches=%lu\n",
          specs[s].name, specs[s].width, specs[s].height,
          specs[s].interlace != 0 ? "interlace" : "plain",
          (unsigned long)image_rows, (unsigned long)image_pixels,
          (unsigned long)image_comparisons,
          (unsigned long)image_mismatches);

      free(encoded.data);
   }

   printf("SUMMARY images=%lu comparisons=%lu rows=%lu pixels=%lu "
       "pixel_bits=%lu mismatches=%lu\n",
       (unsigned long)images, (unsigned long)total_comparisons,
       (unsigned long)total_rows, (unsigned long)total_pixels,
       (unsigned long)total_bits, (unsigned long)total_mismatches);

   return total_mismatches == 0 ? 0 : 1;
}

typedef struct
{
   size_t rows;
} measure_context;

static void PNGCBAPI
measure_row(png_structp png_ptr, png_uint_32 y, png_const_bytep row,
    size_t rowbytes, png_voidp user_data)
{
   measure_context *measure = (measure_context*)user_data;

   (void)png_ptr;
   (void)y;
   (void)row;
   (void)rowbytes;

   ++measure->rows;
}

static int
measure_stream(const memory_buffer *input, region requested, size_t chunk_size,
    size_t *peak, size_t *rows)
{
   png_structp png_ptr;
   png_infop info_ptr;
   png_region_readerp reader;
   memory_reader source;
   measure_context measure;
   int done = 0;
   int success = 0;

   tracked_live = 0;
   tracked_peak = 0;
   memset(&measure, 0, sizeof measure);

   source.data = input->data;
   source.size = input->size;
   source.offset = 0;

   png_ptr = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL,
       NULL, tracked_malloc, tracked_free);
   if (png_ptr == NULL)
      return 0;

   info_ptr = png_create_info_struct(png_ptr);
   if (info_ptr == NULL)
   {
      png_destroy_read_struct(&png_ptr, NULL, NULL);
      return 0;
   }

   if (setjmp(png_jmpbuf(png_ptr)))
      goto done;

   png_set_read_fn(png_ptr, &source, memory_read);
   reader = png_region_reader_create(png_ptr, info_ptr, requested.x,
       requested.y, requested.width, requested.height, NULL, measure_row,
       &measure);

   if (reader == NULL)
      goto done;

   while (source.offset < source.size && done == 0)
   {
      size_t amount = source.size - source.offset;

      if (chunk_size != 0 && amount > chunk_size)
         amount = chunk_size;

      done = png_region_reader_feed(reader, source.data + source.offset,
          amount);
      source.offset += amount;
   }

   if (done == 0)
      done = png_region_reader_end(reader);

   *peak = tracked_peak;
   *rows = measure.rows;
   success = done != 0;

   png_region_reader_destroy(reader);

done:
   png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
   return success;
}

static int
memory_test(void)
{
   static const png_uint_32 heights[] = { 100, 20000 };
   image_spec spec;
   size_t i;

   memset(&spec, 0, sizeof spec);
   spec.name = "memory-gray8";
   spec.width = 256;
   spec.color_type = PNG_COLOR_TYPE_GRAY;
   spec.bit_depth = 8;

   for (i = 0; i < sizeof heights / sizeof heights[0]; ++i)
   {
      memory_buffer encoded;
      region requested;
      size_t peak;
      size_t rows;
      int ok;

      memset(&encoded, 0, sizeof encoded);
      spec.height = heights[i];
      rng_state = 0xabcdef01U;

      if (!write_png(&encoded, &spec))
      {
         fprintf(stderr, "pngregion: failed to generate memory image\n");
         free(encoded.data);
         return 1;
      }

      requested.x = 32;
      requested.y = 16;
      requested.width = 32;
      requested.height = 8;
      ok = measure_stream(&encoded, requested, 4096, &peak, &rows);

      printf("MEM height=%-6u region=%ux%u peak=%lu rows=%lu %s\n",
          spec.height, requested.width, requested.height,
          (unsigned long)peak, (unsigned long)rows, ok ? "ok" : "FAIL");

      free(encoded.data);

      if (!ok)
         return 1;
   }

   return 0;
}

int
main(int argc, char **argv)
{
   if (argc > 1 && strcmp(argv[1], "--self-test") != 0 &&
       strcmp(argv[1], "--memory") != 0)
   {
      fprintf(stderr, "usage: %s [--self-test|--memory]\n", argv[0]);
      return 2;
   }

   setvbuf(stdout, NULL, _IONBF, 0);

   if (argc > 1 && strcmp(argv[1], "--memory") == 0)
      return memory_test();

   return self_test();
}
