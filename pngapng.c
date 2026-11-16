
/* pngapng.c - APNG decoding and compositing
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * The decoder deliberately keeps the APNG layer above the existing libpng
 * read path.  Each frame is a normal PNG sub-image, so libpng performs the
 * filtering, palette expansion, interlacing and bit-depth conversion, while
 * this file owns APNG chunk validation, disposal and alpha compositing.
 */

#include "pngpriv.h"

#ifdef PNG_READ_SUPPORTED

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define PNG_APNG_ERROR_SIZE 192

typedef struct png_apng_frame_record
{
   png_apng_frame frame;
   png_bytep      data;
   size_t         data_size;
   size_t         data_capacity;
   int            have_data;
} png_apng_frame_record, *png_apng_frame_recordp;

struct png_apng_def
{
   png_apng_info          info;
   png_bytep              file;
   size_t                 file_size;

   png_bytep              palette;
   size_t                 palette_size;
   png_bytep              trans;
   size_t                 trans_size;

   png_bytep              canvas;
   png_bytep              previous;
   png_bytep              scratch;
   size_t                 canvas_size;
   png_bytepp             rows;

   png_bytep              pngbuf;
   size_t                 pngbuf_size;
   size_t                 pngbuf_used;

   png_apng_frame_recordp frames;
   png_uint_32            frame_count;
   png_uint_32            frame_capacity;

   png_uint_32            next_frame;
   png_uint_32            current_frame;
   png_apng_frame         last_frame;
   int                    have_last_frame;
   int                    failed;
   int                    valid;
   char                   error[PNG_APNG_ERROR_SIZE];
};

typedef struct png_apng_parse_state
{
   int         have_ihdr;
   int         have_actl;
   int         have_idat;
   int         have_iend;
   int         after_idat_chunk;
   int         sequence_exhausted;
   int         current_frame;
   int         current_uses_idat;
   int         default_image_is_frame;
   png_uint_32 next_sequence;
} png_apng_parse_state;

typedef struct png_apng_memory_reader
{
   png_apngp        apng;
   png_const_bytep  data;
   size_t           size;
   size_t           offset;
   int              failed;
} png_apng_memory_reader, *png_apng_memory_readerp;

static png_uint_32
pnapng_get_u32(png_const_bytep p)
{
   return ((png_uint_32)p[0] << 24) | ((png_uint_32)p[1] << 16) |
          ((png_uint_32)p[2] << 8) | (png_uint_32)p[3];
}

static png_uint_16
pnapng_get_u16(png_const_bytep p)
{
   return (png_uint_16)(((png_uint_16)p[0] << 8) | p[1]);
}

static void
pnapng_put_u32(png_bytep p, png_uint_32 value)
{
   p[0] = (png_byte)(value >> 24);
   p[1] = (png_byte)(value >> 16);
   p[2] = (png_byte)(value >> 8);
   p[3] = (png_byte)value;
}

static png_uint_32
pnapng_crc32(png_uint_32 crc, png_const_bytep data, size_t size)
{
   while (size > UINT_MAX)
   {
      crc = crc32(crc, data, UINT_MAX);
      data += UINT_MAX;
      size -= UINT_MAX;
   }

   if (size != 0)
      crc = crc32(crc, data, (uInt)size);

   return crc;
}

static int
pnapng_error(png_apngp apng, const char *format, ...)
{
   if (apng != NULL && apng->error[0] == 0)
   {
      va_list args;

      va_start(args, format);
      vsnprintf(apng->error, sizeof apng->error, format, args);
      va_end(args);
   }

   return 0;
}

static int
pnapng_reserve(png_apngp apng, png_bytep *buffer, size_t *capacity,
    size_t required)
{
   size_t new_capacity;
   png_bytep new_buffer;

   if (required <= *capacity)
      return 1;

   new_capacity = *capacity == 0 ? 256 : *capacity;
   while (new_capacity < required)
   {
      if (new_capacity > (size_t)-1 / 2)
      {
         new_capacity = required;
         break;
      }

      new_capacity *= 2;
   }

   new_buffer = (png_bytep)realloc(*buffer, new_capacity);
   if (new_buffer == NULL)
      return pnapng_error(apng, "out of memory");

   *buffer = new_buffer;
   *capacity = new_capacity;
   return 1;
}

static int
pnapng_append(png_apngp apng, png_bytep *buffer, size_t *capacity,
    size_t *size, png_const_bytep data, size_t data_size)
{
   size_t required;

   if (data_size == 0)
      return 1;

   if (*size > (size_t)-1 - data_size)
      return pnapng_error(apng, "APNG data is too large");

   required = *size + data_size;
   if (!pnapng_reserve(apng, buffer, capacity, required))
      return 0;

   memcpy(*buffer + *size, data, data_size);
   *size = required;
   return 1;
}

