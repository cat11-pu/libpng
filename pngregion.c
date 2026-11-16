/* pngregion.c - narrow streaming region reader
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 */

#include "pngpriv.h"

#ifdef PNG_PROGRESSIVE_READ_SUPPORTED

struct png_region_reader
{
   png_structp png_ptr;
   png_infop info_ptr;
   png_region_info_ptr info_fn;
   png_region_row_ptr row_fn;
   png_voidp user_data;

   png_uint_32 requested_x;
   png_uint_32 requested_y;
   png_uint_32 requested_width;
   png_uint_32 requested_height;

   png_uint_32 image_width;
   png_uint_32 image_height;
   png_uint_32 x;
   png_uint_32 y;
   png_uint_32 width;
   png_uint_32 height;
   png_uint_32 pixel_depth;
   size_t rowbytes;

   png_bytep row;
   png_bytep rows;

   png_byte interlaced;
   png_byte seen_info;
   png_byte emitted;
   png_byte done;
};

/* Copy a bit span without assuming byte alignment.  The destination is
 * cleared by the caller, so unused trailing bits remain zero.
 */
static void
png_region_copy_bits(png_bytep dp, size_t dbit, png_const_bytep sp,
    size_t sbit, size_t nbits)
{
   if ((dbit & 7) == 0 && (sbit & 7) == 0 && (nbits & 7) == 0)
      memcpy(dp + (dbit >> 3), sp + (sbit >> 3), nbits >> 3);

   else
   {
      size_t i;

      for (i = 0; i < nbits; ++i)
      {
         size_t s = sbit + i;
         size_t d = dbit + i;

         if ((sp[s >> 3] & (0x80U >> (s & 7))) != 0)
            dp[d >> 3] |= (png_byte)(0x80U >> (d & 7));
         else
            dp[d >> 3] &= (png_byte)~(0x80U >> (d & 7));
      }
   }
}

static void
png_region_emit(png_region_readerp reader)
{
   if (reader->emitted == 0)
   {
      reader->emitted = 1;

      if (reader->row_fn != NULL)
      {
         png_uint_32 i;

         for (i = 0; i < reader->height; ++i)
            (*(reader->row_fn))(reader->png_ptr, reader->y + i,
                reader->rows + (size_t)i * reader->rowbytes,
                reader->rowbytes, reader->user_data);
      }
   }
}

static void PNGCBAPI
png_region_info_callback(png_structp png_ptr, png_infop info_ptr)
{
   png_region_readerp reader = png_voidcast(png_region_readerp,
       png_get_progressive_ptr(png_ptr));
   png_byte bit_depth;
   png_byte channels;
   int interlace_type;

   if (reader == NULL || reader->png_ptr != png_ptr)
      png_error(png_ptr, "png_region_reader: invalid reader state");

   if (reader->seen_info != 0)
      return;

   reader->image_width = png_get_image_width(png_ptr, info_ptr);
   reader->image_height = png_get_image_height(png_ptr, info_ptr);
   interlace_type = png_get_interlace_type(png_ptr, info_ptr);
   reader->interlaced =
       (png_byte)(interlace_type != PNG_INTERLACE_NONE);
   reader->x = reader->requested_x;
   reader->y = reader->requested_y;

   if (reader->requested_x >= reader->image_width)
      reader->width = 0;
   else if (reader->requested_width > reader->image_width -
       reader->requested_x)
      reader->width = reader->image_width - reader->requested_x;
   else
      reader->width = reader->requested_width;

   if (reader->requested_y >= reader->image_height)
      reader->height = 0;
   else if (reader->requested_height > reader->image_height -
       reader->requested_y)
      reader->height = reader->image_height - reader->requested_y;
   else
      reader->height = reader->requested_height;

   reader->seen_info = 1;

   if (reader->info_fn != NULL)
      (*(reader->info_fn))(png_ptr, info_ptr, reader->user_data);

   /* The region reader assembles Adam7 pass rows itself.  Letting libpng do
    * the same transformation would require one whole-image row per output
    * row and would change the callback contract.
    */
   if ((png_ptr->transformations & PNG_INTERLACE) != 0)
      png_error(png_ptr,
          "png_region_reader: png_set_interlace_handling is not allowed");

   if ((png_ptr->flags & PNG_FLAG_ROW_INIT) == 0)
      png_read_update_info(png_ptr, info_ptr);

   bit_depth = png_get_bit_depth(png_ptr, info_ptr);
   channels = png_get_channels(png_ptr, info_ptr);
   reader->pixel_depth = (png_uint_32)bit_depth * channels;

   if (reader->width == 0 || reader->height == 0)
   {
      /* Nothing to deliver: stop after the header. */
      reader->done = 1;
      return;
   }

   if (reader->pixel_depth == 0)
      png_error(png_ptr, "png_region_reader: invalid pixel depth");

   reader->rowbytes = PNG_ROWBYTES(reader->pixel_depth, reader->width);

   if (reader->rowbytes == 0 || reader->rowbytes > PNG_SIZE_MAX ||
       (png_alloc_size_t)reader->height > PNG_SIZE_MAX / reader->rowbytes)
      png_error(png_ptr, "png_region_reader: region is too large");

   if (reader->interlaced != 0)
   {
      png_alloc_size_t size =
          (png_alloc_size_t)reader->height * reader->rowbytes;

      reader->rows = png_voidcast(png_bytep, png_malloc(png_ptr, size));
      memset(reader->rows, 0, (size_t)size);
   }

   else
   {
      reader->row = png_voidcast(png_bytep,
          png_malloc(png_ptr, reader->rowbytes));
   }
}

