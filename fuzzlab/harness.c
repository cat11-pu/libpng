/*
 * Strict, bounds-checked PNG decode driver for malformed-input testing.
 *
 * Exit status:
 *   0 - valid PNG decoded
 *   1 - malformed PNG rejected (machine-readable REJECT line on stderr)
 *   2 - command line, file I/O or allocation-limit error
 *
 * The preflight parser never allocates based on chunk or image sizes.  It
 * rejects broken framing, illegal chunk lengths and oversized dimensions
 * before libpng is entered.  libpng still validates the compressed stream,
 * CRCs, filters and chunk semantics.
 */

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"
#include <zlib.h>

#define MAX_DIM             65535U
#define MAX_PIXELS          (64U * 1024U * 1024U)
#define MAX_INPUT_BYTES     (64U * 1024U * 1024U)
#define MAX_SINGLE_ALLOC    (16U * 1024U * 1024U)
#define MAX_TOTAL_ALLOC     (256U * 1024U * 1024U)
#define MAX_CHUNK_ALLOC     (8U * 1024U * 1024U)
#define MAX_ROWBYTES        (1024U * 1024U)
#define MAX_CHUNKS          4096

enum error_code
{
   E_OK = 0,
   E_SIGNATURE = 1,
   E_TRUNCATED = 2,
   E_CHUNK_HEADER = 3,
   E_CHUNK_LENGTH = 4,
   E_CHUNK_ORDER = 5,
   E_DIMENSIONS = 6,
   E_ALLOC = 7,
   E_LIBPNG = 8,
   E_CRC = 9,
   E_TRAILING = 10
};

static const char *const error_names[] =
{
   "ok", "signature", "truncated", "chunk_header", "chunk_length",
   "chunk_order", "dimensions", "alloc", "libpng", "crc", "trailing"
};

typedef struct
{
   const unsigned char *data;
   size_t size;
   size_t pos;
} Reader;

typedef struct
{
   char type[5];
   uint32_t length;
   size_t header_off;
   size_t data_off;
   size_t crc_off;
} Chunk;

typedef struct
{
   size_t size;
   size_t pad;
} AllocHeader;

static Reader reader;
static Chunk chunks[MAX_CHUNKS];
static int chunk_count;
static int error_code;
static size_t error_offset;
static char error_message[192];
static size_t alloc_total;
static size_t alloc_peak;