static int
pnapng_is_letter(png_byte value)
{
   return (value >= 'A' && value <= 'Z') ||
          (value >= 'a' && value <= 'z');
}

static int
pnapng_valid_chunk_name(png_const_bytep name)
{
   return pnapng_is_letter(name[0]) && pnapng_is_letter(name[1]) &&
          pnapng_is_letter(name[2]) && pnapng_is_letter(name[3]) &&
          name[2] >= 'A' && name[2] <= 'Z';
}

static int
pnapng_valid_ihdr(png_uint_32 width, png_uint_32 height, png_byte bit_depth,
    png_byte color_type, png_byte compression, png_byte filter,
    png_byte interlace)
{
   int valid_depth = 0;

   if (width == 0 || height == 0 || width > PNG_UINT_31_MAX ||
       height > PNG_UINT_31_MAX)
      return 0;

   switch (color_type)
   {
      case PNG_COLOR_TYPE_GRAY:
         valid_depth = bit_depth == 1 || bit_depth == 2 ||
                       bit_depth == 4 || bit_depth == 8 ||
                       bit_depth == 16;
         break;

      case PNG_COLOR_TYPE_RGB:
      case PNG_COLOR_TYPE_GRAY_ALPHA:
      case PNG_COLOR_TYPE_RGB_ALPHA:
         valid_depth = bit_depth == 8 || bit_depth == 16;
         break;

      case PNG_COLOR_TYPE_PALETTE:
         valid_depth = bit_depth == 1 || bit_depth == 2 ||
                       bit_depth == 4 || bit_depth == 8;
         break;

      default:
         break;
   }

   return valid_depth && compression == PNG_COMPRESSION_TYPE_BASE &&
          filter == PNG_FILTER_TYPE_BASE &&
          (interlace == PNG_INTERLACE_NONE ||
           interlace == PNG_INTERLACE_ADAM7);
}

static int
pnapng_mul_size(size_t left, size_t right, size_t *result)
{
   if (right != 0 && left > (size_t)-1 / right)
      return 0;

   *result = left * right;
   return 1;
}

static int
pnapng_canvas_size(png_uint_32 width, png_uint_32 height, size_t *size)
{
   size_t rowbytes;

   if (!pnapng_mul_size((size_t)width, 4, &rowbytes) ||
       !pnapng_mul_size((size_t)height, rowbytes, size))
      return 0;

   return 1;
}

static int
pnapng_check_sequence(png_apngp apng, png_apng_parse_state *state,
    png_uint_32 sequence)
{
   if (state->sequence_exhausted)
      return pnapng_error(apng, "APNG sequence number overflow");

   if (sequence != state->next_sequence)
      return pnapng_error(apng,
          "APNG sequence error: expected %lu, got %lu",
          (unsigned long)state->next_sequence, (unsigned long)sequence);

   if (state->next_sequence == PNG_UINT_32_MAX)
      state->sequence_exhausted = 1;
   else
      ++state->next_sequence;

   return 1;
}

static int
pnapng_finish_frame(png_apngp apng, png_apng_parse_state *state)
{
   png_apng_frame_recordp record;

   if (state->current_frame < 0)
      return 1;

   record = &apng->frames[state->current_frame];
   if (!record->have_data || record->data_size == 0)
      return pnapng_error(apng, "APNG frame %lu has no image data",
          (unsigned long)record->frame.frame_index);

   state->current_frame = -1;
   state->current_uses_idat = 0;
   return 1;
}

