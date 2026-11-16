/*
 * Small libpng decode probe used by the property runner.
 *
 * It deliberately uses the simplified read API so the same source can be
 * linked against this checkout and against the system libpng reference.
 */

#include "png.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
safe_message(char *out, size_t out_size, const char *message)
{
   size_t i;

   if (out_size == 0)
      return;

   for (i = 0; i + 1 < out_size && message[i] != '\0'; ++i)
   {
      unsigned char ch = (unsigned char)message[i];
      out[i] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
   }
   out[i] = '\0';
}

static int
read_file(const char *path, unsigned char **data, size_t *size)
{
   FILE *stream;
   long length;
   unsigned char *buffer;

   stream = fopen(path, "rb");
   if (stream == NULL)
   {
      fprintf(stderr, "open %s: %s\n", path, strerror(errno));
      return 0;
   }

   if (fseek(stream, 0, SEEK_END) != 0 || (length = ftell(stream)) < 0 ||
       fseek(stream, 0, SEEK_SET) != 0)
   {
      fprintf(stderr, "size %s: %s\n", path, strerror(errno));
      fclose(stream);
      return 0;
   }

   buffer = (unsigned char *)malloc((size_t)length + (length == 0));
   if (buffer == NULL)
   {
      fprintf(stderr, "allocate input %s failed\n", path);
      fclose(stream);
      return 0;
   }

   if (length != 0 && fread(buffer, 1, (size_t)length, stream) != (size_t)length)
   {
      fprintf(stderr, "read %s failed\n", path);
      free(buffer);
      fclose(stream);
      return 0;
   }

   fclose(stream);
   *data = buffer;
   *size = (size_t)length;
   return 1;
}

static int
parse_limit(const char *text, uint64_t *limit)
{
   char *end = NULL;
   unsigned long long value;

   errno = 0;
   value = strtoull(text, &end, 0);
   if (errno != 0 || end == text || *end != '\0')
      return 0;

   *limit = (uint64_t)value;
   return 1;
}

static int
image_size(const png_image *image, uint64_t *size)
{
   uint64_t component_size =
       (image->format & PNG_FORMAT_FLAG_LINEAR) != 0 ? 2U : 1U;
   uint64_t row = (uint64_t)image->width * 4U * component_size;

   if (image->width != 0 &&
       row / (4U * component_size) != image->width)
      return 0;
   if (image->height != 0 && row > UINT64_MAX / image->height)
      return 0;

   *size = row * image->height;
   return 1;
}

static int
decode(const char *path, uint64_t max_bytes, int write_raw, const char *raw_path,
    png_uint_32 format)
{
   unsigned char *input = NULL;
   unsigned char *pixels = NULL;
   size_t input_size = 0;
   uint64_t allocation_size = 0;
   png_image image;
   int ok = 0;

   if (!read_file(path, &input, &input_size))
      return 3;

   memset(&image, 0, sizeof image);
   image.version = PNG_IMAGE_VERSION;

   if (!png_image_begin_read_from_memory(&image, input, input_size))
   {
      char message[128];
      safe_message(message, sizeof message, image.message);
      printf("REJECT\tstage=begin\tmessage=%s\n", message);
      png_image_free(&image);
      free(input);
      return 0;
   }

   image.format = format;
   if (!image_size(&image, &allocation_size))
   {
      printf("LIMIT\tstage=size\tw=%lu\th=%lu\toverflow=1\n",
             (unsigned long)image.width, (unsigned long)image.height);
      png_image_free(&image);
      free(input);
      return 0;
   }

   if (allocation_size > max_bytes)
   {
      printf("LIMIT\tstage=size\tw=%lu\th=%lu\tbytes=%llu\n",
             (unsigned long)image.width, (unsigned long)image.height,
             (unsigned long long)allocation_size);
      png_image_free(&image);
      free(input);
      return 0;
   }

   if (allocation_size != 0)
   {
      pixels = (unsigned char *)malloc((size_t)allocation_size);
      if (pixels == NULL)
      {
         printf("LIMIT\tstage=alloc\tw=%lu\th=%lu\tbytes=%llu\n",
                (unsigned long)image.width, (unsigned long)image.height,
                (unsigned long long)allocation_size);
         png_image_free(&image);
         free(input);
         return 0;
      }
   }

   if (!png_image_finish_read(&image, NULL, pixels, 0, NULL))
   {
      char message[128];
      safe_message(message, sizeof message, image.message);
      printf("REJECT\tstage=finish\tw=%lu\th=%lu\tmessage=%s\n",
             (unsigned long)image.width, (unsigned long)image.height, message);
      free(pixels);
      png_image_free(&image);
      free(input);
      return 0;
   }

   if (write_raw)
   {
      FILE *stream = fopen(raw_path, "wb");
      if (stream == NULL)
      {
         fprintf(stderr, "open output %s: %s\n", raw_path, strerror(errno));
         free(pixels);
         png_image_free(&image);
         free(input);
         return 4;
      }
      if (allocation_size != 0 &&
          fwrite(pixels, 1, (size_t)allocation_size, stream) !=
              (size_t)allocation_size)
      {
         fprintf(stderr, "write output %s failed\n", raw_path);
         fclose(stream);
         free(pixels);
         png_image_free(&image);
         free(input);
         return 4;
      }
      if (fclose(stream) != 0)
      {
         fprintf(stderr, "close output %s failed\n", raw_path);
         free(pixels);
         png_image_free(&image);
         free(input);
         return 4;
      }
   }

   ok = 1;
   printf("OK\tw=%lu\th=%lu\tbytes=%llu\tbpc=%u\twarn=%lu\n",
          (unsigned long)image.width, (unsigned long)image.height,
          (unsigned long long)allocation_size,
          (unsigned int)((image.format & PNG_FORMAT_FLAG_LINEAR) != 0 ? 2 : 1),
          (unsigned long)(image.warning_or_error & PNG_IMAGE_WARNING));

   free(pixels);
   png_image_free(&image);
   free(input);
   return ok ? 0 : 5;
}

int
main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "version") == 0)
   {
      printf("%s\n", PNG_LIBPNG_VER_STRING);
      return 0;
   }

   if (argc == 4 && strcmp(argv[1], "probe") == 0)
   {
      uint64_t limit;
      if (!parse_limit(argv[3], &limit))
      {
         fprintf(stderr, "invalid byte limit: %s\n", argv[3]);
         return 2;
      }
      return decode(argv[2], limit, 0, NULL, PNG_FORMAT_RGBA);
   }

   if (argc == 5 && strcmp(argv[1], "decode") == 0)
   {
      uint64_t limit;
      if (!parse_limit(argv[4], &limit))
      {
         fprintf(stderr, "invalid byte limit: %s\n", argv[4]);
         return 2;
      }
      {
         const char *format = getenv("PNG_PROBE_FORMAT");
         png_uint_32 output_format =
             (format != NULL && strcmp(format, "rgba16") == 0) ?
                 PNG_FORMAT_LINEAR_RGB_ALPHA : PNG_FORMAT_RGBA;

         return decode(argv[2], limit, 1, argv[3], output_format);
      }
   }

   fprintf(stderr,
           "usage: %s version\n"
           "       %s probe FILE MAX_IMAGE_BYTES\n"
           "       %s decode FILE RAW_RGBA MAX_IMAGE_BYTES\n",
           argv[0], argv[0], argv[0]);
   return 2;
}