static uint32_t
get_u32(const unsigned char *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void
set_error(int code, size_t offset, const char *message)
{
   if (error_code == E_OK)
   {
      error_code = code;
      error_offset = offset;
      if (message != error_message)
         snprintf(error_message, sizeof error_message, "%s",
             message != NULL ? message : "error");
   }
}

static int
is_letter(unsigned int c)
{
   return (c >= 65U && c <= 90U) || (c >= 97U && c <= 122U);
}

static int
valid_color_depth(unsigned int color_type, unsigned int bit_depth)
{
   switch (color_type)
   {
      case 0:
         return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
             bit_depth == 8 || bit_depth == 16;

      case 3:
         return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
             bit_depth == 8;

      case 2:
      case 4:
      case 6:
         return bit_depth == 8 || bit_depth == 16;

      default:
         return 0;
   }
}

/*
 * Validate the chunk table and all size-critical IHDR fields.  This function
 * only reads the input buffer; it does not allocate from untrusted lengths.
 */
static int
preflight(void)
{
   static const unsigned char signature[8] =
       {137, 80, 78, 71, 13, 10, 26, 10};
   size_t p;
   unsigned int have_ihdr = 0;
   unsigned int have_plte = 0;
   unsigned int have_idat = 0;
   unsigned int have_iend = 0;
   unsigned int idat_closed = 0;
   unsigned int width = 0;
   unsigned int height = 0;
   unsigned int bit_depth = 0;
   unsigned int color_type = 0;

   reader.pos = 0;
   chunk_count = 0;

   if (reader.size < sizeof signature)
   {
      set_error(E_TRUNCATED, reader.size, "truncated PNG signature");
      return 0;
   }

   if (memcmp(reader.data, signature, sizeof signature) != 0)
   {
      size_t i;

      for (i = 0; i < sizeof signature; ++i)
         if (reader.data[i] != signature[i])
            break;

      set_error(E_SIGNATURE, i, "invalid PNG signature");
      return 0;
   }

   p = sizeof signature;
   while (p < reader.size)
   {
      uint32_t length;
      size_t available;
      Chunk *chunk;

      if (reader.size - p < 8)
      {
         set_error(E_TRUNCATED, p, "truncated chunk header");
         return 0;
      }

      length = get_u32(reader.data + p);
      if (length > PNG_UINT_31_MAX)
      {
         set_error(E_CHUNK_LENGTH, p, "chunk length exceeds PNG limit");
         return 0;
      }

      if (chunk_count >= MAX_CHUNKS)
      {
         set_error(E_CHUNK_LENGTH, p, "too many chunks");
         return 0;
      }

      if (!is_letter(reader.data[p + 4]) ||
          !is_letter(reader.data[p + 5]) ||
          !is_letter(reader.data[p + 6]) ||
          !is_letter(reader.data[p + 7]))
      {
         set_error(E_CHUNK_HEADER, p + 4, "invalid chunk type");
         return 0;
      }

      available = reader.size - p - 8;
      if ((size_t)length > available || available - (size_t)length < 4)
      {
         set_error(E_TRUNCATED, reader.size,
             "chunk payload extends beyond end of file");
         return 0;
      }

      chunk = &chunks[chunk_count++];
      memcpy(chunk->type, reader.data + p + 4, 4);
      chunk->type[4] = 0;
      chunk->length = length;
      chunk->header_off = p;
      chunk->data_off = p + 8;
      chunk->crc_off = p + 8 + length;

      {
         uLong crc = crc32(0L, Z_NULL, 0);
         crc = crc32(crc, reader.data + p + 4, 4);
         crc = crc32(crc, reader.data + chunk->data_off, length);
         if (crc != get_u32(reader.data + chunk->crc_off))
         {
            set_error(E_CRC, chunk->crc_off, "chunk CRC mismatch");
            return 0;
         }
      }

      if (chunk_count == 1 && memcmp(chunk->type, "IHDR", 4) != 0)
      {
         set_error(E_CHUNK_ORDER, p + 4, "IHDR is not the first chunk");
         return 0;
      }

      if (memcmp(chunk->type, "IHDR", 4) == 0)
      {
         const unsigned char *ihdr = reader.data + chunk->data_off;

         if (have_ihdr)
         {
            set_error(E_CHUNK_ORDER, p + 4, "duplicate IHDR");
            return 0;
         }

         if (length != 13)
         {
            set_error(E_CHUNK_LENGTH, p, "IHDR length is not 13");
            return 0;
         }

         width = get_u32(ihdr);
         height = get_u32(ihdr + 4);
         bit_depth = ihdr[8];
         color_type = ihdr[9];

         if (width == 0)
         {
            set_error(E_DIMENSIONS, chunk->data_off, "image width is zero");
            return 0;
         }

         if (height == 0)
         {
            set_error(E_DIMENSIONS, chunk->data_off + 4,
                "image height is zero");
            return 0;
         }

         if (width > MAX_DIM)
         {
            set_error(E_DIMENSIONS, chunk->data_off,
                "image width exceeds decode limit");
            return 0;
         }

         if (height > MAX_DIM)
         {
            set_error(E_DIMENSIONS, chunk->data_off + 4,
                "image height exceeds decode limit");
            return 0;
         }

         if ((uint64_t)width * height > MAX_PIXELS)
         {
            set_error(E_DIMENSIONS, chunk->data_off + 4,
                "pixel count exceeds decode limit");
            return 0;
         }

         if (!valid_color_depth(color_type, bit_depth))
         {
            set_error(E_DIMENSIONS, chunk->data_off + 8,
                "invalid color type/bit depth");
            return 0;
         }

         if (ihdr[10] != 0 || ihdr[11] != 0 || ihdr[12] > 1)
         {
            set_error(E_DIMENSIONS, chunk->data_off + 10,
                "invalid compression, filter or interlace method");
            return 0;
         }

         have_ihdr = 1;
      }

      else if (memcmp(chunk->type, "PLTE", 4) == 0)
      {
         if (!have_ihdr || have_idat || have_plte)
         {
            set_error(E_CHUNK_ORDER, p + 4, "PLTE out of place");
            return 0;
         }

         if (length == 0 || length % 3 != 0 || length > 256U * 3U)
         {
            set_error(E_CHUNK_LENGTH, p, "invalid PLTE length");
            return 0;
         }

         have_plte = 1;
      }

      else if (memcmp(chunk->type, "IDAT", 4) == 0)
      {
         if (!have_ihdr || (color_type == 3 && !have_plte) ||
             idat_closed)
         {
            set_error(E_CHUNK_ORDER, p + 4, "IDAT out of place");
            return 0;
         }

         have_idat = 1;
      }

      else if (memcmp(chunk->type, "IEND", 4) == 0)
      {
         if (!have_ihdr || !have_idat || length != 0)
         {
            set_error(length != 0 ? E_CHUNK_LENGTH : E_CHUNK_ORDER,
                length != 0 ? p : p + 4, "invalid IEND");
            return 0;
         }

         have_iend = 1;
         p += 12;
         break;
      }

      else
      {
         if (have_idat)
            idat_closed = 1;

         if ((length > MAX_CHUNK_ALLOC) &&
             ((reader.data[p + 4] & 32U) != 0))
         {
            set_error(E_ALLOC, p, "ancillary chunk exceeds allocation limit");
            return 0;
         }
      }

      p += 12 + (size_t)length;
   }

   if (!have_iend)
   {
      set_error(E_TRUNCATED, reader.size, "missing IEND");
      return 0;
   }

   if (p != reader.size)
   {
      set_error(E_TRAILING, p, "data after IEND");
      return 0;
   }

   return 1;
}

static void
locate(size_t offset, char *out, size_t out_size)
{
   int i;
   int last = -1;

   if (out_size == 0)
      return;

   out[0] = 0;
   if (offset >= reader.size)
   {
      snprintf(out, out_size, "eof+0x0");
      return;
   }

   if (offset < 8)
   {
      snprintf(out, out_size, "signature+0x%zx", offset);
      return;
   }

   if (chunk_count == 0)
   {
      snprintf(out, out_size, "chunk_header+0x%zx", offset - 8);
      return;
   }

   for (i = 0; i < chunk_count; ++i)
   {
      const Chunk *chunk = &chunks[i];

      if (offset >= chunk->header_off)
         last = i;

      if (offset >= chunk->header_off && offset < chunk->data_off)
      {
         snprintf(out, out_size, "%s(header)+0x%zx", chunk->type,
             offset - chunk->header_off);
         return;
      }

      if (offset >= chunk->data_off && offset < chunk->crc_off)
      {
         snprintf(out, out_size, "%s+0x%zx", chunk->type,
             offset - chunk->data_off);
         return;
      }

      if (offset >= chunk->crc_off && offset < chunk->crc_off + 4)
      {
         snprintf(out, out_size, "%s(CRC)+0x%zx", chunk->type,
             offset - chunk->crc_off);
         return;
      }

      if (offset == chunk->crc_off + 4)
      {
         snprintf(out, out_size, "%s(end)", chunk->type);
         return;
      }
   }

   if (last >= 0)
      snprintf(out, out_size, "%s(framing)+0x%zx", chunks[last].type,
          offset - chunks[last].data_off);
   else if (offset >= reader.size)
      snprintf(out, out_size, "eof+0x0");
   else
      snprintf(out, out_size, "unknown+0x%zx", offset);
}

static void
report_reject(const char *path)
{
   char where[96];
   int code = error_code != E_OK ? error_code : E_LIBPNG;

   locate(error_offset, where, sizeof where);
   fprintf(stderr,
       "REJECT code=%d(%s) offset=0x%zx chunk=%s detail=\"%s\" file=%s "
       "alloc_peak=%zu\n", code, error_names[code], error_offset, where,
       error_message, path, alloc_peak);
}

static void PNGCBAPI
quiet_warning(png_structp png_ptr, png_const_charp message)
{
   (void)png_ptr;
   (void)message;
}

static void PNGCBAPI
catch_error(png_structp png_ptr, png_const_charp message)
{
   snprintf(error_message, sizeof error_message, "%s",
       message != NULL ? message : "libpng decode error");
   png_longjmp(png_ptr, 1);
}

static png_voidp PNGCBAPI
tracked_malloc(png_structp png_ptr, png_alloc_size_t size)
{
   AllocHeader *header;
   size_t request = (size_t)size;
   size_t total;

   (void)png_ptr;
   if (request > MAX_SINGLE_ALLOC)
   {
      set_error(E_ALLOC, reader.pos, "single allocation exceeds limit");
      return NULL;
   }

   total = request + sizeof *header;
   if (total < request || alloc_total > MAX_TOTAL_ALLOC - total)
   {
      set_error(E_ALLOC, reader.pos, "total allocation exceeds limit");
      return NULL;
   }

   header = (AllocHeader *)malloc(total == 0 ? 1 : total);
   if (header == NULL)
   {
      set_error(E_ALLOC, reader.pos, "out of memory");
      return NULL;
   }

   header->size = request;
   header->pad = 0;
   alloc_total += total;
   if (alloc_total > alloc_peak)
      alloc_peak = alloc_total;
   return header + 1;
}

static void PNGCBAPI
tracked_free(png_structp png_ptr, png_voidp ptr)
{
   AllocHeader *header;

   (void)png_ptr;
   if (ptr == NULL)
      return;

   header = ((AllocHeader *)ptr) - 1;
   if (alloc_total >= header->size + sizeof *header)
      alloc_total -= header->size + sizeof *header;

   free(header);
}

static void PNGCBAPI
read_callback(png_structp png_ptr, png_bytep output, png_size_t length)
{
   if (reader.pos > reader.size || length > reader.size - reader.pos)
   {
      set_error(E_TRUNCATED, reader.pos, "read past end of input");
      png_error(png_ptr, "read past end of input");
      return;
   }

   memcpy(output, reader.data + reader.pos, length);
   reader.pos += length;
}

static void
classify_libpng_error(const char *message)
{
   if (error_code != E_OK)
      return;

   if (strstr(message, "CRC") != NULL)
      set_error(E_CRC, reader.pos, message);

   else if (strstr(message, "memory") != NULL ||
       strstr(message, "too large") != NULL ||
       strstr(message, "exceeds") != NULL)
      set_error(E_ALLOC, reader.pos, message);

   else
      set_error(E_LIBPNG, reader.pos, message);
}

static uint64_t
hash_bytes(const unsigned char *data, size_t size)
{
   uint64_t hash = UINT64_C(1469598103934665603);
   size_t i;

   for (i = 0; i < size; ++i)
   {
      hash ^= data[i];
      hash *= UINT64_C(1099511628211);
   }

   return hash;
}

static int
write_ppm(const char *path, const unsigned char *pixels, size_t width,
    size_t height, size_t rowbytes)
{
   FILE *file = fopen(path, "wb");
   size_t y;

   if (file == NULL)
   {
      fprintf(stderr, "fopen(%s): %s\n", path, strerror(errno));
      return 0;
   }

   if (fprintf(file, "P6\n%zu %zu\n255\n", width, height) < 0)
   {
      fclose(file);
      return 0;
   }

   for (y = 0; y < height; ++y)
   {
      const unsigned char *row = pixels + y * rowbytes;
      size_t x;

      for (x = 0; x < width; ++x)
         if (fwrite(row + x * 4, 1, 3, file) != 3)
         {
            fclose(file);
            return 0;
         }
   }

   if (fclose(file) != 0)
      return 0;

   return 1;
}

int
main(int argc, char **argv)
{
   const char *mode;
   const char *path;
   const char *ppm_path = NULL;
   FILE *file;
   long file_size;
   unsigned char *input;
   png_structp png_ptr = NULL;
   png_infop info_ptr = NULL;
   png_bytepp rows = NULL;
   png_bytep pixels = NULL;
   png_uint_32 width, height;
   size_t rowbytes;
   int exit_status = 1;

   if (argc < 3)
   {
      fprintf(stderr, "usage: %s check|ppm <file.png> [out.ppm]\n", argv[0]);
      return 2;
   }

   mode = argv[1];
   path = argv[2];
   if (strcmp(mode, "ppm") == 0)
   {
      if (argc < 4)
      {
         fprintf(stderr, "usage: %s ppm <file.png> <out.ppm>\n", argv[0]);
         return 2;
      }
      ppm_path = argv[3];
   }

   else if (strcmp(mode, "check") != 0)
   {
      fprintf(stderr, "unknown mode: %s\n", mode);
      return 2;
   }

   file = fopen(path, "rb");
   if (file == NULL)
   {
      fprintf(stderr, "fopen(%s): %s\n", path, strerror(errno));
      return 2;
   }

   if (fseek(file, 0, SEEK_END) != 0 ||
       (file_size = ftell(file)) < 0 ||
       fseek(file, 0, SEEK_SET) != 0)
   {
      fprintf(stderr, "cannot determine size of %s\n", path);
      fclose(file);
      return 2;
   }

   if ((unsigned long)file_size > MAX_INPUT_BYTES)
   {
      fprintf(stderr, "REJECT code=%d(%s) offset=0x0 chunk=input "
          "detail=\"input exceeds limit\" file=%s\n",
          E_ALLOC, error_names[E_ALLOC], path);
      fclose(file);
      return 1;
   }

   input = (unsigned char *)malloc((size_t)file_size + 1);
   if (input == NULL)
   {
      fprintf(stderr, "out of memory reading input\n");
      fclose(file);
      return 2;
   }

   if (fread(input, 1, (size_t)file_size, file) != (size_t)file_size)
   {
      fprintf(stderr, "fread(%s): %s\n", path, strerror(errno));
      free(input);
      fclose(file);
      return 2;
   }
   fclose(file);

   reader.data = input;
   reader.size = (size_t)file_size;
   reader.pos = 0;
   error_code = E_OK;
   error_offset = 0;
   error_message[0] = 0;
   alloc_total = 0;
   alloc_peak = 0;

   if (!preflight())
   {
      report_reject(path);
      free(input);
      return 1;
   }

   png_ptr = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, NULL,
       catch_error, quiet_warning, NULL, tracked_malloc, tracked_free);
   if (png_ptr == NULL)
   {
      if (error_code == E_OK)
         set_error(E_ALLOC, reader.pos, "cannot create png_struct");
      report_reject(path);
      free(input);
      return 1;
   }

   info_ptr = png_create_info_struct(png_ptr);
   if (info_ptr == NULL)
   {
      if (error_code == E_OK)
         set_error(E_ALLOC, reader.pos, "cannot create png_info");
      report_reject(path);
      png_destroy_read_struct(&png_ptr, NULL, NULL);
      free(input);
      return 1;
   }

   if (setjmp(png_jmpbuf(png_ptr)) != 0)
   {
      classify_libpng_error(error_message[0] != 0 ?
          error_message : "libpng decode error");
      if (error_code == E_OK)
         set_error(E_LIBPNG, reader.pos, "libpng decode error");
      report_reject(path);
      goto cleanup;
   }

   png_set_read_fn(png_ptr, &reader, read_callback);
#ifdef PNG_SET_USER_LIMITS_SUPPORTED
   png_set_user_limits(png_ptr, MAX_DIM, MAX_DIM);
   png_set_chunk_malloc_max(png_ptr, MAX_CHUNK_ALLOC);
#endif
   png_read_info(png_ptr, info_ptr);

   width = png_get_image_width(png_ptr, info_ptr);
   height = png_get_image_height(png_ptr, info_ptr);
   if (width == 0 || height == 0 || width > MAX_DIM || height > MAX_DIM ||
       (uint64_t)width * height > MAX_PIXELS)
   {
      set_error(E_DIMENSIONS, reader.pos, "image exceeds decode limits");
      png_error(png_ptr, "image exceeds decode limits");
   }

   if (png_get_color_type(png_ptr, info_ptr) == PNG_COLOR_TYPE_PALETTE)
      png_set_palette_to_rgb(png_ptr);

   if (png_get_color_type(png_ptr, info_ptr) == PNG_COLOR_TYPE_GRAY &&
       png_get_bit_depth(png_ptr, info_ptr) < 8)
      png_set_expand_gray_1_2_4_to_8(png_ptr);

   if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS) != 0)
      png_set_tRNS_to_alpha(png_ptr);

   if (png_get_bit_depth(png_ptr, info_ptr) == 16)
      png_set_strip_16(png_ptr);

   if (png_get_color_type(png_ptr, info_ptr) == PNG_COLOR_TYPE_GRAY ||
       png_get_color_type(png_ptr, info_ptr) == PNG_COLOR_TYPE_GRAY_ALPHA)
      png_set_gray_to_rgb(png_ptr);

   png_set_filler(png_ptr, 0xff, PNG_FILLER_AFTER);
   (void)png_set_interlace_handling(png_ptr);
   png_read_update_info(png_ptr, info_ptr);

   rowbytes = png_get_rowbytes(png_ptr, info_ptr);
   if (rowbytes == 0 || rowbytes > MAX_ROWBYTES ||
       (size_t)height > MAX_PIXELS / (rowbytes / 4) ||
       (size_t)height > SIZE_MAX / rowbytes ||
       (size_t)height > SIZE_MAX / sizeof(png_bytep))
   {
      set_error(E_ALLOC, reader.pos, "row buffers exceed decode limits");
      png_error(png_ptr, "row buffers exceed decode limits");
   }

   pixels = (png_bytep)calloc((size_t)height, rowbytes);
   rows = (png_bytepp)calloc((size_t)height, sizeof(png_bytep));
   if (pixels == NULL || rows == NULL)
   {
      set_error(E_ALLOC, reader.pos, "cannot allocate row buffers");
      png_error(png_ptr, "cannot allocate row buffers");
   }

   {
      png_uint_32 y;
      for (y = 0; y < height; ++y)
         rows[y] = pixels + (size_t)y * rowbytes;
   }

   png_read_image(png_ptr, rows);
   png_read_end(png_ptr, NULL);

   if (ppm_path != NULL &&
       !write_ppm(ppm_path, pixels, width, height, rowbytes))
   {
      fprintf(stderr, "cannot write %s\n", ppm_path);
      exit_status = 2;
      goto cleanup;
   }

   printf("OK width=%u height=%u rowbytes=%zu pixels=%llu hash=0x%016llx "
       "alloc_peak=%zu\n", width, height, rowbytes,
       (unsigned long long)width * height,
       (unsigned long long)hash_bytes(pixels, (size_t)height * rowbytes),
       alloc_peak);
   exit_status = 0;

cleanup:
   free(rows);
   free(pixels);
   png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
   free(input);
   return exit_status;
}