static int
pnapng_add_frame(png_apngp apng, png_apng_parse_state *state,
    png_const_bytep data)
{
   png_apng_frame_recordp record;
   png_apng_frame frame;
   png_uint_32 width;
   png_uint_32 height;
   png_uint_32 x_offset;
   png_uint_32 y_offset;
   png_uint_16 delay_num;
   png_uint_16 delay_den;
   png_byte dispose_op;
   png_byte blend_op;

   if (!pnapng_finish_frame(apng, state))
      return 0;

   if (apng->frame_count >= apng->info.num_frames)
      return pnapng_error(apng, "APNG has more frames than acTL declares");

   width = pnapng_get_u32(data + 4);
   height = pnapng_get_u32(data + 8);
   x_offset = pnapng_get_u32(data + 12);
   y_offset = pnapng_get_u32(data + 16);
   delay_num = pnapng_get_u16(data + 20);
   delay_den = pnapng_get_u16(data + 22);
   dispose_op = data[24];
   blend_op = data[25];

   if (width == 0 || height == 0)
      return pnapng_error(apng, "APNG frame has zero width or height");

   if (x_offset > apng->info.width ||
       width > apng->info.width - x_offset ||
       y_offset > apng->info.height ||
       height > apng->info.height - y_offset)
      return pnapng_error(apng, "APNG frame rectangle exceeds canvas");

   if (dispose_op > PNG_APNG_DISPOSE_OP_PREVIOUS)
      return pnapng_error(apng, "invalid APNG dispose_op %u",
          (unsigned int)dispose_op);

   if (blend_op > PNG_APNG_BLEND_OP_OVER)
      return pnapng_error(apng, "invalid APNG blend_op %u",
          (unsigned int)blend_op);

   if (delay_den == 0)
      delay_den = 100;

   if (apng->frame_count == 0)
   {
      state->default_image_is_frame = state->have_idat == 0;

      if (state->default_image_is_frame &&
          (width != apng->info.width || height != apng->info.height ||
           x_offset != 0 || y_offset != 0))
         return pnapng_error(apng,
             "APNG default image frame must cover the complete canvas");
   }

   if (apng->frame_count == apng->frame_capacity)
   {
      png_uint_32 new_capacity = apng->frame_capacity == 0 ?
          4 : apng->frame_capacity * 2;
      png_apng_frame_recordp new_frames;

      if (new_capacity < apng->frame_capacity)
         return pnapng_error(apng, "too many APNG frames");

      new_frames = (png_apng_frame_recordp)realloc(apng->frames,
          (size_t)new_capacity * sizeof *new_frames);
      if (new_frames == NULL)
         return pnapng_error(apng, "out of memory");

      apng->frames = new_frames;
      apng->frame_capacity = new_capacity;
   }

   memset(&frame, 0, sizeof frame);
   frame.frame_index = apng->frame_count;
   frame.width = width;
   frame.height = height;
   frame.x_offset = x_offset;
   frame.y_offset = y_offset;
   frame.delay_num = delay_num;
   frame.delay_den = delay_den;
   frame.delay_ms = (png_uint_32)delay_num * 1000u;
   frame.delay_ms = (frame.delay_ms + delay_den / 2) / delay_den;
   frame.dispose_op = dispose_op;
   frame.blend_op = blend_op;

   record = &apng->frames[apng->frame_count];
   memset(record, 0, sizeof *record);
   record->frame = frame;

   state->current_frame = (int)apng->frame_count;
   state->current_uses_idat = state->default_image_is_frame &&
       state->current_frame == 0;
   ++apng->frame_count;
   return 1;
}

