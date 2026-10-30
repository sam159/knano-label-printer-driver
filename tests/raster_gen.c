/*
 * raster_gen - generate a CUPS raster file for testing the KNAON filter.
 *
 * Usage: raster_gen OUTPUT WIDTH HEIGHT [PATTERN] [PAGES]
 *
 * PATTERN is one of: gradient, checker, noise, text, stripes, solid
 */

#include <cups/cups.h>
#include <cups/raster.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char
pattern_pixel(const char *pattern, unsigned int x, unsigned int y,
              unsigned int width, unsigned int height)
{
  (void)height;

  if (!strcmp(pattern, "checker"))
    return (((x / 4) + (y / 4)) & 1) ? 255 : 0;

  if (!strcmp(pattern, "noise"))
    return (unsigned char)((x * 1103515245u + y * 12345u) >> 16);

  if (!strcmp(pattern, "stripes"))
    return (x % 9 < 3) ? 0 : 255;

  if (!strcmp(pattern, "text"))
  {
    /* Crude glyph-like blocks. */
    if ((y / 8) % 3 == 0 && (x / 6) % 5 != 0)
      return 0;
    return 255;
  }

  if (!strcmp(pattern, "solid"))
    return 128;

  /* gradient */
  return (unsigned char)((x * 255u) / (width > 1 ? width - 1 : 1));
}

int
main(int argc, char *argv[])
{
  const char *path;
  const char *pattern = "gradient";
  unsigned int width, height, pages = 1, page, x, y;
  int fd;
  cups_raster_t *ras;
  cups_page_header2_t header;
  unsigned char *line;

  if (argc < 4)
  {
    fprintf(stderr, "usage: %s OUTPUT WIDTH HEIGHT [PATTERN] [PAGES]\n", argv[0]);
    return (1);
  }

  path   = argv[1];
  width  = (unsigned int)atoi(argv[2]);
  height = (unsigned int)atoi(argv[3]);

  if (argc > 4)
    pattern = argv[4];
  if (argc > 5)
    pages = (unsigned int)atoi(argv[5]);

  if (width == 0 || height == 0 || pages == 0)
  {
    fprintf(stderr, "invalid dimensions/page count\n");
    return (1);
  }

  if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
  {
    perror(path);
    return (1);
  }

  ras = cupsRasterOpen(fd, CUPS_RASTER_WRITE);
  if (!ras)
  {
    perror("cupsRasterOpen");
    return (1);
  }

  line = malloc(width);

  for (page = 0; page < pages; page ++)
  {
    memset(&header, 0, sizeof(header));

    strncpy(header.MediaClass, "PwgRaster", sizeof(header.MediaClass) - 1);
    strncpy(header.MediaType, "Direct", sizeof(header.MediaType) - 1);
    header.HWResolution[0] = 203;
    header.HWResolution[1] = 203;
    header.PageSize[0]     = (unsigned int)(width * 72 / 203);
    header.PageSize[1]     = (unsigned int)(height * 72 / 203);
    header.cupsWidth        = width;
    header.cupsHeight       = height;
    header.cupsBitsPerColor = 8;
    header.cupsBitsPerPixel = 8;
    header.cupsBytesPerLine = width;
    header.cupsColorOrder   = CUPS_ORDER_CHUNKED;
    header.cupsColorSpace   = CUPS_CSPACE_W;
    header.cupsCompression  = 0;
    header.NumCopies        = 1;

    if (!cupsRasterWriteHeader2(ras, &header))
    {
      fprintf(stderr, "cupsRasterWriteHeader2 failed\n");
      return (1);
    }

    for (y = 0; y < height; y ++)
    {
      for (x = 0; x < width; x ++)
        line[x] = pattern_pixel(pattern, x, y + page * 7, width, height);

      if (!cupsRasterWritePixels(ras, line, width))
      {
        fprintf(stderr, "cupsRasterWritePixels failed\n");
        return (1);
      }
    }
  }

  free(line);
  cupsRasterClose(ras);
  close(fd);

  return (0);
}
