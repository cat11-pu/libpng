
/* pngapng.c - self-test for the libpng APNG decoder
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "png.h"

typedef struct
{
   png_bytep data;
   size_t   size;
   size_t   capacity;
} byte_buffer;

typedef struct
{
   png_uint_32 width;
   png_uint_32 height;
   png_uint_32 x_offset;
   png_uint_32 y_offset;
   png_uint_16 delay_num;
   png_uint_16 delay_den;
   png_byte    dispose_op;
   png_byte    blend_op;
   png_bytep   rgba;
} test_frame;

typedef struct
{
   png_uint_32 width;
   png_uint_32 height;
   png_byte    bit_depth;
   png_byte    color_type;
   png_byte    interlace_type;
   png_uint_32 num_plays;
   int         default_image_is_frame;
   int         split_fdat;
   int         omit_actl;
   int         actl_after_idat;
   int         duplicate_actl;
   int         fctl_before_actl;
   int         omit_frame_data;
   int         frame_count_override;
   png_uint_32 declared_frames;

   png_bytep   default_rgba;
   png_bytep   palette;
   png_uint_16 palette_count;
   png_uint_16 num_trans;

   test_frame *frames;
   png_uint_32 num_frames;
} test_apng;

typedef struct
{
   png_bytep canvas;
   png_bytep previous;
   png_bytep save;
   png_bytep scratch;
   int       have_last;
   test_frame last;
} reference_state;

static unsigned long long total_frames;
static unsigned long long total_pixels;
static unsigned long long total_bytes;
static unsigned int total_invalid_cases;
static int failures;

static void
fail(const char *test, const char *message)
{
   fprintf(stderr, "FAIL %s: %s\n", test, message);
   ++failures;
}

static png_uint_32
get_u32(png_const_bytep p)
{
   return ((png_uint_32)p[0] << 24) | ((png_uint_32)p[1] << 16) |
          ((png_uint_32)p[2] << 8) | (png_uint_32)p[3];
}

static void
put_u32(png_bytep p, png_uint_32 value)
{
   p[0] = (png_byte)(value >> 24);
   p[1] = (png_byte)(value >> 16);
   p[2] = (png_byte)(value >> 8);
   p[3] = (png_byte)value;
}

static void
put_u16(png_bytep p, png_uint_16 value)
{
   p[0] = (png_byte)(value >> 8);
   p[1] = (png_byte)value;
}

static int
buffer_reserve(byte_buffer *buffer, size_t required)
{
   size_t capacity;
   png_bytep data;

   if (required <= buffer->capacity)
      return 1;

   capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
   while (capacity < required)
   {
      if (capacity > (size_t)-1 / 2)
      {
         capacity = required;
         break;
      }

      capacity *= 2;
   }

   data = (png_bytep)realloc(buffer->data, capacity);
   if (data == NULL)
      return 0;

   buffer->data = data;
   buffer->capacity = capacity;
   return 1;
}

static int
buffer_append(byte_buffer *buffer, png_const_bytep data, size_t size)
{
   if (size > (size_t)-1 - buffer->size ||
       !buffer_reserve(buffer, buffer->size + size))
      return 0;

   if (size != 0)
      memcpy(buffer->data + buffer->size, data, size);

   buffer->size += size;
   return 1;
}

static void
buffer_free(byte_buffer *buffer)
{
   free(buffer->data);
   memset(buffer, 0, sizeof *buffer);
}

static int
put_chunk(byte_buffer *buffer, png_const_charp type, png_const_bytep data,
    size_t size)
{
   png_byte header[8];
   png_byte crc_bytes[4];
   png_uint_32 crc;

   if (size > PNG_UINT_31_MAX)
      return 0;

   put_u32(header, (png_uint_32)size);
   memcpy(header + 4, type, 4);

   crc = crc32(0L, Z_NULL, 0);
   crc = crc32(crc, header + 4, 4);
   if (size != 0)
      crc = crc32(crc, data, size);
   put_u32(crc_bytes, crc);

   return buffer_append(buffer, header, sizeof header) &&
          buffer_append(buffer, data, size) &&
          buffer_append(buffer, crc_bytes, sizeof crc_bytes);
}

static png_uint_32
channels_for(png_byte color_type)
{
   switch (color_type)
   {
      case PNG_COLOR_TYPE_GRAY:
         return 1;
      case PNG_COLOR_TYPE_GRAY_ALPHA:
         return 2;
      case PNG_COLOR_TYPE_RGB:
         return 3;
      case PNG_COLOR_TYPE_RGB_ALPHA:
         return 4;
      case PNG_COLOR_TYPE_PALETTE:
         return 1;
      default:
         return 0;
   }
}

static int
find_palette_index(const test_apng *apng, png_const_bytep rgba,
    png_uint_16 *index)
{
   png_uint_16 i;

   for (i = 0; i < apng->palette_count; ++i)
   {
      png_const_bytep entry = apng->palette + (size_t)i * 4;
      png_byte alpha = i < apng->num_trans ? entry[3] : 255;

      if (entry[0] == rgba[0] && entry[1] == rgba[1] &&
          entry[2] == rgba[2] && alpha == rgba[3])
      {
         *index = i;
         return 1;
      }
   }

   return 0;
}

static int
encode_row(const test_apng *apng, const test_frame *frame, png_uint_32 y,
    png_uint_32 x_start, png_uint_32 x_step, png_bytep row)
{
   png_uint_32 channels = channels_for(apng->color_type);
   png_uint_32 x;
   size_t output = 0;

   for (x = x_start; x < frame->width; x += x_step, ++output)
   {
      png_const_bytep pixel = frame->rgba +
          ((size_t)y * frame->width + x) * 4;

      if (apng->color_type == PNG_COLOR_TYPE_PALETTE)
      {
         png_uint_16 index;

         if (!find_palette_index(apng, pixel, &index))
            return 0;

         row[output] = (png_byte)index;
      }

      else if (apng->bit_depth == 8)
      {
         png_bytep out = row + output * channels;

         switch (apng->color_type)
         {
            case PNG_COLOR_TYPE_GRAY:
               out[0] = pixel[0];
               break;
            case PNG_COLOR_TYPE_GRAY_ALPHA:
               out[0] = pixel[0];
               out[1] = pixel[3];
               break;
            case PNG_COLOR_TYPE_RGB:
               out[0] = pixel[0];
               out[1] = pixel[1];
               out[2] = pixel[2];
               break;
            case PNG_COLOR_TYPE_RGB_ALPHA:
               memcpy(out, pixel, 4);
               break;
            default:
               return 0;
         }
      }

      else
      {
         png_bytep out = row + output * channels * 2;
         png_uint_32 c;

         if (apng->color_type == PNG_COLOR_TYPE_GRAY)
         {
            out[0] = pixel[0];
            out[1] = pixel[0];
         }

         else if (apng->color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
         {
            out[0] = pixel[0];
            out[1] = pixel[0];
            out[2] = pixel[3];
            out[3] = pixel[3];
         }

         else
         {
            for (c = 0; c < channels; ++c)
            {
               png_byte value = pixel[c];
               out[c * 2] = value;
               out[c * 2 + 1] = value;
            }
         }
      }
   }

   return 1;
}

static int
encode_frame(const test_apng *apng, const test_frame *frame,
    png_bytep *compressed, size_t *compressed_size)
{
   static const png_uint_32 passes[7][4] =
   {
      { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 },
      { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 },
      { 0, 1, 1, 2 }
   };
   png_uint_32 channels = channels_for(apng->color_type);
   size_t sample_size = apng->bit_depth == 16 ? 2 : 1;
   size_t max_rowbytes = (size_t)frame->width * channels * sample_size;
   size_t raw_size = (size_t)(frame->height + 7) * (max_rowbytes + 1);
   size_t raw_used = 0;
   png_bytep raw;
   png_bytep compressed_data;
   uLongf dest_len;
   png_uint_32 y;
   int result = 0;

   if (apng->color_type == PNG_COLOR_TYPE_PALETTE && apng->bit_depth == 8)
      max_rowbytes = frame->width;

   raw = (png_bytep)calloc(raw_size == 0 ? 1 : raw_size, 1);
   if (raw == NULL)
      return 0;

   if (apng->interlace_type == PNG_INTERLACE_ADAM7)
   {
      size_t pass_index;

      for (pass_index = 0; pass_index < 7; ++pass_index)
      {
         png_uint_32 x_start = passes[pass_index][0];
         png_uint_32 y_start = passes[pass_index][1];
         png_uint_32 x_step = passes[pass_index][2];
         png_uint_32 y_step = passes[pass_index][3];
         size_t pass_width;
         size_t pass_rowbytes;

         if (x_start >= frame->width || y_start >= frame->height)
            continue;

         pass_width = (frame->width - x_start + x_step - 1) / x_step;
         pass_rowbytes = pass_width * channels * sample_size;
         if (apng->color_type == PNG_COLOR_TYPE_PALETTE &&
             apng->bit_depth == 8)
            pass_rowbytes = pass_width;

         for (y = y_start; y < frame->height; y += y_step)
         {
            png_bytep row = raw + raw_used;

            row[0] = 0;
            if (!encode_row(apng, frame, y, x_start, x_step, row + 1))
            {
               free(raw);
               return 0;
            }

            raw_used += pass_rowbytes + 1;
         }
      }
   }

   else
   {
      size_t rowbytes = max_rowbytes;

      for (y = 0; y < frame->height; ++y)
      {
         png_bytep row = raw + raw_used;

         row[0] = 0;
         if (!encode_row(apng, frame, y, 0, 1, row + 1))
         {
            free(raw);
            return 0;
         }

         raw_used += rowbytes + 1;
      }
   }

   dest_len = compressBound((uLong)raw_used);
   compressed_data = (png_bytep)malloc(dest_len == 0 ? 1 : dest_len);
   if (compressed_data == NULL)
   {
      free(raw);
      return 0;
   }

   if (compress2(compressed_data, &dest_len, raw, (uLong)raw_used,
       Z_BEST_COMPRESSION) == Z_OK)
   {
      *compressed = compressed_data;
      *compressed_size = dest_len;
      result = 1;
   }

   else
      free(compressed_data);

   free(raw);
   return result;
}

static int
put_fctl(byte_buffer *buffer, png_uint_32 sequence, const test_frame *frame)
{
   png_byte data[26];

   put_u32(data, sequence);
   put_u32(data + 4, frame->width);
   put_u32(data + 8, frame->height);
   put_u32(data + 12, frame->x_offset);
   put_u32(data + 16, frame->y_offset);
   put_u16(data + 20, frame->delay_num);
   put_u16(data + 22, frame->delay_den);
   data[24] = frame->dispose_op;
   data[25] = frame->blend_op;
   return put_chunk(buffer, "fcTL", data, sizeof data);
}

static int
put_fdat(byte_buffer *buffer, png_uint_32 sequence, png_const_bytep data,
    size_t size, int split, png_uint_32 *chunks)
{
   png_bytep payload;
   size_t parts = split ? 3 : 1;
   size_t part_size = (size + parts - 1) / parts;
   size_t offset = 0;
   png_uint_32 count = 0;

   payload = (png_bytep)malloc(4 + part_size);
   if (payload == NULL)
      return 0;

   while (offset < size)
   {
      size_t length = size - offset;

      if (length > part_size)
         length = part_size;

      put_u32(payload, sequence++);
      memcpy(payload + 4, data + offset, length);

      if (!put_chunk(buffer, "fdAT", payload, length + 4))
      {
         free(payload);
         return 0;
      }

      offset += length;
      ++count;
   }

   free(payload);
   *chunks = count;
   return 1;
}

static int
make_apng(const test_apng *apng, png_bytep *output, size_t *output_size)
{
   static const png_byte signature[8] =
       { 137, 80, 78, 71, 13, 10, 26, 10 };
   byte_buffer buffer;
   png_byte ihdr[13];
   png_byte actl[8];
   png_bytep compressed = NULL;
   size_t compressed_size = 0;
   png_uint_32 sequence = 0;
   png_uint_32 i;
   int result = 0;

   memset(&buffer, 0, sizeof buffer);
   put_u32(ihdr, apng->width);
   put_u32(ihdr + 4, apng->height);
   ihdr[8] = apng->bit_depth;
   ihdr[9] = apng->color_type;
   ihdr[10] = PNG_COMPRESSION_TYPE_BASE;
   ihdr[11] = PNG_FILTER_TYPE_BASE;
   ihdr[12] = apng->interlace_type;

   put_u32(actl, apng->frame_count_override ?
       apng->declared_frames : apng->num_frames);
   put_u32(actl + 4, apng->num_plays);

   if (!buffer_append(&buffer, signature, sizeof signature) ||
       !put_chunk(&buffer, "IHDR", ihdr, sizeof ihdr))
      goto done;

   if (apng->color_type == PNG_COLOR_TYPE_PALETTE)
   {
      png_byte palette_data[768];
      png_byte trans_data[256];
      png_uint_16 p;

      if (apng->palette_count == 0 || apng->palette_count > 256 ||
          apng->palette == NULL)
         goto done;

      for (p = 0; p < apng->palette_count; ++p)
      {
         png_const_bytep entry = apng->palette + (size_t)p * 4;
         palette_data[(size_t)p * 3] = entry[0];
         palette_data[(size_t)p * 3 + 1] = entry[1];
         palette_data[(size_t)p * 3 + 2] = entry[2];
         trans_data[p] = p < apng->num_trans ? entry[3] : 255;
      }

      if (!put_chunk(&buffer, "PLTE", palette_data,
          (size_t)apng->palette_count * 3) ||
          (apng->num_trans != 0 &&
          !put_chunk(&buffer, "tRNS", trans_data, apng->num_trans)))
         goto done;
   }

   if (apng->fctl_before_actl && apng->num_frames != 0)
   {
      if (!put_fctl(&buffer, sequence++, &apng->frames[0]))
         goto done;

      if (!apng->omit_actl && !put_chunk(&buffer, "acTL", actl, sizeof actl))
         goto done;
   }

   else if (!apng->omit_actl && !apng->actl_after_idat &&
       !put_chunk(&buffer, "acTL", actl, sizeof actl))
      goto done;

   if (apng->duplicate_actl &&
       !put_chunk(&buffer, "acTL", actl, sizeof actl))
      goto done;

   if (apng->default_image_is_frame)
   {
      if (!apng->fctl_before_actl)
      {
         if (!put_fctl(&buffer, sequence++, &apng->frames[0]))
            goto done;
      }

      if (!encode_frame(apng, &apng->frames[0], &compressed,
          &compressed_size) ||
          !put_chunk(&buffer, "IDAT", compressed, compressed_size))
         goto done;

      free(compressed);
      compressed = NULL;

      if (apng->actl_after_idat &&
          !put_chunk(&buffer, "acTL", actl, sizeof actl))
         goto done;

      for (i = 1; i < apng->num_frames; ++i)
      {
         if (!put_fctl(&buffer, sequence++, &apng->frames[i]))
            goto done;

      if (!apng->omit_frame_data)
      {
            png_uint_32 chunks;

            if (!encode_frame(apng, &apng->frames[i], &compressed,
                &compressed_size))
               goto done;

            if (!put_fdat(&buffer, sequence, compressed, compressed_size,
                apng->split_fdat, &chunks))
               goto done;

            sequence += chunks;
            free(compressed);
            compressed = NULL;
         }
      }
   }

   else
   {
      test_frame default_frame;

      memset(&default_frame, 0, sizeof default_frame);
      default_frame.width = apng->width;
      default_frame.height = apng->height;
      default_frame.rgba = apng->default_rgba;

      if (!encode_frame(apng, &default_frame, &compressed, &compressed_size) ||
          !put_chunk(&buffer, "IDAT", compressed, compressed_size))
         goto done;

      free(compressed);
      compressed = NULL;

      if (apng->actl_after_idat &&
          !put_chunk(&buffer, "acTL", actl, sizeof actl))
         goto done;

      for (i = 0; i < apng->num_frames; ++i)
      {
         if (!put_fctl(&buffer, sequence++, &apng->frames[i]))
            goto done;

         if (!apng->omit_frame_data)
         {
            png_uint_32 chunks;

            if (!encode_frame(apng, &apng->frames[i], &compressed,
                &compressed_size))
               goto done;

            if (!put_fdat(&buffer, sequence, compressed, compressed_size,
                apng->split_fdat, &chunks))
               goto done;

            sequence += chunks;
            free(compressed);
            compressed = NULL;
         }
      }
   }

   if (!put_chunk(&buffer, "IEND", NULL, 0))
      goto done;

   *output = buffer.data;
   *output_size = buffer.size;
   buffer.data = NULL;
   result = 1;

done:
   free(compressed);
   buffer_free(&buffer);
   return result;
}

static png_byte
div255(png_uint_32 value)
{
   return (png_byte)((value + 127) / 255);
}

static void
reference_blend(png_bytep destination, png_const_bytep source)
{
   png_uint_32 source_alpha = source[3];
   png_uint_32 destination_alpha = destination[3];
   png_uint_32 output_alpha;
   png_uint_32 channel;

   if (source_alpha == 0)
      return;

   if (source_alpha == 255)
   {
      memcpy(destination, source, 4);
      return;
   }

   output_alpha = source_alpha +
       div255((255 - source_alpha) * destination_alpha);

   for (channel = 0; channel < 3; ++channel)
   {
      png_uint_32 value;

      if (destination_alpha == 255)
         value = div255(source_alpha * source[channel] +
             (255 - source_alpha) * destination[channel]);

      else
         value = (255 * source_alpha * source[channel] +
             (255 - source_alpha) * destination_alpha *
             destination[channel]) / (255 * output_alpha);

      destination[channel] = (png_byte)value;
   }

   destination[3] = (png_byte)output_alpha;
}

static int
reference_init(reference_state *state, const test_apng *apng)
{
   size_t size = (size_t)apng->width * apng->height * 4;

   memset(state, 0, sizeof *state);
   state->canvas = (png_bytep)calloc(size == 0 ? 1 : size, 1);
   state->previous = (png_bytep)malloc(size == 0 ? 1 : size);
   state->save = (png_bytep)malloc(size == 0 ? 1 : size);
   state->scratch = (png_bytep)malloc(size == 0 ? 1 : size);

   return state->canvas != NULL && state->previous != NULL &&
          state->save != NULL && state->scratch != NULL;
}

static void
reference_free(reference_state *state)
{
   free(state->canvas);
   free(state->previous);
   free(state->save);
   free(state->scratch);
   memset(state, 0, sizeof *state);
}

static void
reference_clear_rectangle(reference_state *state, const test_apng *apng,
    const test_frame *frame)
{
   size_t rowbytes = (size_t)apng->width * 4;
   png_uint_32 y;

   for (y = 0; y < frame->height; ++y)
      memset(state->canvas + (size_t)(frame->y_offset + y) * rowbytes +
          (size_t)frame->x_offset * 4, 0, (size_t)frame->width * 4);
}

static void
reference_step(reference_state *state, const test_apng *apng,
    const test_frame *frame)
{
   size_t canvas_rowbytes = (size_t)apng->width * 4;
   size_t frame_rowbytes = (size_t)frame->width * 4;
   size_t canvas_size = (size_t)apng->height * canvas_rowbytes;
   png_uint_32 y;

   if (state->have_last)
   {
      if (state->last.dispose_op == PNG_APNG_DISPOSE_OP_BACKGROUND)
         reference_clear_rectangle(state, apng, &state->last);

      else if (state->last.dispose_op == PNG_APNG_DISPOSE_OP_PREVIOUS)
         memcpy(state->canvas, state->previous, canvas_size);
   }

   if (frame->dispose_op == PNG_APNG_DISPOSE_OP_PREVIOUS)
      memcpy(state->previous, state->canvas, canvas_size);

   for (y = 0; y < frame->height; ++y)
   {
      png_bytep destination = state->canvas +
          (size_t)(frame->y_offset + y) * canvas_rowbytes +
          (size_t)frame->x_offset * 4;
      png_const_bytep source = frame->rgba +
          (size_t)y * frame_rowbytes;

      if (frame->blend_op == PNG_APNG_BLEND_OP_SOURCE)
         memcpy(destination, source, frame_rowbytes);

      else
      {
         png_uint_32 x;

         for (x = 0; x < frame->width; ++x)
            reference_blend(destination + (size_t)x * 4,
                source + (size_t)x * 4);
      }
   }

   state->last = *frame;
   state->have_last = 1;
}

static int
check_metadata(const char *test, png_uint_32 index,
    const png_apng_frame *actual, const test_frame *expected)
{
   png_uint_16 delay_den = expected->delay_den == 0 ? 100 :
       expected->delay_den;
   png_uint_32 delay_ms = (png_uint_32)expected->delay_num * 1000u;
   delay_ms = (delay_ms + delay_den / 2) / delay_den;

   if (actual->frame_index != index ||
       actual->width != expected->width ||
       actual->height != expected->height ||
       actual->x_offset != expected->x_offset ||
       actual->y_offset != expected->y_offset ||
       actual->delay_num != expected->delay_num ||
       actual->delay_den != delay_den ||
       actual->delay_ms != delay_ms ||
       actual->dispose_op != expected->dispose_op ||
       actual->blend_op != expected->blend_op)
   {
      fail(test, "frame metadata mismatch");
      return 0;
   }

   return 1;
}

static int
check_canvas(const char *test, png_uint_32 frame_index,
    png_const_bytep actual, png_const_bytep expected, size_t size)
{
   size_t i;

   for (i = 0; i < size; ++i)
   {
      if (actual[i] != expected[i])
      {
         char message[128];

         snprintf(message, sizeof message,
             "pixel mismatch at frame %lu byte %lu (%u != %u)",
             (unsigned long)frame_index, (unsigned long)i,
             (unsigned int)actual[i], (unsigned int)expected[i]);
         fail(test, message);
         return 0;
      }
   }

   return 1;
}

static int
run_valid_case(const char *test, test_apng *apng)
{
   png_bytep file = NULL;
   size_t file_size = 0;
   png_apngp decoder = NULL;
   png_apng_info info;
   png_apng_frame frame;
   reference_state reference;
   png_bytep actual = NULL;
   size_t canvas_size = (size_t)apng->width * apng->height * 4;
   png_uint_32 i;
   int reference_initialized = 0;
   int result = 0;

   if (!make_apng(apng, &file, &file_size))
   {
      fail(test, "cannot build APNG");
      return 0;
   }

   decoder = png_apng_create_read_memory(file, file_size);
   free(file);

   if (decoder == NULL || !png_apng_get_info(decoder, &info))
   {
      char message[256];
      snprintf(message, sizeof message, "decoder rejected valid APNG: %s",
          decoder == NULL ? "allocation failure" :
          png_apng_get_error(decoder));
      fail(test, message);
      goto done;
   }

   if (info.width != apng->width || info.height != apng->height ||
       info.num_frames != apng->num_frames ||
       info.num_plays != apng->num_plays ||
       info.bit_depth != apng->bit_depth ||
       info.color_type != apng->color_type ||
       info.default_image_is_frame !=
       (png_byte)apng->default_image_is_frame)
   {
      fail(test, "pnapng_info mismatch");
      goto done;
   }

   if (!reference_init(&reference, apng))
   {
      fail(test, "reference allocation failure");
      goto done;
   }
   reference_initialized = 1;

   actual = (png_bytep)malloc(canvas_size == 0 ? 1 : canvas_size);
   if (actual == NULL)
   {
      reference_free(&reference);
      fail(test, "output allocation failure");
      goto done;
   }

   for (i = 0; i < apng->num_frames; ++i)
   {
      reference_step(&reference, apng, &apng->frames[i]);
      if (png_apng_next_frame(decoder, actual, 0, &frame) != 1)
      {
         char message[256];
         snprintf(message, sizeof message, "frame %lu decode failed: %s",
             (unsigned long)i, png_apng_get_error(decoder));
         fail(test, message);
         goto done;
      }

      if (!check_metadata(test, i, &frame, &apng->frames[i]) ||
          !check_canvas(test, i, actual, reference.canvas, canvas_size))
         goto done;

      ++total_frames;
      total_pixels += (unsigned long long)apng->width * apng->height;
      total_bytes += canvas_size;
   }

   if (png_apng_next_frame(decoder, actual, 0, NULL) != 0)
   {
      fail(test, "animation did not report end");
      goto done;
   }

   if (png_apng_get_current_frame(decoder) != apng->num_frames - 1)
   {
      fail(test, "current frame is wrong at end");
      goto done;
   }

   if (!png_apng_rewind(decoder) ||
       png_apng_get_current_frame(decoder) != PNG_APNG_NO_FRAME)
   {
      fail(test, "rewind did not reset the playback cursor");
      goto done;
   }

   result = 1;

done:
   if (reference_initialized)
      reference_free(&reference);
   free(actual);
   png_apng_destroy(&decoder);
   return result;
}

static void
fill_pixels(png_bytep rgba, png_uint_32 width, png_uint_32 height,
    png_uint_32 seed)
{
   png_uint_32 x;
   png_uint_32 y;

   for (y = 0; y < height; ++y)
   {
      for (x = 0; x < width; ++x)
      {
         png_bytep pixel = rgba + ((size_t)y * width + x) * 4;
         png_uint_32 value = seed + x * 37 + y * 73;

         pixel[0] = (png_byte)(value * 17);
         pixel[1] = (png_byte)(value * 29 + 11);
         pixel[2] = (png_byte)(value * 43 + 23);
         pixel[3] = (png_byte)((value * 13) & 255);
      }
   }
}

static void
init_frame(test_apng *apng, test_frame *frame, png_uint_32 width,
    png_uint_32 height, png_uint_32 x_offset, png_uint_32 y_offset,
    png_uint_16 delay_num, png_uint_16 delay_den, png_byte dispose_op,
    png_byte blend_op, png_uint_32 seed)
{
   (void)apng;
   memset(frame, 0, sizeof *frame);
   frame->width = width;
   frame->height = height;
   frame->x_offset = x_offset;
   frame->y_offset = y_offset;
   frame->delay_num = delay_num;
   frame->delay_den = delay_den;
   frame->dispose_op = dispose_op;
   frame->blend_op = blend_op;
   frame->rgba = (png_bytep)malloc((size_t)width * height * 4);
   if (frame->rgba != NULL)
      fill_pixels(frame->rgba, width, height, seed);
   else
      fail("setup", "out of memory");
}

static void
free_apng(test_apng *apng)
{
   png_uint_32 i;

   if (apng->frames != NULL)
   {
      for (i = 0; i < apng->num_frames; ++i)
         free(apng->frames[i].rgba);
   }

   free(apng->frames);
   free(apng->default_rgba);
   free(apng->palette);
   memset(apng, 0, sizeof *apng);
}

static void
add_frame(test_apng *apng, png_uint_32 width, png_uint_32 height,
    png_uint_32 x_offset, png_uint_32 y_offset, png_uint_16 delay_num,
    png_uint_16 delay_den, png_byte dispose_op, png_byte blend_op,
    png_uint_32 seed)
{
   if (apng->frames == NULL)
   {
      apng->frames = (test_frame *)calloc(32, sizeof *apng->frames);
      if (apng->frames == NULL)
      {
         fail("setup", "out of memory");
         return;
      }
   }

   if (apng->num_frames < 32)
   {
      init_frame(apng, &apng->frames[apng->num_frames], width, height,
          x_offset, y_offset, delay_num, delay_den, dispose_op, blend_op,
          seed);
      ++apng->num_frames;
   }
}

static void
test_dispose_blend_matrix(void)
{
   static const png_byte dispose_ops[3] =
       { PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_DISPOSE_OP_BACKGROUND,
         PNG_APNG_DISPOSE_OP_PREVIOUS };
   static const png_byte blend_ops[2] =
       { PNG_APNG_BLEND_OP_SOURCE, PNG_APNG_BLEND_OP_OVER };
   png_uint_32 dispose_index;
   png_uint_32 blend_index;

   for (dispose_index = 0; dispose_index < 3; ++dispose_index)
   {
      for (blend_index = 0; blend_index < 2; ++blend_index)
      {
         char name[80];
         test_apng apng;
         png_uint_32 i;

         memset(&apng, 0, sizeof apng);
         apng.width = 11;
         apng.height = 9;
         apng.bit_depth = 8;
         apng.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
         apng.num_plays = 0;
         apng.default_image_is_frame =
             (dispose_index + blend_index) % 2 == 0;
         apng.split_fdat = (dispose_index == blend_index);
         apng.default_rgba = (png_bytep)malloc(
             (size_t)apng.width * apng.height * 4);
         if (apng.default_rgba == NULL)
         {
            fail("matrix", "out of memory");
            return;
         }
         fill_pixels(apng.default_rgba, apng.width, apng.height, 999);

         add_frame(&apng, 11, 9, 0, 0, 1, 100,
             dispose_ops[dispose_index], blend_ops[blend_index], 1);
         add_frame(&apng, 5, 4, 2, 1, 0, 100,
             dispose_ops[(dispose_index + 1) % 3],
             blend_ops[(blend_index + 1) % 2], 2);
         add_frame(&apng, 4, 5, 7, 4, 1, 0,
             dispose_ops[(dispose_index + 2) % 3],
             blend_ops[blend_index], 3);
         add_frame(&apng, 1, 1, 10, 8, 65535, 65535,
             dispose_ops[dispose_index],
             blend_ops[(blend_index + 1) % 2], 4);

         for (i = 0; i < apng.num_frames; ++i)
            apng.frames[i].blend_op = blend_ops[blend_index];

         snprintf(name, sizeof name, "dispose%lu-blend%lu",
             (unsigned long)dispose_index, (unsigned long)blend_index);
         (void)run_valid_case(name, &apng);
         free_apng(&apng);
      }
   }
}

static void
test_color_modes(void)
{
   static const struct
   {
      png_byte depth;
      png_byte color_type;
      const char *name;
   } modes[] =
   {
      { 8, PNG_COLOR_TYPE_GRAY, "gray8" },
      { 8, PNG_COLOR_TYPE_GRAY_ALPHA, "ga8" },
      { 8, PNG_COLOR_TYPE_RGB, "rgb8" },
      { 8, PNG_COLOR_TYPE_RGB_ALPHA, "rgba8" },
      { 16, PNG_COLOR_TYPE_GRAY, "gray16" },
      { 16, PNG_COLOR_TYPE_GRAY_ALPHA, "ga16" },
      { 16, PNG_COLOR_TYPE_RGB, "rgb16" },
      { 16, PNG_COLOR_TYPE_RGB_ALPHA, "rgba16" },
      { 8, PNG_COLOR_TYPE_PALETTE, "palette8" }
   };
   size_t mode_index;

   for (mode_index = 0; mode_index < sizeof modes / sizeof modes[0];
       ++mode_index)
   {
      test_apng apng;
      png_uint_32 i;
      unsigned long long before = total_frames;

      memset(&apng, 0, sizeof apng);
      apng.width = 6;
      apng.height = 5;
      apng.bit_depth = modes[mode_index].depth;
      apng.color_type = modes[mode_index].color_type;
      apng.num_plays = 1;
      apng.default_image_is_frame = 0;
      apng.default_rgba = (png_bytep)malloc(
          (size_t)apng.width * apng.height * 4);
      if (apng.default_rgba == NULL)
      {
         fail("colors", "out of memory");
         return;
      }
      fill_pixels(apng.default_rgba, apng.width, apng.height, 77);

      if (apng.color_type == PNG_COLOR_TYPE_PALETTE)
      {
         png_uint_16 p;

         apng.palette_count = 8;
         apng.num_trans = 8;
         apng.palette = (png_bytep)malloc((size_t)apng.palette_count * 4);
         if (apng.palette == NULL)
         {
            fail("colors", "out of memory");
            free_apng(&apng);
            return;
         }

         for (p = 0; p < apng.palette_count; ++p)
         {
            png_bytep entry = apng.palette + (size_t)p * 4;
            entry[0] = (png_byte)(p * 31 + 1);
            entry[1] = (png_byte)(p * 47 + 3);
            entry[2] = (png_byte)(p * 61 + 5);
            entry[3] = (png_byte)(p * 23 + 7);
         }

         {
            size_t pixel;
            size_t pixels = (size_t)apng.width * apng.height;

            for (pixel = 0; pixel < pixels; ++pixel)
               memcpy(apng.default_rgba + pixel * 4, apng.palette, 4);
         }
      }

      add_frame(&apng, 6, 5, 0, 0, 10, 100,
          PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_BLEND_OP_SOURCE, 11);
      add_frame(&apng, 3, 2, 2, 1, 1, 3,
          PNG_APNG_DISPOSE_OP_PREVIOUS, PNG_APNG_BLEND_OP_OVER, 12);
      add_frame(&apng, 2, 3, 0, 2, 2, 3,
          PNG_APNG_DISPOSE_OP_BACKGROUND, PNG_APNG_BLEND_OP_OVER, 13);

      for (i = 0; i < apng.num_frames; ++i)
      {
         if (apng.color_type == PNG_COLOR_TYPE_PALETTE)
         {
            png_uint_32 x;
            png_uint_32 y;
            png_bytep rgba = apng.frames[i].rgba;

            for (y = 0; y < apng.frames[i].height; ++y)
            {
               for (x = 0; x < apng.frames[i].width; ++x)
               {
                  png_uint_16 p = (png_uint_16)((x + y * 3 + i) &
                      (apng.palette_count - 1));
                  memcpy(rgba + ((size_t)y * apng.frames[i].width + x) * 4,
                      apng.palette + (size_t)p * 4, 4);
               }
            }
         }

         else if (apng.color_type == PNG_COLOR_TYPE_GRAY ||
             apng.color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
         {
            png_bytep rgba = apng.frames[i].rgba;
            size_t pixel;
            size_t pixels = (size_t)apng.frames[i].width *
                apng.frames[i].height;

            for (pixel = 0; pixel < pixels; ++pixel)
            {
               rgba[pixel * 4 + 1] = rgba[pixel * 4];
               rgba[pixel * 4 + 2] = rgba[pixel * 4];

               if (apng.color_type == PNG_COLOR_TYPE_GRAY)
                  rgba[pixel * 4 + 3] = 255;
            }
         }

         else if (apng.color_type == PNG_COLOR_TYPE_RGB)
         {
            png_bytep rgba = apng.frames[i].rgba;
            size_t pixel;
            size_t pixels = (size_t)apng.frames[i].width *
                apng.frames[i].height;

            for (pixel = 0; pixel < pixels; ++pixel)
               rgba[pixel * 4 + 3] = 255;
         }
      }

      (void)run_valid_case(modes[mode_index].name, &apng);
      printf("group %s: frames=%llu\n", modes[mode_index].name,
          total_frames - before);
      free_apng(&apng);
   }
}

static void
test_interlaced(void)
{
   test_apng apng;

   memset(&apng, 0, sizeof apng);
   apng.width = 9;
   apng.height = 7;
   apng.bit_depth = 8;
   apng.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
   apng.interlace_type = PNG_INTERLACE_ADAM7;
   apng.default_image_is_frame = 1;
   add_frame(&apng, 9, 7, 0, 0, 1, 100,
       PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_BLEND_OP_SOURCE, 1);
   add_frame(&apng, 5, 4, 2, 1, 1, 100,
       PNG_APNG_DISPOSE_OP_PREVIOUS, PNG_APNG_BLEND_OP_OVER, 2);
   add_frame(&apng, 3, 3, 6, 4, 1, 100,
       PNG_APNG_DISPOSE_OP_BACKGROUND, PNG_APNG_BLEND_OP_OVER, 3);
   (void)run_valid_case("interlaced", &apng);
   free_apng(&apng);
}

static void
test_edges_and_delays(void)
{
   test_apng apng;

   memset(&apng, 0, sizeof apng);
   apng.width = 1;
   apng.height = 1;
   apng.bit_depth = 8;
   apng.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
   apng.default_image_is_frame = 1;
   add_frame(&apng, 1, 1, 0, 0, 0, 0, PNG_APNG_DISPOSE_OP_NONE,
       PNG_APNG_BLEND_OP_SOURCE, 1);
   add_frame(&apng, 1, 1, 0, 0, 1, 1000,
       PNG_APNG_DISPOSE_OP_PREVIOUS, PNG_APNG_BLEND_OP_OVER, 2);
   add_frame(&apng, 1, 1, 0, 0, 1, 3,
       PNG_APNG_DISPOSE_OP_BACKGROUND, PNG_APNG_BLEND_OP_SOURCE, 3);
   (void)run_valid_case("1x1", &apng);
   free_apng(&apng);

   memset(&apng, 0, sizeof apng);
   apng.width = 6;
   apng.height = 1;
   apng.bit_depth = 8;
   apng.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
   apng.default_image_is_frame = 1;
   add_frame(&apng, 6, 1, 0, 0, 1, 100,
       PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_BLEND_OP_SOURCE, 1);
   add_frame(&apng, 6, 1, 0, 0, 1, 100,
       PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_BLEND_OP_OVER, 2);
   add_frame(&apng, 6, 1, 0, 0, 1, 100,
       PNG_APNG_DISPOSE_OP_PREVIOUS, PNG_APNG_BLEND_OP_OVER, 3);
   {
      static const png_byte alphas[6] = { 0, 1, 127, 128, 254, 255 };
      png_uint_32 x;

      for (x = 0; x < 6; ++x)
      {
         png_bytep first = apng.frames[0].rgba + (size_t)x * 4;
         png_bytep second = apng.frames[1].rgba + (size_t)x * 4;
         png_bytep third = apng.frames[2].rgba + (size_t)x * 4;

         first[0] = (png_byte)(10 + x * 20);
         first[1] = (png_byte)(30 + x * 10);
         first[2] = (png_byte)(50 + x * 5);
         first[3] = alphas[x];

         second[0] = (png_byte)(250 - x * 10);
         second[1] = (png_byte)(200 - x * 20);
         second[2] = (png_byte)(150 - x * 5);
         second[3] = alphas[5 - x];

         third[0] = (png_byte)(x * 40);
         third[1] = (png_byte)(255 - x * 20);
         third[2] = (png_byte)(x * 15 + 3);
         third[3] = alphas[x];
      }
   }
   (void)run_valid_case("blend-alpha-edges", &apng);
   free_apng(&apng);

   memset(&apng, 0, sizeof apng);
   apng.width = 8;
   apng.height = 6;
   apng.bit_depth = 8;
   apng.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
   apng.default_image_is_frame = 0;
   apng.default_rgba = (png_bytep)malloc((size_t)apng.width * apng.height * 4);
   if (apng.default_rgba == NULL)
   {
      fail("edges", "out of memory");
      return;
   }
   fill_pixels(apng.default_rgba, apng.width, apng.height, 123);
   add_frame(&apng, 1, 1, 0, 0, 1, 0,
       PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_BLEND_OP_OVER, 1);
   add_frame(&apng, 1, 1, 7, 0, 2, 3,
       PNG_APNG_DISPOSE_OP_BACKGROUND, PNG_APNG_BLEND_OP_SOURCE, 2);
   add_frame(&apng, 1, 1, 0, 5, 1, 100,
       PNG_APNG_DISPOSE_OP_PREVIOUS, PNG_APNG_BLEND_OP_OVER, 3);
   add_frame(&apng, 1, 1, 7, 5, 65535, 65535,
       PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_BLEND_OP_SOURCE, 4);
   add_frame(&apng, 8, 6, 0, 0, 3, 7,
       PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_BLEND_OP_OVER, 5);
   (void)run_valid_case("corners-delays", &apng);
   free_apng(&apng);
}

static void
test_seek_and_rewind(void)
{
   test_apng apng;
   png_bytep file = NULL;
   size_t file_size = 0;
   png_apngp decoder;
   png_apng_info info;
   reference_state reference;
   png_bytep actual;
   size_t canvas_size;
   png_uint_32 i;

   memset(&apng, 0, sizeof apng);
   apng.width = 7;
   apng.height = 6;
   apng.bit_depth = 8;
   apng.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
   apng.default_image_is_frame = 1;
   add_frame(&apng, 7, 6, 0, 0, 1, 100,
       PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_BLEND_OP_SOURCE, 1);
   add_frame(&apng, 4, 3, 1, 1, 1, 100,
       PNG_APNG_DISPOSE_OP_PREVIOUS, PNG_APNG_BLEND_OP_OVER, 2);
   add_frame(&apng, 3, 4, 3, 2, 1, 100,
       PNG_APNG_DISPOSE_OP_BACKGROUND, PNG_APNG_BLEND_OP_SOURCE, 3);
   add_frame(&apng, 2, 2, 5, 4, 1, 100,
       PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_BLEND_OP_OVER, 4);

   if (!make_apng(&apng, &file, &file_size))
   {
      fail("seek", "cannot build APNG");
      free_apng(&apng);
      return;
   }

   decoder = png_apng_create_read_memory(file, file_size);
   free(file);
   if (decoder == NULL || !png_apng_get_info(decoder, &info))
   {
      fail("seek", "decoder rejected valid APNG");
      png_apng_destroy(&decoder);
      free_apng(&apng);
      return;
   }

   if (info.width != apng.width || info.height != apng.height ||
       info.num_frames != apng.num_frames)
   {
      fail("seek", "decoder info mismatch");
      png_apng_destroy(&decoder);
      free_apng(&apng);
      return;
   }

   if (!reference_init(&reference, &apng))
   {
      fail("seek", "reference allocation failure");
      png_apng_destroy(&decoder);
      free_apng(&apng);
      return;
   }

   canvas_size = (size_t)apng.width * apng.height * 4;
   actual = (png_bytep)malloc(canvas_size);
   if (actual == NULL)
   {
      fail("seek", "output allocation failure");
      reference_free(&reference);
      png_apng_destroy(&decoder);
      free_apng(&apng);
      return;
   }

   for (i = 0; i < apng.num_frames * 2 && failures == 0; ++i)
   {
      png_uint_32 target = i < apng.num_frames ? i :
          apng.num_frames - 1 - (i - apng.num_frames);
      png_uint_32 j;
      png_apng_frame frame;

      reference.have_last = 0;
      memset(reference.canvas, 0, canvas_size);

      for (j = 0; j <= target; ++j)
         reference_step(&reference, &apng, &apng.frames[j]);

      if (!png_apng_seek_frame(decoder, target) ||
          png_apng_get_current_frame(decoder) != target)
      {
         fail("seek", "seek to frame failed");
         break;
      }

      if (target + 1 < apng.num_frames)
      {
         reference_step(&reference, &apng, &apng.frames[target + 1]);
         if (png_apng_next_frame(decoder, actual, 0, &frame) != 1 ||
             !check_metadata("seek", target + 1, &frame,
             &apng.frames[target + 1]) ||
             !check_canvas("seek", target + 1, actual,
             reference.canvas, canvas_size))
            break;
      }

      else if (png_apng_next_frame(decoder, actual, 0, &frame) != 0)
      {
         fail("seek", "last frame did not report end");
         break;
      }
   }

   free(actual);
   reference_free(&reference);
   png_apng_destroy(&decoder);
   free_apng(&apng);
}

static void
test_stress_10000(void)
{
   test_apng apng;
   png_bytep file = NULL;
   size_t file_size = 0;
   png_apngp decoder;
   png_apng_info info;
   reference_state reference;
   png_bytep actual;
   size_t canvas_size;
   unsigned int produced = 0;
   unsigned int cycle;
   int ok = 1;

   memset(&apng, 0, sizeof apng);
   apng.width = 16;
   apng.height = 16;
   apng.bit_depth = 8;
   apng.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
   apng.default_image_is_frame = 0;
   apng.default_rgba = (png_bytep)malloc((size_t)apng.width * apng.height * 4);
   if (apng.default_rgba == NULL)
   {
      fail("stress", "out of memory");
      return;
   }
   fill_pixels(apng.default_rgba, apng.width, apng.height, 500);

   add_frame(&apng, 16, 16, 0, 0, 1, 60,
       PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_BLEND_OP_SOURCE, 1);
   add_frame(&apng, 7, 5, 3, 2, 1, 60,
       PNG_APNG_DISPOSE_OP_PREVIOUS, PNG_APNG_BLEND_OP_OVER, 2);
   add_frame(&apng, 6, 6, 9, 8, 1, 60,
       PNG_APNG_DISPOSE_OP_BACKGROUND, PNG_APNG_BLEND_OP_OVER, 3);
   add_frame(&apng, 3, 3, 1, 12, 1, 60,
       PNG_APNG_DISPOSE_OP_NONE, PNG_APNG_BLEND_OP_SOURCE, 4);

   if (!make_apng(&apng, &file, &file_size))
   {
      fail("stress", "cannot build APNG");
      free_apng(&apng);
      return;
   }

   decoder = png_apng_create_read_memory(file, file_size);
   free(file);
   if (decoder == NULL || !png_apng_get_info(decoder, &info))
   {
      fail("stress", "decoder rejected valid APNG");
      png_apng_destroy(&decoder);
      free_apng(&apng);
      return;
   }

   if (info.width != apng.width || info.height != apng.height ||
       info.num_frames != apng.num_frames)
   {
      fail("stress", "decoder info mismatch");
      png_apng_destroy(&decoder);
      free_apng(&apng);
      return;
   }

   if (!reference_init(&reference, &apng))
   {
      fail("stress", "reference allocation failure");
      png_apng_destroy(&decoder);
      free_apng(&apng);
      return;
   }

   canvas_size = (size_t)apng.width * apng.height * 4;
   actual = (png_bytep)malloc(canvas_size);
   if (actual == NULL)
   {
      fail("stress", "output allocation failure");
      reference_free(&reference);
      png_apng_destroy(&decoder);
      free_apng(&apng);
      return;
   }

   for (cycle = 0; cycle < 2500 && ok; ++cycle)
   {
      png_uint_32 i;

      reference.have_last = 0;
      memset(reference.canvas, 0, canvas_size);

      if (!png_apng_rewind(decoder))
      {
         fail("stress", "rewind failed");
         ok = 0;
         break;
      }

      for (i = 0; i < 4; ++i)
      {
         png_apng_frame frame;

         reference_step(&reference, &apng, &apng.frames[i]);
         if (png_apng_next_frame(decoder, actual, 0, &frame) != 1)
         {
            fail("stress", "frame decode failed");
            ok = 0;
            break;
         }

         if (!check_metadata("stress", i, &frame, &apng.frames[i]) ||
             !check_canvas("stress", i, actual, reference.canvas, canvas_size))
         {
            ok = 0;
            break;
         }

         ++produced;
         ++total_frames;
         total_pixels += (unsigned long long)apng.width * apng.height;
         total_bytes += canvas_size;
      }
   }

   printf("stress: produced=%u cycles=%u mismatches=%d\n", produced, cycle,
       ok ? 0 : 1);

   free(actual);
   reference_free(&reference);
   png_apng_destroy(&decoder);
   free_apng(&apng);
}

static int
expect_invalid(const char *test, test_apng *apng)
{
   png_bytep file = NULL;
   size_t file_size = 0;
   png_apngp decoder;
   int result = 0;

   if (!make_apng(apng, &file, &file_size))
   {
      fail(test, "cannot build invalid APNG");
      return 0;
   }

   decoder = png_apng_create_read_memory(file, file_size);
   free(file);
   if (decoder == NULL)
   {
      fail(test, "decoder allocation returned NULL");
      return 0;
   }

   if (png_apng_get_info(decoder, NULL))
   {
      fail(test, "decoder accepted invalid APNG");
      result = 0;
   }

   else if (png_apng_get_error(decoder)[0] == 0)
   {
      fail(test, "decoder did not provide an error message");
      result = 0;
   }

   else
   {
      result = 1;
      ++total_invalid_cases;
   }

   png_apng_destroy(&decoder);
   return result;
}

static int
find_chunk(png_bytep data, size_t size, const char *type,
    png_uint_32 occurrence, size_t *offset)
{
   size_t position = 8;
   png_uint_32 count = 0;

   while (position + 12 <= size)
   {
      png_uint_32 length = get_u32(data + position);

      if (memcmp(data + position + 4, type, 4) == 0)
      {
         if (count++ == occurrence)
         {
            *offset = position;
            return 1;
         }
      }

      if ((size_t)length > size - position - 12)
         break;

      position += 12 + (size_t)length;
   }

   return 0;
}

static int
run_mutated_invalid(const char *test, test_apng *apng,
    void (*mutate)(png_bytep, size_t))
{
   png_bytep file = NULL;
   size_t file_size = 0;
   png_apngp decoder;
   int result = 0;

   if (!make_apng(apng, &file, &file_size))
   {
      fail(test, "cannot build APNG");
      return 0;
   }

   mutate(file, file_size);
   decoder = png_apng_create_read_memory(file, file_size);
   free(file);

   if (decoder == NULL)
   {
      fail(test, "decoder allocation returned NULL");
      return 0;
   }

   if (png_apng_get_info(decoder, NULL))
   {
      fail(test, "decoder accepted mutated APNG");
      result = 0;
   }

   else
   {
      result = 1;
      ++total_invalid_cases;
   }

   png_apng_destroy(&decoder);
   return result;
}

static void
mutate_sequence_gap(png_bytep data, size_t size)
{
   size_t offset;

   if (find_chunk(data, size, "fdAT", 0, &offset))
      put_u32(data + offset + 8, 99);
}

static void
mutate_frame_rectangle(png_bytep data, size_t size)
{
   size_t offset;

   if (find_chunk(data, size, "fcTL", 0, &offset))
      put_u32(data + offset + 8 + 4, 0x7ffffff0u);
}

static void
mutate_zero_dimensions(png_bytep data, size_t size)
{
   size_t offset;

   if (find_chunk(data, size, "fcTL", 0, &offset))
      put_u32(data + offset + 8 + 4, 0);
}

static void
mutate_dispose_op(png_bytep data, size_t size)
{
   size_t offset;

   if (find_chunk(data, size, "fcTL", 0, &offset))
      data[offset + 8 + 24] = 3;
}

static void
mutate_blend_op(png_bytep data, size_t size)
{
   size_t offset;

   if (find_chunk(data, size, "fcTL", 0, &offset))
      data[offset + 8 + 25] = 2;
}

static void
mutate_bad_crc(png_bytep data, size_t size)
{
   size_t offset;

   if (find_chunk(data, size, "acTL", 0, &offset))
   {
      png_uint_32 length = get_u32(data + offset);
      data[offset + 8 + length] ^= 0xff;
   }
}

static void
mutate_num_frames_low(png_bytep data, size_t size)
{
   size_t offset;

   if (find_chunk(data, size, "acTL", 0, &offset))
      put_u32(data + offset + 8, 1);
}

static void
mutate_num_frames_high(png_bytep data, size_t size)
{
   size_t offset;

   if (find_chunk(data, size, "acTL", 0, &offset))
      put_u32(data + offset + 8, 99);
}

static void
mutate_num_frames_zero(png_bytep data, size_t size)
{
   size_t offset;

   if (find_chunk(data, size, "acTL", 0, &offset))
      put_u32(data + offset + 8, 0);
}

static void
test_invalid_cases(void)
{
   test_apng apng;

   memset(&apng, 0, sizeof apng);
   apng.width = 4;
   apng.height = 3;
   apng.bit_depth = 8;
   apng.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
   apng.default_image_is_frame = 0;
   apng.default_rgba = (png_bytep)malloc((size_t)apng.width * apng.height * 4);
   if (apng.default_rgba == NULL)
   {
      fail("invalid", "out of memory");
      return;
   }
   fill_pixels(apng.default_rgba, apng.width, apng.height, 1);
   add_frame(&apng, 2, 2, 1, 1, 1, 100, PNG_APNG_DISPOSE_OP_NONE,
       PNG_APNG_BLEND_OP_OVER, 2);
   add_frame(&apng, 1, 1, 0, 0, 1, 100, PNG_APNG_DISPOSE_OP_NONE,
       PNG_APNG_BLEND_OP_SOURCE, 3);

   (void)run_mutated_invalid("sequence-gap", &apng, mutate_sequence_gap);
   (void)run_mutated_invalid("frame-rectangle", &apng,
       mutate_frame_rectangle);
   (void)run_mutated_invalid("zero-dimensions", &apng, mutate_zero_dimensions);
   (void)run_mutated_invalid("dispose-op", &apng, mutate_dispose_op);
   (void)run_mutated_invalid("blend-op", &apng, mutate_blend_op);
   (void)run_mutated_invalid("bad-crc", &apng, mutate_bad_crc);
   (void)run_mutated_invalid("num-frames-low", &apng, mutate_num_frames_low);
   (void)run_mutated_invalid("num-frames-high", &apng, mutate_num_frames_high);
   (void)run_mutated_invalid("num-frames-zero", &apng, mutate_num_frames_zero);

   apng.omit_actl = 1;
   (void)expect_invalid("missing-actl", &apng);
   apng.omit_actl = 0;

   apng.actl_after_idat = 1;
   (void)expect_invalid("actl-after-idat", &apng);
   apng.actl_after_idat = 0;

   apng.duplicate_actl = 1;
   (void)expect_invalid("duplicate-actl", &apng);
   apng.duplicate_actl = 0;

   apng.fctl_before_actl = 1;
   apng.default_image_is_frame = 1;
   (void)expect_invalid("fctl-before-actl", &apng);
   apng.fctl_before_actl = 0;
   apng.default_image_is_frame = 0;

   apng.omit_frame_data = 1;
   (void)expect_invalid("missing-fdat", &apng);
   apng.omit_frame_data = 0;

   free_apng(&apng);

   memset(&apng, 0, sizeof apng);
   apng.width = 4;
   apng.height = 3;
   apng.bit_depth = 8;
   apng.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
   apng.default_image_is_frame = 1;
   apng.default_rgba = (png_bytep)malloc((size_t)apng.width * apng.height * 4);
   if (apng.default_rgba != NULL)
   {
      fill_pixels(apng.default_rgba, apng.width, apng.height, 1);
      add_frame(&apng, 2, 2, 1, 1, 1, 100, PNG_APNG_DISPOSE_OP_NONE,
          PNG_APNG_BLEND_OP_OVER, 2);
      (void)expect_invalid("default-frame-not-canvas", &apng);
   }
   free_apng(&apng);
}

int
main(void)
{
   unsigned long long before;

   before = total_frames;
   test_dispose_blend_matrix();
   printf("group dispose_blend: frames=%llu\n", total_frames - before);

   before = total_frames;
   test_color_modes();
   printf("group color_modes: frames=%llu\n", total_frames - before);

   before = total_frames;
   test_edges_and_delays();
   printf("group edges_delays: frames=%llu\n", total_frames - before);

   before = total_frames;
   test_interlaced();
   printf("group interlaced: frames=%llu\n", total_frames - before);

   test_seek_and_rewind();
   test_invalid_cases();
   test_stress_10000();

   printf("consistency: frames=%llu pixels=%llu rgba_bytes=%llu "
       "invalid_cases=%u mismatches=%d\n", total_frames, total_pixels,
       total_bytes, total_invalid_cases, failures);

   if (failures != 0)
   {
      fprintf(stderr, "APNG tests failed: %d\n", failures);
      return 1;
   }

   printf("APNG tests passed\n");
   return 0;
}