static int
pnapng_parse_chunks(png_apngp apng)
{
   static const png_byte signature[8] =
       { 137, 80, 78, 71, 13, 10, 26, 10 };
   png_apng_parse_state state;
   size_t offset = 8;

   memset(&state, 0, sizeof state);
   state.current_frame = -1;

   if (apng->file_size < 8 ||
       memcmp(apng->file, signature, sizeof signature) != 0)
      return pnapng_error(apng, "not a PNG file");

   while (offset + 12 <= apng->file_size)
   {
      png_const_bytep chunk = apng->file + offset;
      png_const_bytep name = chunk + 4;
      png_const_bytep data = chunk + 8;
      png_uint_32 length = pnapng_get_u32(chunk);
      png_uint_32 stored_crc;
      png_uint_32 crc;

      if (length > PNG_UINT_31_MAX ||
          (size_t)length > apng->file_size - offset - 12)
         return pnapng_error(apng, "truncated APNG chunk");

      if (!pnapng_valid_chunk_name(name))
         return pnapng_error(apng, "invalid APNG chunk name");

      stored_crc = pnapng_get_u32(data + length);
      crc = crc32(0L, Z_NULL, 0);
      crc = crc32(crc, name, 4);
      crc = pnapng_crc32(crc, data, length);

      if (crc != stored_crc)
         return pnapng_error(apng, "CRC error in APNG chunk %.4s", name);

      if (!state.have_ihdr && memcmp(name, "IHDR", 4) != 0)
         return pnapng_error(apng, "IHDR is not the first APNG chunk");

      if (state.have_idat && memcmp(name, "IDAT", 4) != 0)
         state.after_idat_chunk = 1;

      if (memcmp(name, "IHDR", 4) == 0)
      {
         png_uint_32 width;
         png_uint_32 height;
         png_byte bit_depth;
         png_byte color_type;
         png_byte compression;
         png_byte filter;
         png_byte interlace;

         if (state.have_ihdr)
            return pnapng_error(apng, "duplicate IHDR");

         if (length != 13)
            return pnapng_error(apng, "invalid IHDR length");

         width = pnapng_get_u32(data);
         height = pnapng_get_u32(data + 4);
         bit_depth = data[8];
         color_type = data[9];
         compression = data[10];
         filter = data[11];
         interlace = data[12];

         if (!pnapng_valid_ihdr(width, height, bit_depth, color_type,
             compression, filter, interlace))
            return pnapng_error(apng, "invalid IHDR");

         if (!pnapng_canvas_size(width, height, &apng->canvas_size))
            return pnapng_error(apng, "APNG canvas is too large");

         apng->info.width = width;
         apng->info.height = height;
         apng->info.bit_depth = bit_depth;
         apng->info.color_type = color_type;
         apng->info.interlace_type = interlace;
         state.have_ihdr = 1;
      }

      else if (memcmp(name, "acTL", 4) == 0)
      {
         if (length != 8)
            return pnapng_error(apng, "invalid acTL length");

         if (state.have_actl)
            return pnapng_error(apng, "duplicate acTL");

         if (state.have_idat)
            return pnapng_error(apng, "acTL after IDAT");

         if (state.current_frame >= 0)
            return pnapng_error(apng, "acTL after fcTL");

         apng->info.num_frames = pnapng_get_u32(data);
         apng->info.num_plays = pnapng_get_u32(data + 4);

         if (apng->info.num_frames == 0)
            return pnapng_error(apng, "acTL declares zero frames");

         state.have_actl = 1;
      }

      else if (memcmp(name, "fcTL", 4) == 0)
      {
         png_uint_32 sequence;

         if (length != 26)
            return pnapng_error(apng, "invalid fcTL length");

         if (!state.have_actl)
            return pnapng_error(apng, "fcTL before acTL");

         sequence = pnapng_get_u32(data);
         if (!pnapng_check_sequence(apng, &state, sequence))
            return 0;

         if (!pnapng_add_frame(apng, &state, data))
            return 0;
      }

      else if (memcmp(name, "IDAT", 4) == 0)
      {
         if (state.after_idat_chunk)
            return pnapng_error(apng, "IDAT chunks are not consecutive");

         state.have_idat = 1;

         if (state.current_frame == 0 && state.current_uses_idat)
         {
            png_apng_frame_recordp record =
                &apng->frames[state.current_frame];

            if (!pnapng_append(apng, &record->data, &record->data_capacity,
                &record->data_size, data, length))
               return 0;

            if (length != 0)
               record->have_data = 1;
         }
      }

      else if (memcmp(name, "fdAT", 4) == 0)
      {
         png_uint_32 sequence;
         png_apng_frame_recordp record;

         if (length < 4)
            return pnapng_error(apng, "invalid fdAT length");

         if (!state.have_actl)
            return pnapng_error(apng, "fdAT before acTL");

         if (state.current_frame < 0)
            return pnapng_error(apng, "fdAT without fcTL");

         sequence = pnapng_get_u32(data);
         if (!pnapng_check_sequence(apng, &state, sequence))
            return 0;

         if (state.current_uses_idat)
            return pnapng_error(apng,
                "fdAT used for the default-image frame");

         record = &apng->frames[state.current_frame];
         if (!pnapng_append(apng, &record->data, &record->data_capacity,
             &record->data_size, data + 4, length - 4))
            return 0;

         if (length > 4)
            record->have_data = 1;
      }

      else if (memcmp(name, "IEND", 4) == 0)
      {
         if (length != 0)
            return pnapng_error(apng, "invalid IEND length");

         if (!pnapng_finish_frame(apng, &state))
            return 0;

         state.have_iend = 1;
         break;
      }

      else
      {
         if (memcmp(name, "PLTE", 4) == 0)
         {
            if (state.have_idat)
               return pnapng_error(apng, "PLTE after IDAT");

            if (apng->palette != NULL)
               return pnapng_error(apng, "duplicate PLTE");

            if (length == 0 || length > 768 || length % 3 != 0)
               return pnapng_error(apng, "invalid PLTE length");

            apng->palette = (png_bytep)malloc(length);
            if (apng->palette == NULL)
               return pnapng_error(apng, "out of memory");

            memcpy(apng->palette, data, length);
            apng->palette_size = length;
         }

         else if (memcmp(name, "tRNS", 4) == 0)
         {
            if (state.have_idat)
               return pnapng_error(apng, "tRNS after IDAT");

            if (apng->info.color_type == PNG_COLOR_TYPE_PALETTE &&
                apng->palette == NULL)
               return pnapng_error(apng, "tRNS before PLTE");

            if (apng->trans != NULL)
               return pnapng_error(apng, "duplicate tRNS");

            if (length == 0 || length > 256)
               return pnapng_error(apng, "invalid tRNS length");

            apng->trans = (png_bytep)malloc(length);
            if (apng->trans == NULL)
               return pnapng_error(apng, "out of memory");

            memcpy(apng->trans, data, length);
            apng->trans_size = length;
         }
      }

      offset += 12 + (size_t)length;
   }

   if (!state.have_ihdr)
      return pnapng_error(apng, "missing IHDR");

   if (!state.have_actl)
      return pnapng_error(apng, "missing acTL (not an APNG)");

   if (!state.have_idat)
      return pnapng_error(apng, "missing IDAT");

   if (!state.have_iend)
      return pnapng_error(apng, "missing IEND");

   if (apng->frame_count != apng->info.num_frames)
      return pnapng_error(apng, "acTL declares %lu frames, found %lu",
          (unsigned long)apng->info.num_frames,
          (unsigned long)apng->frame_count);

   switch (apng->info.color_type)
   {
      case PNG_COLOR_TYPE_PALETTE:
         if (apng->palette == NULL)
            return pnapng_error(apng, "palette image has no PLTE");

         if (apng->trans_size > apng->palette_size / 3)
            return pnapng_error(apng, "tRNS is larger than the palette");
         break;

      case PNG_COLOR_TYPE_GRAY:
         if (apng->palette != NULL)
            return pnapng_error(apng, "PLTE is not valid for grayscale");

         if (apng->trans_size != 0 && apng->trans_size != 2)
            return pnapng_error(apng, "invalid grayscale tRNS length");
         break;

      case PNG_COLOR_TYPE_RGB:
         if (apng->trans_size != 0 && apng->trans_size != 6)
            return pnapng_error(apng, "invalid RGB tRNS length");
         break;

      default:
         if (apng->trans_size != 0)
            return pnapng_error(apng,
                "tRNS is not valid for an image with alpha");
         break;
   }

   apng->info.default_image_is_frame =
       (png_byte)state.default_image_is_frame;

   {
      size_t rows_size;

      if (!pnapng_mul_size((size_t)apng->info.height, sizeof(png_bytep),
          &rows_size))
         return pnapng_error(apng, "APNG canvas is too tall");

      apng->canvas = (png_bytep)calloc(apng->canvas_size == 0 ? 1 :
          apng->canvas_size, 1);
      apng->previous = (png_bytep)malloc(apng->canvas_size == 0 ? 1 :
          apng->canvas_size);
      apng->scratch = (png_bytep)malloc(apng->canvas_size == 0 ? 1 :
          apng->canvas_size);
      apng->rows = (png_bytepp)malloc(rows_size);
   }

   if (apng->canvas == NULL || apng->previous == NULL ||
       apng->scratch == NULL || apng->rows == NULL)
      return pnapng_error(apng, "out of memory");

   apng->valid = 1;
   free(apng->file);
   apng->file = NULL;
   apng->file_size = 0;
   return 1;
}