static void PNGCBAPI
png_region_row_callback(png_structp png_ptr, png_bytep new_row,
    png_uint_32 row_number, int pass)
{
   png_region_readerp reader = png_voidcast(png_region_readerp,
       png_get_progressive_ptr(png_ptr));

   if (reader == NULL || reader->done != 0)
      return;

   if (reader->seen_info == 0 || reader->width == 0 || reader->height == 0)
      png_error(png_ptr, "png_region_reader: row before image info");

   if (reader->interlaced == 0)
   {
      if (new_row != NULL && row_number >= reader->y &&
          row_number - reader->y < reader->height)
      {
         memset(reader->row, 0, reader->rowbytes);
         png_region_copy_bits(reader->row, 0, new_row,
             (size_t)reader->x * reader->pixel_depth,
             (size_t)reader->width * reader->pixel_depth);

         if (reader->row_fn != NULL)
            (*(reader->row_fn))(png_ptr, row_number, reader->row,
                reader->rowbytes, reader->user_data);

         if (row_number == reader->y + reader->height - 1)
         {
            reader->emitted = 1;
            reader->done = 1;
         }
      }

      return;
   }

   else
   {
      png_uint_32 full_y = PNG_ROW_FROM_PASS_ROW(row_number, pass);
      png_uint_32 last_y = reader->y + reader->height - 1;
      png_uint_32 last_pass_row;
      int last_pass;

      if (new_row != NULL && full_y >= reader->y &&
          full_y - reader->y < reader->height)
      {
         png_uint_32 pass_cols = PNG_PASS_COLS(reader->image_width, pass);
         png_uint_32 sx;
         png_bytep dp = reader->rows +
             (size_t)(full_y - reader->y) * reader->rowbytes;

         for (sx = 0; sx < pass_cols; ++sx)
         {
            png_uint_32 full_x = PNG_COL_FROM_PASS_COL(sx, pass);

            if (full_x >= reader->x &&
                full_x - reader->x < reader->width)
               png_region_copy_bits(dp,
                   (size_t)(full_x - reader->x) * reader->pixel_depth,
                   new_row, (size_t)sx * reader->pixel_depth,
                   reader->pixel_depth);
         }
      }

      /* Even rows are complete after pass 5; odd rows after pass 6. */
      if (reader->height > 1 || (reader->y & 1) != 0)
      {
         last_pass = 6;
         last_pass_row = (last_y & 1) != 0 ? last_y : last_y - 1;
      }
      else
      {
         last_pass = 5;
         last_pass_row = last_y;
      }

      if (pass == last_pass && full_y == last_pass_row)
      {
         png_region_emit(reader);
         reader->done = 1;
      }
   }
}