static png_apngp
pnapng_create_empty(void)
{
   png_apngp apng = (png_apngp)calloc(1, sizeof *apng);

   if (apng != NULL)
   {
      apng->current_frame = PNG_APNG_NO_FRAME;
      apng->next_frame = 0;
   }

   return apng;
}

png_apngp PNGAPI
png_apng_create_read_memory(png_const_bytep data, size_t size)
{
   png_apngp apng = pnapng_create_empty();

   if (apng == NULL)
      return NULL;

   if (data == NULL || size == 0)
   {
      pnapng_error(apng, "empty APNG input");
      return apng;
   }

   apng->file = (png_bytep)malloc(size);
   if (apng->file == NULL)
   {
      pnapng_error(apng, "out of memory");
      return apng;
   }

   memcpy(apng->file, data, size);
   apng->file_size = size;
   (void)pnapng_parse_chunks(apng);
   return apng;
}

png_apngp PNGAPI
png_apng_create_read_file(png_const_charp file_name)
{
   png_apngp apng = pnapng_create_empty();
   FILE *file;
   long length;

   if (apng == NULL)
      return NULL;

   if (file_name == NULL)
   {
      pnapng_error(apng, "NULL APNG file name");
      return apng;
   }

   file = fopen(file_name, "rb");
   if (file == NULL)
   {
      pnapng_error(apng, "cannot open APNG file");
      return apng;
   }

   if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0)
   {
      fclose(file);
      pnapng_error(apng, "cannot determine APNG file size");
      return apng;
   }

   if (fseek(file, 0, SEEK_SET) != 0)
   {
      fclose(file);
      pnapng_error(apng, "cannot rewind APNG file");
      return apng;
   }

   apng->file_size = (size_t)length;
   apng->file = (png_bytep)malloc(apng->file_size == 0 ? 1 :
       apng->file_size);
   if (apng->file == NULL)
   {
      fclose(file);
      pnapng_error(apng, "out of memory");
      return apng;
   }

   if (apng->file_size != 0 &&
       fread(apng->file, 1, apng->file_size, file) != apng->file_size)
   {
      fclose(file);
      pnapng_error(apng, "cannot read APNG file");
      return apng;
   }

   fclose(file);
   (void)pnapng_parse_chunks(apng);
   return apng;
}

void PNGAPI
png_apng_destroy(png_apngpp apng_ptr)
{
   png_uint_32 i;

   if (apng_ptr == NULL || *apng_ptr == NULL)
      return;

   for (i = 0; i < (*apng_ptr)->frame_count; ++i)
      free((*apng_ptr)->frames[i].data);

   free((*apng_ptr)->frames);
   free((*apng_ptr)->palette);
   free((*apng_ptr)->trans);
   free((*apng_ptr)->file);
   free((*apng_ptr)->canvas);
   free((*apng_ptr)->previous);
   free((*apng_ptr)->scratch);
   free((*apng_ptr)->rows);
   free((*apng_ptr)->pngbuf);
   free(*apng_ptr);
   *apng_ptr = NULL;
}

int PNGAPI
png_apng_is_valid(png_const_apngp apng)
{
   return apng != NULL && apng->valid && !apng->failed;
}

png_const_charp PNGAPI
png_apng_get_error(png_const_apngp apng)
{
   if (apng == NULL)
      return "invalid APNG decoder";

   return apng->error;
}

int PNGAPI
png_apng_get_info(png_const_apngp apng, png_apng_info *info)
{
   if (apng == NULL || info == NULL || !apng->valid || apng->failed)
      return 0;

   *info = apng->info;
   return 1;
}

int PNGAPI
png_apng_rewind(png_apngp apng)
{
   if (apng == NULL || !apng->valid)
      return 0;

   if (apng->canvas != NULL)
      memset(apng->canvas, 0, apng->canvas_size);

   apng->next_frame = 0;
   apng->current_frame = PNG_APNG_NO_FRAME;
   apng->have_last_frame = 0;
   apng->failed = 0;
   apng->error[0] = 0;
   return 1;
}

png_uint_32 PNGAPI
png_apng_get_current_frame(png_const_apngp apng)
{
   if (apng == NULL || !apng->valid || apng->current_frame ==
       PNG_APNG_NO_FRAME)
      return PNG_APNG_NO_FRAME;

   return apng->current_frame;
}

static void PNGCBAPI
pnapng_png_warning(png_structp png_ptr, png_const_charp warning)
{
   PNG_UNUSED(png_ptr)
   PNG_UNUSED(warning)
}

static void PNGCBAPI
pnapng_png_error(png_structp png_ptr, png_const_charp message)
{
   png_apng_memory_readerp reader =
       (png_apng_memory_readerp)png_get_error_ptr(png_ptr);

   if (reader != NULL)
   {
      reader->failed = 1;
      pnapng_error(reader->apng, "PNG frame decode failed: %s", message);
   }

   png_longjmp(png_ptr, 1);
}

static void PNGCBAPI
pnapng_read_data(png_structp png_ptr, png_bytep data, size_t size)
{
   png_apng_memory_readerp reader =
       (png_apng_memory_readerp)png_get_io_ptr(png_ptr);

   if (reader == NULL || size > reader->size - reader->offset)
   {
      png_error(png_ptr, "unexpected end of APNG frame");
      return;
   }

   memcpy(data, reader->data + reader->offset, size);
   reader->offset += size;
}

static int
pnapng_put_chunk(png_apngp apng, png_bytep *output, png_const_bytep name,
    png_const_bytep data, size_t size)
{
   png_uint_32 crc;

   if (size > PNG_UINT_31_MAX)
      return pnapng_error(apng, "APNG chunk is too large");

   pnapng_put_u32(*output, (png_uint_32)size);
   memcpy(*output + 4, name, 4);

   if (size != 0)
      memcpy(*output + 8, data, size);

   crc = crc32(0L, Z_NULL, 0);
   crc = crc32(crc, name, 4);
   crc = pnapng_crc32(crc, data, size);
   pnapng_put_u32(*output + 8 + size, crc);

   *output += 12 + size;
   return 1;
}

static int
pnapng_build_frame_png(png_apngp apng, png_apng_frame_recordp record)
{
   static const png_byte signature[8] =
       { 137, 80, 78, 71, 13, 10, 26, 10 };
   png_byte ihdr[13];
   png_bytep output;
   size_t required;

   if (record->data_size > PNG_UINT_31_MAX)
      return pnapng_error(apng, "APNG frame data is too large");

   required = 8 + 12 + sizeof ihdr + 12 + 12;
   if (record->data_size > (size_t)-1 - required)
      return pnapng_error(apng, "APNG frame is too large");
   required += record->data_size;

   if (apng->palette_size != 0)
   {
      if (apng->palette_size > (size_t)-1 - required - 12)
         return pnapng_error(apng, "APNG frame is too large");
      required += 12 + apng->palette_size;
   }

   if (apng->trans_size != 0)
   {
      if (apng->trans_size > (size_t)-1 - required - 12)
         return pnapng_error(apng, "APNG frame is too large");
      required += 12 + apng->trans_size;
   }

   if (!pnapng_reserve(apng, &apng->pngbuf, &apng->pngbuf_size, required))
      return 0;

   output = apng->pngbuf;
   memcpy(output, signature, sizeof signature);
   output += sizeof signature;

   pnapng_put_u32(ihdr, record->frame.width);
   pnapng_put_u32(ihdr + 4, record->frame.height);
   ihdr[8] = apng->info.bit_depth;
   ihdr[9] = apng->info.color_type;
   ihdr[10] = PNG_COMPRESSION_TYPE_BASE;
   ihdr[11] = PNG_FILTER_TYPE_BASE;
   ihdr[12] = apng->info.interlace_type;

   if (!pnapng_put_chunk(apng, &output, (png_const_bytep)"IHDR", ihdr,
       sizeof ihdr))
      return 0;

   if (apng->palette_size != 0 &&
       !pnapng_put_chunk(apng, &output, (png_const_bytep)"PLTE",
       apng->palette, apng->palette_size))
      return 0;

   if (apng->trans_size != 0 &&
       !pnapng_put_chunk(apng, &output, (png_const_bytep)"tRNS",
       apng->trans, apng->trans_size))
      return 0;

   if (!pnapng_put_chunk(apng, &output, (png_const_bytep)"IDAT",
       record->data, record->data_size))
      return 0;

   if (!pnapng_put_chunk(apng, &output, (png_const_bytep)"IEND", NULL, 0))
      return 0;

   apng->pngbuf_used = (size_t)(output - apng->pngbuf);
   return 1;
}