static void PNGCBAPI
png_region_end_callback(png_structp png_ptr, png_infop info_ptr)
{
   png_region_readerp reader = png_voidcast(png_region_readerp,
       png_get_progressive_ptr(png_ptr));

   PNG_UNUSED(info_ptr)

   if (reader == NULL)
      return;

   if (reader->interlaced != 0 && reader->seen_info != 0 &&
       reader->emitted == 0 && reader->width != 0 && reader->height != 0)
      png_region_emit(reader);

   reader->done = 1;
}

png_region_readerp PNGAPI
png_region_reader_create(png_structrp png_ptr, png_inforp info_ptr,
    png_uint_32 x, png_uint_32 y, png_uint_32 width, png_uint_32 height,
    png_region_info_ptr info_fn, png_region_row_ptr row_fn,
    png_voidp user_data)
{
   png_region_readerp reader;

   png_debug(1, "in png_region_reader_create");

   if (png_ptr == NULL || info_ptr == NULL)
      return NULL;

   if ((png_ptr->mode & ~(png_uint_32)PNG_IS_READ_STRUCT) != 0 ||
       png_ptr->sig_bytes != 0 ||
       (png_ptr->flags & PNG_FLAG_ROW_INIT) != 0)
      png_error(png_ptr,
          "png_region_reader_create: call before feeding PNG data");

   reader = png_voidcast(png_region_readerp,
       png_malloc(png_ptr, sizeof *reader));
   memset(reader, 0, sizeof *reader);
   reader->png_ptr = png_ptr;
   reader->info_ptr = info_ptr;
   reader->info_fn = info_fn;
   reader->row_fn = row_fn;
   reader->user_data = user_data;
   reader->requested_x = x;
   reader->requested_y = y;
   reader->requested_width = width;
   reader->requested_height = height;

   png_set_progressive_read_fn(png_ptr, reader, png_region_info_callback,
       png_region_row_callback, png_region_end_callback);

   return reader;
}

int PNGAPI
png_region_reader_feed(png_region_readerp reader, png_const_bytep data,
    size_t size)
{
   if (reader == NULL || (data == NULL && size != 0))
      return 0;

   if (reader->done != 0)
      return 1;

   if (size != 0)
   {
      size_t offset = 0;

      /* Keep each call into the push reader short.  Once the last required
       * row has been delivered there is no need to inspect the rest of the
       * caller's chunk.
       */
      while (offset < size && reader->done == 0)
      {
         size_t amount = size - offset;

         if (amount > 256)
            amount = 256;

         png_process_data(reader->png_ptr, reader->info_ptr,
             png_constcast(png_bytep, data + offset), amount);
         offset += amount;
      }
   }

   return reader->done != 0;
}

int PNGAPI
png_region_reader_end(png_const_region_readerp reader)
{
   return reader != NULL && reader->done != 0;
}

int PNGAPI
png_region_reader_get_region(png_const_region_readerp reader,
    png_uint_32 *x, png_uint_32 *y, png_uint_32 *width,
    png_uint_32 *height)
{
   if (reader == NULL || reader->seen_info == 0)
      return 0;

   if (x != NULL)
      *x = reader->x;

   if (y != NULL)
      *y = reader->y;

   if (width != NULL)
      *width = reader->width;

   if (height != NULL)
      *height = reader->height;

   return 1;
}

void PNGAPI
png_region_reader_destroy(png_region_readerp reader)
{
   if (reader != NULL)
   {
      png_structp png_ptr = reader->png_ptr;

      png_free(png_ptr, reader->row);
      png_free(png_ptr, reader->rows);
      png_free(png_ptr, reader);
   }
}

#endif /* PROGRESSIVE_READ */