static int
pnapng_decode_frame(png_apngp apng, png_apng_frame_recordp record)
{
   png_apng_memory_reader reader;
   png_structp png_ptr = NULL;
   png_infop info_ptr = NULL;
   png_byte color_type;
   png_uint_32 y;
   size_t rowbytes;
   int result = 0;

   if (!pnapng_build_frame_png(apng, record))
      return 0;

   memset(&reader, 0, sizeof reader);
   reader.apng = apng;
   reader.data = apng->pngbuf;
   reader.size = apng->pngbuf_used;

   png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, &reader,
       pnapng_png_error, pnapng_png_warning);
   if (png_ptr == NULL)
      return pnapng_error(apng, "out of memory");

   info_ptr = png_create_info_struct(png_ptr);
   if (info_ptr == NULL)
   {
      png_destroy_read_struct(&png_ptr, NULL, NULL);
      return pnapng_error(apng, "out of memory");
   }

   if (setjmp(png_jmpbuf(png_ptr)) != 0)
   {
      if (apng->error[0] == 0)
         pnapng_error(apng, "PNG frame decode failed");
      result = 0;
      goto done;
   }

   png_set_read_fn(png_ptr, &reader, pnapng_read_data);
   png_read_info(png_ptr, info_ptr);

   color_type = png_get_color_type(png_ptr, info_ptr);

   png_set_expand(png_ptr);
   png_set_strip_16(png_ptr);
   png_set_gray_to_rgb(png_ptr);

   if ((color_type & PNG_COLOR_MASK_ALPHA) == 0 &&
       png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS) == 0)
      png_set_add_alpha(png_ptr, 0xff, PNG_FILLER_AFTER);

   (void)png_set_interlace_handling(png_ptr);
   png_read_update_info(png_ptr, info_ptr);

   rowbytes = png_get_rowbytes(png_ptr, info_ptr);
   if (rowbytes != (size_t)record->frame.width * 4 ||
       png_get_channels(png_ptr, info_ptr) != 4)
   {
      pnapng_error(apng, "unexpected frame output format");
      goto done;
   }

   if (rowbytes > (size_t)-1 / record->frame.height ||
       rowbytes * record->frame.height > apng->canvas_size)
   {
      pnapng_error(apng, "frame decode buffer is too small");
      goto done;
   }

   for (y = 0; y < record->frame.height; ++y)
      apng->rows[y] = apng->scratch + (size_t)y * rowbytes;

   png_read_image(png_ptr, apng->rows);
   png_read_end(png_ptr, NULL);
   result = reader.failed == 0;

done:
   png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
   return result;
}

static void
pnapng_clear_rectangle(png_apngp apng, const png_apng_frame *frame)
{
   size_t rowbytes = (size_t)apng->info.width * 4;
   png_uint_32 y;

   for (y = 0; y < frame->height; ++y)
   {
      png_bytep row = apng->canvas +
          (size_t)(frame->y_offset + y) * rowbytes +
          (size_t)frame->x_offset * 4;

      memset(row, 0, (size_t)frame->width * 4);
   }
}

static void
pnapng_blend_pixel(png_bytep destination, png_const_bytep source)
{
   png_uint_32 source_alpha = source[3];
   png_uint_32 destination_alpha = destination[3];
   png_uint_32 output_alpha;
   png_uint_32 channel;

   if (source_alpha == 0)
      return;

   if (source_alpha == 255)
   {
      destination[0] = source[0];
      destination[1] = source[1];
      destination[2] = source[2];
      destination[3] = 255;
      return;
   }

   /* output_alpha = sa + (1-sa) * da, using 255 as one. */
   output_alpha = source_alpha +
       ((((255 - source_alpha) * destination_alpha + 128) * 257) >> 16);

   for (channel = 0; channel < 3; ++channel)
   {
      png_uint_32 value;

      if (destination_alpha == 255)
      {
         value = source_alpha * source[channel] +
             (255 - source_alpha) * destination[channel];
         destination[channel] =
             (png_byte)(((value + 128) * 257) >> 16);
      }

      else
      {
         value = 255 * source_alpha * source[channel] +
             (255 - source_alpha) * destination_alpha *
             destination[channel];
         destination[channel] =
             (png_byte)(value / (255 * output_alpha));
      }
   }

   destination[3] = (png_byte)output_alpha;
}

static void
pnapng_composite_frame(png_apngp apng, png_apng_frame_recordp record)
{
   size_t canvas_rowbytes = (size_t)apng->info.width * 4;
   size_t frame_rowbytes = (size_t)record->frame.width * 4;
   png_uint_32 y;

   for (y = 0; y < record->frame.height; ++y)
   {
      png_bytep destination = apng->canvas +
          (size_t)(record->frame.y_offset + y) * canvas_rowbytes +
          (size_t)record->frame.x_offset * 4;
      png_const_bytep source = apng->scratch +
          (size_t)y * frame_rowbytes;

      if (record->frame.blend_op == PNG_APNG_BLEND_OP_SOURCE)
         memcpy(destination, source, frame_rowbytes);

      else
      {
         png_uint_32 x;

         for (x = 0; x < record->frame.width; ++x)
         {
            pnapng_blend_pixel(destination + (size_t)x * 4,
                source + (size_t)x * 4);
         }
      }
   }
}

static int
pnapng_next_frame_impl(png_apngp apng, png_bytep rgba, size_t stride,
    png_apng_frame *frame)
{
   png_apng_frame_recordp record;
   size_t rowbytes;
   png_uint_32 y;

   if (apng == NULL || !apng->valid)
   {
      pnapng_error(apng, "invalid APNG decoder");
      return -1;
   }

   if (apng->failed)
      return -1;

   if (apng->next_frame >= apng->info.num_frames)
      return 0;

   rowbytes = (size_t)apng->info.width * 4;
   if (rgba != NULL && stride != 0 && stride < rowbytes)
   {
      pnapng_error(apng, "RGBA stride is smaller than a canvas row");
      return -1;
   }

   if (apng->have_last_frame)
   {
      if (apng->last_frame.dispose_op == PNG_APNG_DISPOSE_OP_BACKGROUND)
         pnapng_clear_rectangle(apng, &apng->last_frame);

      else if (apng->last_frame.dispose_op == PNG_APNG_DISPOSE_OP_PREVIOUS)
         memcpy(apng->canvas, apng->previous, apng->canvas_size);
   }

   record = &apng->frames[apng->next_frame];
   if (!pnapng_decode_frame(apng, record))
   {
      apng->failed = 1;
      return -1;
   }

   if (record->frame.dispose_op == PNG_APNG_DISPOSE_OP_PREVIOUS)
      memcpy(apng->previous, apng->canvas, apng->canvas_size);

   pnapng_composite_frame(apng, record);

   if (rgba != NULL)
   {
      size_t output_stride = stride == 0 ? rowbytes : stride;

      for (y = 0; y < apng->info.height; ++y)
         memcpy(rgba + (size_t)y * output_stride,
             apng->canvas + (size_t)y * rowbytes, rowbytes);
   }

   apng->current_frame = apng->next_frame;
   apng->last_frame = record->frame;
   apng->have_last_frame = 1;
   ++apng->next_frame;

   if (frame != NULL)
      *frame = record->frame;

   return 1;
}

int PNGAPI
png_apng_next_frame(png_apngp apng, png_bytep rgba, size_t stride,
    png_apng_frame *frame)
{
   return pnapng_next_frame_impl(apng, rgba, stride, frame);
}

int PNGAPI
png_apng_seek_frame(png_apngp apng, png_uint_32 frame_index)
{
   if (apng == NULL || !apng->valid || apng->failed)
      return 0;

   if (frame_index >= apng->info.num_frames)
      return pnapng_error(apng,
          "APNG frame index %lu is out of range", (unsigned long)frame_index);

   if (apng->current_frame == frame_index)
      return 1;

   if (apng->current_frame == PNG_APNG_NO_FRAME ||
       frame_index < apng->current_frame)
   {
      if (!png_apng_rewind(apng))
         return 0;
   }

   while (apng->current_frame == PNG_APNG_NO_FRAME ||
          apng->current_frame < frame_index)
   {
      int result = pnapng_next_frame_impl(apng, NULL, 0, NULL);

      if (result != 1)
         return 0;
   }

   return 1;
}

#endif /* PNG_READ_SUPPORTED */
