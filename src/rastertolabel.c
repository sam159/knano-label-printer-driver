/*
 * Label printer filter for CUPS.
 *
 * Copyright 2007-2016 by Apple Inc.
 * Copyright 2001-2007 by Easy Software Products.
 *
 * These coded instructions, statements, and computer programs are the
 * property of Apple Inc. and are protected by Federal copyright
 * law.  Distribution and use rights are outlined in the file "LICENSE.txt"
 * which should have been included with this file.  If this file is
 * missing or damaged, see the license at "http://www.cups.org/".
 *
 * This file is subject to the Apple OS-Developed Software exception.
 */

/*
 * KNAON label printer CUPS filter -- reconstructed source.
 *
 * This is a reverse-engineered, behavior-preserving reconstruction of the
 * vendor-supplied `knaon/Filter/rastertolabel` binary from
 * Knaon_Linux_Driver (PPD file version 1.1.1, *cupsModelNumber: 20).
 *
 * The binary was built from a modified copy of the CUPS 2.2.7 sample filter
 * `filter/rastertolabel.c` (the copyright notice above).
 *
 * The vendor added:
 *   - printer-model detection through the CUPS device URI,
 *   - a KNAON-specific Page Description Language output path (model 20),
 *   - image rotation, brightness/contrast, thresholding and dithering.
 *
 * Reconstructed from the unstripped binary (DWARF debug info, GCC 7.5.0,
 * -Os) and a differential test harness.  The only intentional deviation is
 * g_GammaLUT being 256 bytes instead of the original 255-byte array (the
 * original writes index 255 past the end of the array).
 */

#include <cups/cups.h>
#include <cups/http.h>
#include <cups/ppd.h>
#include <cups/raster.h>

/*
 * CUPS private localization helpers used by the original filter.  They are
 * exported by libcups but live in cups/i18n.h, which needs CUPS' private
 * config.h to compile, so declare the two functions we use here.
 */

extern void _cupsLangPrintError(const char *prefix, const char *message);
extern int _cupsLangPrintFilter(FILE *fp, const char *prefix,
                                const char *message, ...)
                                __attribute__((format(printf, 3, 4)));

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Model number constants (from the original CUPS sample filter), plus the
 * KNAON model used by KnaonLabel.ppd (*cupsModelNumber: 20).
 */

#define DYMO_3x0        0       /* Dymo Labelwriter 300/330/330 Turbo */
#define ZEBRA_EPL_LINE  0x10    /* Zebra EPL line mode printers */
#define ZEBRA_EPL_PAGE  0x11    /* Zebra EPL page mode printers */
#define ZEBRA_ZPL       0x12    /* Zebra ZPL-based printers */
#define ZEBRA_CPCL      0x13    /* Zebra CPCL-based printers */
#define KNAON_LABEL     0x14    /* KNAON label printer (this driver) */
#define INTELLITECH_PCL 0x20    /* Intellitech PCL-based printers */

typedef unsigned char UNIT8;

/*
 * Globals...
 */

unsigned char *Buffer;          /* Output buffer (one raster line) */
unsigned char *CompBuffer;      /* Compression buffer */
unsigned char *LastBuffer;      /* Last buffer */
unsigned char *BufferBmp;       /* Complete page bitmap (8bpp gray) */
unsigned char *BufferDes;       /* Destination bitmap after transforms */

unsigned Feed;                  /* Number of lines to skip */
int LastSet;                    /* Number of repeat characters */

int ModelNumber,                /* cupsModelNumber attribute */
    Page,                       /* Current page */
    Canceled;                   /* Non-zero if job is canceled */

int rotate;                     /* Rotate option (0, 1, 2, 3) */

unsigned int **mymatrix;        /* Dither matrix rows */
unsigned int mymatrixWidth;     /* Dither matrix width */
unsigned int mymatrixHeight;    /* Dither matrix height */
unsigned int mydivisor;         /* Dither error divisor */
bool myuseShifting;             /* Divide by shifting instead of division */
unsigned int mystartingOffset;  /* Horizontal matrix offset */

UNIT8 g_GammaLUT[256];          /* Gamma lookup table */

/*
 * Prototypes...
 */

void CancelJob(int sig);
void Setup(ppd_file_t *ppd);
void StartPage(ppd_file_t *ppd, cups_page_header2_t *header);
void EndPage(ppd_file_t *ppd, cups_page_header2_t *header);
void OutputLine(ppd_file_t *ppd, cups_page_header2_t *header, unsigned y);
void PCLCompress(unsigned char *line, unsigned length);
void ZPLCompress(unsigned char repeat_char, unsigned repeat_count);

void rotate90(unsigned int biWidth, unsigned int biHeight,
              unsigned char *srcData, unsigned char *desData);
void rotate180(unsigned int biWidth, unsigned int biHeight,
               unsigned char *srcData, unsigned char *desData);
void rotate270(unsigned int biWidth, unsigned int biHeight,
               unsigned char *srcData, unsigned char *desData);

char *getDeviceUriWithPrinterName(void);
int search_str(char *mainstr, char *substr);

void ProcessImage(ppd_file_t *ppd, cups_page_header2_t *header);
void OutputAllImageData(ppd_file_t *ppd, cups_page_header2_t *header);

void sobel(unsigned char *data, unsigned int width, unsigned int height);
void erosion(unsigned char *data, unsigned int width, unsigned int height);
void dilation(unsigned char *data, unsigned int width, unsigned int height);
unsigned char threshold(unsigned char *data, unsigned int width,
                        unsigned int height);
void imageShake(unsigned char *data, unsigned int width, unsigned int height,
                unsigned int **matrix, unsigned int matrixWidth,
                unsigned int matrixHeight, unsigned int divisor,
                bool useShifting, unsigned char threshold);
void buildTable(double fPrecompensation);
unsigned char TransformPixel(unsigned char gray, unsigned char threshold);
void createTransformedImage(unsigned char *data, unsigned int width,
                            unsigned int height, unsigned char threshold);
void diffuse(unsigned char *data, unsigned char ori, unsigned char trans,
             unsigned int x, unsigned int y, unsigned int width,
             unsigned int height);
void ImageBrightnessGray(int biWidth, int biHeight, unsigned char *srcData,
                         float strength);
void ImageContrastGray(int biWidth, int biHeight, unsigned char *srcData,
                       float strength);

/*
 * Dither matrices used by the "PrintEffect" PPD option.
 *
 * Effect1 is a 3x5 Floyd-Steinberg-like matrix with divisor 42,
 * Effect2 is a 3x5 matrix with divisor 48.
 */

static unsigned int effect1Matrix[3][5] = {
  { 0, 0, 0, 8, 4 },
  { 2, 4, 8, 4, 2 },
  { 1, 2, 4, 2, 1 }
};

static unsigned int effect2Matrix[3][5] = {
  { 0, 0, 0, 7, 5 },
  { 3, 5, 7, 5, 3 },
  { 1, 3, 5, 3, 1 }
};

/*
 * 'rotate90()' - Rotate a bitmap 90 degrees clockwise.
 */

void
rotate90(unsigned int biWidth, unsigned int biHeight,
         unsigned char *srcData, unsigned char *desData)
{
  unsigned int i, j, src, dst = 0;

  for (j = 0; j < biWidth; j ++)
  {
    src = j + (biHeight - 1) * biWidth;

    for (i = 0; i < biHeight; i ++)
    {
      desData[dst ++] = srcData[src];
      src -= biWidth;
    }
  }
}

/*
 * 'rotate180()' - Rotate a bitmap 180 degrees.
 */

void
rotate180(unsigned int biWidth, unsigned int biHeight,
          unsigned char *srcData, unsigned char *desData)
{
  unsigned int i, j, src, dst = 0;

  for (j = 0; j < biHeight; j ++)
  {
    src = (biHeight - 1 - j) * biWidth + biWidth - 1;

    for (i = 0; i < biWidth; i ++)
    {
      desData[dst ++] = srcData[src];
      src --;
    }
  }
}

/*
 * 'rotate270()' - Rotate a bitmap 270 degrees clockwise.
 */

void
rotate270(unsigned int biWidth, unsigned int biHeight,
          unsigned char *srcData, unsigned char *desData)
{
  unsigned int i, j, src, dst = 0;

  for (j = biWidth; j > 0; j --)
  {
    src = j - 1;

    for (i = 0; i < biHeight; i ++)
    {
      desData[dst ++] = srcData[src];
      src += biWidth;
    }
  }
}

/*
 * 'Setup()' - Prepare the printer for printing.
 */

void
Setup(ppd_file_t *ppd)                  /* I - PPD file */
{
  int i;                                /* Looping var */

 /*
  * Get the model number from the PPD file...
  */

  if (ppd)
    ModelNumber = ppd->model_number;

 /*
  * Initialize based on the model number...
  */

  switch (ModelNumber)
  {
    case DYMO_3x0 :
       /*
        * Clear any remaining data...
        */

        for (i = 0; i < 100; i ++)
          putchar(0x1b);

       /*
        * Reset the printer...
        */

        fputs("\033@", stdout);
        break;

    case ZEBRA_EPL_LINE :
    case ZEBRA_EPL_PAGE :
    case ZEBRA_ZPL :
    case ZEBRA_CPCL :
    case KNAON_LABEL :
        break;

    case INTELLITECH_PCL :
       /*
        * Send a PCL reset sequence.
        */

        putchar(0x1b);
        putchar('E');
        break;
  }
}

/*
 * 'getDeviceUriWithPrinterName()' - Return the CUPS device-uri of the
 * default destination.
 */

char *
getDeviceUriWithPrinterName(void)
{
  char *name = NULL;                    /* Destination name */
  char *instance = NULL;                /* Instance name */
  http_t *pHTTPConnection;              /* HTTP connection to CUPS */
  cups_dest_t *mdest;                   /* Named destination */
  char *printer;                        /* Printer name */
  int num_dests;                        /* Number of destinations */
  cups_dest_t *dest;                    /* Destination */
  cups_dest_t *dests;                   /* Destinations */
  const char *value;                    /* device-uri value */

  pHTTPConnection = httpConnectEncrypt(cupsServer(), ippPort(),
                                       cupsEncryption());
  if (pHTTPConnection != NULL)
  {
    mdest = cupsGetNamedDest(pHTTPConnection, name, instance);
    httpClose(pHTTPConnection);

    printer = mdest->name;

    num_dests = cupsGetDests(&dests);
    dest      = cupsGetDest(printer, instance, num_dests, dests);

    if (dest != NULL)
    {
      value = NULL;

      if (dest->instance == NULL)
        value = cupsGetOption("device-uri", dest->num_options, dest->options);

      cupsFreeDests(num_dests, dests);
      return ((char *)value);
    }
  }

  return (NULL);
}

/*
 * 'search_str()' - Find a substring and return its offset, or -1.
 */

int
search_str(char *mainstr, char *substr)
{
  size_t mainlen, sublen;
  size_t i, temp;

  mainlen = strlen(mainstr);
  sublen  = strlen(substr);

  if (sublen <= mainlen)
  {
    for (i = 0; i <= mainlen - sublen; i ++)
    {
      for (temp = 0; temp < sublen; temp ++)
        if (mainstr[i + temp] != substr[temp])
          break;

      if (temp == sublen)
        return ((int)i);
    }
  }

  return (-1);
}

/*
 * 'StartPage()' - Start a page of graphics.
 */

void
StartPage(ppd_file_t *ppd,              /* I - PPD file */
          cups_page_header2_t *header)  /* I - Page header */
{
  ppd_choice_t *choice;                 /* Marked choice */
  unsigned int length;                  /* Actual label length */
  int vals;                             /* Generic option value */
  int hOffset;                          /* Horizontal offset */
  int vOffset;                          /* Vertical offset */
  int FeedOffset;                       /* Feed offset */
  int GapMarkHeight;                    /* Gap/mark height */
  int GapMarkOffset;                    /* Gap/mark offset */
  int Rotate;                           /* Rotation value */
  int AutoDotted;                       /* Auto dotted-line value */

 /*
  * Show page device dictionary...
  */

  fprintf(stderr, "DEBUG: StartPage...\n");
  fprintf(stderr, "DEBUG: Duplex = %d\n", header->Duplex);
  fprintf(stderr, "DEBUG: HWResolution = [ %d %d ]\n",
          header->HWResolution[0], header->HWResolution[1]);
  fprintf(stderr, "DEBUG: ImagingBoundingBox = [ %d %d %d %d ]\n",
          header->ImagingBoundingBox[0], header->ImagingBoundingBox[1],
          header->ImagingBoundingBox[2], header->ImagingBoundingBox[3]);
  fprintf(stderr, "DEBUG: Margins = [ %d %d ]\n",
          header->Margins[0], header->Margins[1]);
  fprintf(stderr, "DEBUG: ManualFeed = %d\n", header->ManualFeed);
  fprintf(stderr, "DEBUG: MediaPosition = %d\n", header->MediaPosition);
  fprintf(stderr, "DEBUG: NumCopies = %d\n", header->NumCopies);
  fprintf(stderr, "DEBUG: Orientation = %d\n", header->Orientation);
  fprintf(stderr, "DEBUG: PageSize = [ %d %d ]\n",
          header->PageSize[0], header->PageSize[1]);
  fprintf(stderr, "DEBUG: cupsWidth = %d\n", header->cupsWidth);
  fprintf(stderr, "DEBUG: cupsHeight = %d\n", header->cupsHeight);
  fprintf(stderr, "DEBUG: cupsMediaType = %d\n", header->cupsMediaType);
  fprintf(stderr, "DEBUG: cupsBitsPerColor = %d\n", header->cupsBitsPerColor);
  fprintf(stderr, "DEBUG: cupsBitsPerPixel = %d\n", header->cupsBitsPerPixel);
  fprintf(stderr, "DEBUG: cupsBytesPerLine = %d\n", header->cupsBytesPerLine);
  fprintf(stderr, "DEBUG: cupsColorOrder = %d\n", header->cupsColorOrder);
  fprintf(stderr, "DEBUG: cupsColorSpace = %d\n", header->cupsColorSpace);
  fprintf(stderr, "DEBUG: cupsCompression = %d\n", header->cupsCompression);

  switch (ModelNumber)
  {
    case KNAON_LABEL :
       /*
        * Read the rotation option first; it affects the bitmap dimensions.
        */

        if ((choice = ppdFindMarkedChoice(ppd, "Rotate")) != NULL)
          rotate = atoi(choice->choice);

       /*
        * Send 200 NUL bytes to clear any pending printer data.
        */

        for (vals = 0; vals < 200; vals ++)
          putchar(0);

       /*
        * Set the label size in millimetres.  At 203 dpi there are
        * approximately 8 pixels per millimetre.
        */

        {
          unsigned int width  = (header->cupsWidth + 7) >> 3;
          unsigned int height = (header->cupsHeight + 7) >> 3;

          if (rotate > 1)
          {
            unsigned int temp = width;

            width  = height;
            height = temp;
          }

          printf("SIZE %u mm,%u mm\r\n", width, height);
        }

       /*
        * Set the origin offset in pixels (the PPD stores millimetres).
        */

        hOffset = 0;
        if ((choice = ppdFindMarkedChoice(ppd, "AdjustHoriaontal")) != NULL)
          hOffset = atoi(choice->choice) * 8;

        vOffset = 0;
        if ((choice = ppdFindMarkedChoice(ppd, "AdjustVertical")) != NULL)
          vOffset = atoi(choice->choice) * 8;

        printf("REFERENCE %d,%d\r\n", hOffset, vOffset);

       /*
        * Set media tracking...
        */

        if ((choice = ppdFindMarkedChoice(ppd, "zeMediaTracking")) != NULL)
        {
          if (!strcmp(choice->choice, "Continuous"))
          {
            puts("GAP 0 mm,0 mm\r");
          }
          else if (!strcmp(choice->choice, "Gap"))
          {
            GapMarkHeight = 3;
            if ((choice = ppdFindMarkedChoice(ppd, "GapOrMarkHeight")) != NULL)
              GapMarkHeight = atoi(choice->choice);

            GapMarkOffset = 0;
            if ((choice = ppdFindMarkedChoice(ppd, "GapOrMarkOffset")) != NULL)
              GapMarkOffset = atoi(choice->choice);

            printf("GAP %d mm,%d mm\r\n", GapMarkHeight, GapMarkOffset);
          }
          else
          {
            GapMarkHeight = 3;
            if ((choice = ppdFindMarkedChoice(ppd, "GapOrMarkHeight")) != NULL)
              GapMarkHeight = atoi(choice->choice);

            GapMarkOffset = 0;
            if ((choice = ppdFindMarkedChoice(ppd, "GapOrMarkOffset")) != NULL)
              GapMarkOffset = atoi(choice->choice);

            printf("BLINE %d mm,%d mm\r\n", GapMarkHeight, GapMarkOffset);
          }
        }

       /*
        * Set darkness...
        */

        if ((choice = ppdFindMarkedChoice(ppd, "Darkness")) != NULL &&
            strcmp(choice->choice, "Default"))
          printf("DENSITY %d\r\n", atoi(choice->choice));

       /*
        * Set print speed...
        */

        if ((choice = ppdFindMarkedChoice(ppd, "zePrintRate")) != NULL &&
            strcmp(choice->choice, "Default"))
        {
          vals = atoi(choice->choice);
          if (vals == 1)
            vals = 2;

          printf("SPEED %d\r\n", vals);
        }

       /*
        * Set dotted-line printing...
        */

        if ((choice = ppdFindMarkedChoice(ppd, "AutoDotted")) == NULL ||
            atoi(choice->choice) == 0)
          puts("SETC AUTODOTTED OFF\r");
        else
          puts("SETC AUTODOTTED ON\r");

        puts("SETC PAUSEKEY ON\r");
        puts("SETC WATERMARK OFF\r");
        puts("CLS\r");

       /*
        * Start the bitmap.  Width is in bytes, height in rows.
        */

        {
          unsigned int width  = header->cupsWidth;
          unsigned int height = header->cupsHeight;

          if (rotate > 1)
          {
            unsigned int temp = width;

            width  = height;
            height = temp;
          }

          printf("BITMAP 0,0,%u,%u,1,", (width + 7) >> 3, height);
        }

       /*
        * The original binary builds these diagnostic strings but never
        * emits them.  Keep them to preserve the original code layout and
        * stack usage.
        */

        {
          char buf1[200], buf2[200], buf3[200], buf4[200];

          memset(buf1, 0, sizeof(buf1));
          memset(buf2, 0, sizeof(buf2));
          memset(buf3, 0, sizeof(buf3));
          memset(buf4, 0, sizeof(buf4));

          sprintf(buf1, "\"cupsBitsPerPixel = %u  cupsBitsPerColor = %u  \"",
                  header->cupsBitsPerPixel, header->cupsBitsPerColor);
          sprintf(buf2, "\"  cupsColorOrder = %d  cupsColorSpace = %d\"",
                  header->cupsColorOrder, header->cupsColorSpace);
          sprintf(buf3, "\"cupsWidth = %u  cupsHeight = %u \"",
                  header->cupsWidth, header->cupsHeight);
          sprintf(buf4, "\"cupsBytesPerLine = %u  \"", header->cupsBytesPerLine);
        }

        break;

    case DYMO_3x0 :
       /*
        * Setup printer/job attributes...
        */

        length = header->PageSize[1] * header->HWResolution[1] / 72;

        printf("\033L%c%c", length >> 8, length);
        printf("\033D%c", header->cupsBytesPerLine);
        printf("\033%c", header->cupsCompression + 'c'); /* Darkness */
        break;

    case ZEBRA_EPL_LINE :
       /*
        * Set print rate...
        */

        if ((choice = ppdFindMarkedChoice(ppd, "zePrintRate")) != NULL &&
            strcmp(choice->choice, "Default"))
          printf("\033S%.0f", atof(choice->choice) * 2.0 - 2.0);

       /*
        * Set darkness...
        */

        if (header->cupsCompression > 0 && header->cupsCompression <= 100)
          printf("\033D%d", 7 * header->cupsCompression / 100);

       /*
        * Set left margin to 0...
        */

        fputs("\033M01", stdout);

       /*
        * Start buffered output...
        */

        fputs("\033B", stdout);
        break;

    case ZEBRA_EPL_PAGE :
       /*
        * Start a new label...
        */

        puts("");
        puts("N");

       /*
        * Set hardware options...
        */

        if (!strcmp(header->MediaType, "Direct"))
          puts("OD");

       /*
        * Set print rate...
        */

        if ((choice = ppdFindMarkedChoice(ppd, "zePrintRate")) != NULL &&
            strcmp(choice->choice, "Default"))
        {
          double val = atof(choice->choice);

          if (val >= 3.0)
            printf("S%.0f\n", val);
          else
            printf("S%.0f\n", val * 2.0 - 2.0);
        }

       /*
        * Set darkness...
        */

        if (header->cupsCompression > 0 && header->cupsCompression <= 100)
          printf("D%u\n", 15 * header->cupsCompression / 100);

       /*
        * Set label size...
        */

        printf("q%u\n", (header->cupsWidth + 7) & ~7U);
        break;

    case ZEBRA_ZPL :
       /*
        * Set darkness...
        */

        if (header->cupsCompression > 0 && header->cupsCompression <= 100)
          printf("~SD%02u\n", 30 * header->cupsCompression / 100);

       /*
        * Start bitmap graphics...
        */

        printf("~DGR:CUPS.GRF,%u,%u,\n",
               header->cupsHeight * header->cupsBytesPerLine,
               header->cupsBytesPerLine);

       /*
        * Allocate compression buffers...
        */

        CompBuffer = malloc(2 * header->cupsBytesPerLine + 1);
        LastBuffer = malloc(header->cupsBytesPerLine);
        LastSet    = 0;
        break;

    case ZEBRA_CPCL :
       /*
        * Start label...
        */

        printf("! 0 %u %u %u %u\r\n", header->HWResolution[0],
               header->HWResolution[1], header->cupsHeight,
               header->NumCopies);
        printf("PAGE-WIDTH %u\r\n", header->cupsWidth);
        printf("PAGE-HEIGHT %u\r\n", header->cupsWidth);
        break;

    case INTELLITECH_PCL :
       /*
        * Set the media size...
        */

        printf("\033&l6D\033&k12H");    /* Set 6 LPI, 10 CPI */
        printf("\033&l0O");             /* Set portrait orientation */

        switch (header->PageSize[1])
        {
          case 540 : /* Monarch Envelope */
              printf("\033&l80A");      /* Set page size */
              break;

          case 624 : /* DL Envelope */
              printf("\033&l90A");      /* Set page size */
              break;

          case 649 : /* C5 Envelope */
              printf("\033&l91A");      /* Set page size */
              break;

          case 684 : /* COM-10 Envelope */
              printf("\033&l81A");      /* Set page size */
              break;

          case 756 : /* Executive */
              printf("\033&l1A");       /* Set page size */
              break;

          case 792 : /* Letter */
              printf("\033&l2A");       /* Set page size */
              break;

          case 842 : /* A4 */
              printf("\033&l26A");      /* Set page size */
              break;

          case 1008 : /* Legal */
              printf("\033&l3A");       /* Set page size */
              break;

          default : /* Custom size */
              printf("\033!f%uZ", header->PageSize[1] * 300 / 72);
              break;
        }

        printf("\033&l%uP",              /* Set page length */
               header->PageSize[1] / 12);
        printf("\033&l0E");              /* Set top margin to 0 */
        if (header->NumCopies)
          printf("\033&l%uX", header->NumCopies);
                                         /* Set number copies */
        printf("\033&l0L");              /* Turn off perforation skip */

       /*
        * Print settings...
        */

        if (Page == 1)
        {
          if (header->cupsRowFeed)       /* inPrintRate */
            printf("\033!p%uS", header->cupsRowFeed);

          if (header->cupsCompression != ~0U)
                                         /* inPrintDensity */
            printf("\033&d%uA", 30 * header->cupsCompression / 100 - 15);

          if ((choice = ppdFindMarkedChoice(ppd, "inPrintMode")) != NULL)
          {
            if (!strcmp(choice->choice, "Standard"))
              fputs("\033!p0M", stdout);
            else if (!strcmp(choice->choice, "Tear"))
            {
              fputs("\033!p1M", stdout);

              if (header->cupsRowCount)  /* inTearInterval */
                printf("\033!n%uT", header->cupsRowCount);
            }
            else
            {
              fputs("\033!p2M", stdout);

              if (header->cupsRowStep)   /* inCutInterval */
                printf("\033!n%uC", header->cupsRowStep);
            }
          }
        }

       /*
        * Setup graphics...
        */

        printf("\033*t%uR", header->HWResolution[0]);
                                         /* Set resolution */

        printf("\033*r%uS", header->cupsWidth);
                                         /* Set width */
        printf("\033*r%uT", header->cupsHeight);
                                         /* Set height */

        printf("\033&a0H");              /* Set horizontal position */
        printf("\033&a0V");              /* Set vertical position */
        printf("\033*r1A");              /* Start graphics */
        printf("\033*b3M");              /* Set compression */

       /*
        * Allocate compression buffers...
        */

        CompBuffer = malloc(2 * header->cupsBytesPerLine + 1);
        LastBuffer = malloc(header->cupsBytesPerLine);
        LastSet    = 0;
        break;
  }

 /*
  * Allocate memory for a line of graphics...
  */

  Buffer    = malloc(header->cupsBytesPerLine);
  BufferBmp = malloc(header->cupsBytesPerLine * header->cupsHeight);
  Feed      = 0;

  (void)FeedOffset;
  (void)Rotate;
  (void)AutoDotted;
}

/*
 * 'EndPage()' - Finish a page of graphics.
 */

void
EndPage(ppd_file_t *ppd,                /* I - PPD file */
        cups_page_header2_t *header)    /* I - Page header */
{
  int val;                              /* Option value */
  ppd_choice_t *choice;                 /* Marked choice */

  switch (ModelNumber)
  {
    case KNAON_LABEL :
        puts("PRINT 1,1\r");
        break;

    case DYMO_3x0 :
       /*
        * Eject the current page...
        */

        fputs("\033E", stdout);
        break;

    case ZEBRA_EPL_LINE :
       /*
        * End buffered output, eject the label...
        */

        fputs("\033E\f", stdout);
        break;

    case ZEBRA_EPL_PAGE :
       /*
        * Print the label...
        */

        puts("P1");

       /*
        * Cut the label as needed...
        */

        if (header->CutMedia)
          puts("C");
        break;

    case ZEBRA_ZPL :
        if (Canceled)
        {
         /*
          * Cancel bitmap download...
          */

          puts("~DN");
          break;
        }

       /*
        * Start label...
        */

        puts("^XA");

       /*
        * Set print rate...
        */

        if ((choice = ppdFindMarkedChoice(ppd, "zePrintRate")) != NULL &&
            strcmp(choice->choice, "Default"))
        {
          val = atoi(choice->choice);
          printf("^PR%d,%d,%d\n", val, val, val);
        }

       /*
        * Put label home in default position (0,0)...
        */

        printf("^LH0,0\n");

       /*
        * Set media tracking...
        */

        if (ppdIsMarked(ppd, "zeMediaTracking", "Continuous"))
        {
         /*
          * Add label length command for continuous...
          */

          printf("^LL%d\n", header->cupsHeight);
          printf("^MNN\n");
        }
        else if (ppdIsMarked(ppd, "zeMediaTracking", "Web"))
          printf("^MNY\n");
        else if (ppdIsMarked(ppd, "zeMediaTracking", "Mark"))
          printf("^MNM\n");

       /*
        * Set label top
        */

        if (header->cupsRowStep != 200)
          printf("^LT%d\n", header->cupsRowStep);

       /*
        * Set media type...
        */

        if (!strcmp(header->MediaType, "Thermal"))
          printf("^MTT\n");
        else if (!strcmp(header->MediaType, "Direct"))
          printf("^MTD\n");

       /*
        * Set print mode...
        */

        if ((choice = ppdFindMarkedChoice(ppd, "zePrintMode")) != NULL &&
            strcmp(choice->choice, "Saved"))
        {
          printf("^MM");

          if (!strcmp(choice->choice, "Tear"))
            printf("T,Y\n");
          else if (!strcmp(choice->choice, "Peel"))
            printf("P,Y\n");
          else if (!strcmp(choice->choice, "Rewind"))
            printf("R,Y\n");
          else if (!strcmp(choice->choice, "Applicator"))
            printf("A,Y\n");
          else
            printf("C,Y\n");
        }

       /*
        * Set tear-off adjust position...
        */

        if (header->AdvanceDistance != 1000)
        {
          if ((int)header->AdvanceDistance < 0)
            printf("~TA%04d\n", (int)header->AdvanceDistance);
          else
            printf("~TA%03d\n", (int)header->AdvanceDistance);
        }

       /*
        * Allow for reprinting after an error...
        */

        if (ppdIsMarked(ppd, "zeErrorReprint", "Always"))
          printf("^JZY\n");
        else if (ppdIsMarked(ppd, "zeErrorReprint", "Never"))
          printf("^JZN\n");

       /*
        * Print multiple copies
        */

        if (header->NumCopies > 1)
          printf("^PQ%d, 0, 0, N\n", header->NumCopies);

       /*
        * Display the label image...
        */

        puts("^FO0,0^XGR:CUPS.GRF,1,1^FS");

       /*
        * Delete the bitmap, then end the label and eject...
        */

        puts("^IDR:CUPS.GRF^FS");
        puts("^XZ");

       /*
        * Cut the label as needed...
        */

        if (header->CutMedia)
          puts("^CN1");
        break;

    case ZEBRA_CPCL :
       /*
        * Set tear-off adjust position...
        */

        if (header->AdvanceDistance != 1000)
          printf("PRESENT-AT %d 1\r\n", (int)header->AdvanceDistance);

       /*
        * Allow for reprinting after an error...
        */

        if (ppdIsMarked(ppd, "zeErrorReprint", "Always"))
          puts("ON-OUT-OF-PAPER WAIT\r");
        else if (ppdIsMarked(ppd, "zeErrorReprint", "Never"))
          puts("ON-OUT-OF-PAPER PURGE\r");

       /*
        * Cut label?
        */

        if (header->CutMedia)
          puts("CUT\r");

       /*
        * Set darkness...
        */

        if (header->cupsCompression > 0)
          printf("TONE %u\r\n", 2 * header->cupsCompression);

       /*
        * Set print rate...
        */

        if ((choice = ppdFindMarkedChoice(ppd, "zePrintRate")) != NULL &&
            strcmp(choice->choice, "Default"))
        {
          val = atoi(choice->choice);
          printf("SPEED %d\r\n", val);
        }

       /*
        * Print the label...
        */

        if ((choice = ppdFindMarkedChoice(ppd, "zeMediaTracking")) == NULL ||
            strcmp(choice->choice, "Continuous"))
          puts("FORM\r");

        puts("PRINT\r");
        break;

    case INTELLITECH_PCL :
        printf("\033*rB");              /* End GFX */
        putchar('\f');                  /* Eject current page */
        break;
  }

  fflush(stdout);

 /*
  * Free memory...
  */

  free(Buffer);

  if (CompBuffer)
  {
    free(CompBuffer);
    CompBuffer = NULL;
  }

  if (LastBuffer)
  {
    free(LastBuffer);
    LastBuffer = NULL;
  }
}

/*
 * 'CancelJob()' - Cancel the current job...
 */

void
CancelJob(int sig)                      /* I - Signal */
{
  (void)sig;

  Canceled = 1;
}

/*
 * 'OutputLine()' - Output a line of graphics...
 */

void
OutputLine(ppd_file_t *ppd,             /* I - PPD file */
           cups_page_header2_t *header, /* I - Page header */
           unsigned y)                  /* I - Line number */
{
  unsigned int i;                       /* Looping var */
  unsigned int j;                       /* Looping var */
  unsigned char *ptr;                   /* Pointer into buffer */
  unsigned char temp;                   /* Temporary byte */
  unsigned char *compptr;               /* Pointer into compression buffer */
  unsigned char repeat_char;            /* Repeated character */
  unsigned int repeat_count;            /* Number of repeated characters */
  unsigned int bytewidth;               /* Byte width of the line */
  static const unsigned char *hex =
      (const unsigned char *)"0123456789ABCDEF";
                                        /* Hex digits */
  unsigned int index;                   /* Pixel index */

  (void)ppd;

  switch (ModelNumber)
  {
    case KNAON_LABEL :
       /*
        * Convert the 8bpp gray line to a 1bpp, inverted bitmap.
        */

        bytewidth = (header->cupsWidth + 7) & ~7U;

        for (ptr = Buffer, i = 0; i < bytewidth; i += 8)
        {
          unsigned int pixelColor = 0;

          for (j = 0; j < 8; j ++)
            if (i + j < header->cupsWidth && ptr[i + j] < 0xc9)
              pixelColor |= 0x80 >> j;

          putchar(~pixelColor & 0xff);
        }

        fflush(stdout);
        break;

    case DYMO_3x0 :
       /*
        * See if the line is blank; if not, write it to the printer...
        */

        if (Buffer[0] ||
            memcmp(Buffer, Buffer + 1, header->cupsBytesPerLine - 1))
        {
          if (Feed)
          {
            while (Feed > 255)
            {
              printf("\033f\001%c", 255);
              Feed -= 255;
            }

            printf("\033f\001%c", Feed);
            Feed = 0;
          }

          putchar(0x16);
          fwrite(Buffer, header->cupsBytesPerLine, 1, stdout);
          fflush(stdout);
        }
        else
          Feed ++;
        break;

    case ZEBRA_EPL_LINE :
        printf("\033g%03d", header->cupsBytesPerLine);
        fwrite(Buffer, 1, header->cupsBytesPerLine, stdout);
        fflush(stdout);
        break;

    case ZEBRA_EPL_PAGE :
        if (Buffer[0] || memcmp(Buffer, Buffer + 1, header->cupsBytesPerLine))
        {
          printf("GW0,%d,%d,1\n", y, header->cupsBytesPerLine);
          for (i = header->cupsBytesPerLine, ptr = Buffer; i > 0; i --, ptr ++)
            putchar(~*ptr);
          putchar('\n');
          fflush(stdout);
        }
        break;

    case ZEBRA_ZPL :
       /*
        * Determine if this row is the same as the previous line.
        * If so, output a ':' and return...
        */

        if (LastSet)
        {
          if (!memcmp(Buffer, LastBuffer, header->cupsBytesPerLine))
          {
            putchar(':');
            return;
          }
        }

       /*
        * Convert the line to hex digits...
        */

        for (ptr = Buffer, compptr = CompBuffer, i = header->cupsBytesPerLine;
             i > 0;
             i --, ptr ++)
        {
          *compptr++ = hex[*ptr >> 4];
          *compptr++ = hex[*ptr & 15];
        }

        *compptr = '\0';

       /*
        * Run-length compress the graphics...
        */

        for (compptr = CompBuffer + 1, repeat_char = CompBuffer[0],
             repeat_count = 1;
             *compptr;
             compptr ++)
          if (*compptr == repeat_char)
            repeat_count ++;
          else
          {
            ZPLCompress(repeat_char, repeat_count);
            repeat_char  = *compptr;
            repeat_count = 1;
          }

        if (repeat_char == '0')
        {
         /*
          * Handle 0's on the end of the line...
          */

          if (repeat_count & 1)
          {
            repeat_count --;
            putchar('0');
          }

          if (repeat_count > 0)
            putchar(',');
        }
        else
          ZPLCompress(repeat_char, repeat_count);

        fflush(stdout);

       /*
        * Save this line for the next round...
        */

        memcpy(LastBuffer, Buffer, header->cupsBytesPerLine);
        LastSet = 1;
        break;

    case ZEBRA_CPCL :
        if (Buffer[0] || memcmp(Buffer, Buffer + 1, header->cupsBytesPerLine))
        {
          printf("CG %u 1 0 %d ", header->cupsBytesPerLine, y);
          fwrite(Buffer, 1, header->cupsBytesPerLine, stdout);
          puts("\r");
          fflush(stdout);
        }
        break;

    case INTELLITECH_PCL :
        if (Buffer[0] ||
            memcmp(Buffer, Buffer + 1, header->cupsBytesPerLine - 1))
        {
          if (Feed)
          {
            printf("\033*b%dY", Feed);
            Feed    = 0;
            LastSet = 0;
          }

          PCLCompress(Buffer, header->cupsBytesPerLine);
        }
        else
          Feed ++;
        break;
  }

  (void)temp;
  (void)index;
}

/*
 * 'ProcessImage()' - Transform and emit the complete page bitmap.
 */

void
ProcessImage(ppd_file_t *ppd,           /* I - PPD file */
             cups_page_header2_t *header) /* I - Page header */
{
  unsigned int i;                       /* Looping var */
  unsigned int j;                       /* Looping var */
  unsigned int h;                       /* Looping var */
  unsigned char *ptr;                   /* Pointer into bitmap */
  unsigned char temp;                   /* Temporary byte */
  unsigned int bytewidth;               /* Byte width of the bitmap */
  unsigned int index;                   /* Bitmap index */
  int mthreshold;                       /* Threshold value */
  int mContrast;                        /* Contrast value */
  int mBrightness;                      /* Brightness value */
  unsigned int newwidth;                /* Transformed width */
  unsigned int newheight;               /* Transformed height */
  ppd_choice_t *choice;                 /* Marked choice */
  unsigned char *srcData;               /* Source bitmap */
  unsigned char *desData;               /* Destination bitmap */
  unsigned int biWidth;                 /* Source width */
  unsigned int biHeight;                /* Source height */

  (void)i;
  (void)j;
  (void)h;
  (void)ptr;
  (void)temp;
  (void)bytewidth;
  (void)index;
  (void)newwidth;
  (void)newheight;

  if (ModelNumber != KNAON_LABEL)
    return;

  biWidth  = header->cupsWidth;
  biHeight = header->cupsHeight;
  srcData  = BufferBmp;

  desData   = malloc(biWidth * biHeight);
  BufferDes = desData;
  memset(desData, 0xff, biWidth * biHeight);

  if (rotate == 0)
  {
    BufferDes = srcData;
  }
  else if (rotate == 1)
  {
    rotate180(biWidth, biHeight, srcData, desData);
    biWidth  = header->cupsWidth;
    biHeight = header->cupsHeight;
  }

  if (rotate == 2)
  {
    rotate90(header->cupsWidth, header->cupsHeight, srcData, BufferDes);
    biWidth  = header->cupsHeight;
    biHeight = header->cupsWidth;
  }

  if (rotate == 3)
  {
    rotate270(header->cupsWidth, header->cupsHeight, srcData, BufferDes);
    biWidth  = header->cupsHeight;
    biHeight = header->cupsWidth;
  }

  mthreshold = 0xaf;
  if ((choice = ppdFindMarkedChoice(ppd, "Threshold")) != NULL)
  {
    mthreshold = atoi(choice->choice);
    if (mthreshold < 0)
      mthreshold += 0x100;
  }

  mBrightness = 0x80;
  if ((choice = ppdFindMarkedChoice(ppd, "Brightness")) != NULL)
  {
    mBrightness = atoi(choice->choice);
    if (mBrightness < 0)
      mBrightness += 0x100;
  }

  mContrast = 0x80;
  if ((choice = ppdFindMarkedChoice(ppd, "Contrast")) != NULL)
  {
    mContrast = atoi(choice->choice);
    if (mContrast < 0)
      mContrast += 0x100;
  }

  ImageBrightnessGray(biWidth, biHeight, BufferDes, (float)mBrightness);
  ImageContrastGray(biWidth, biHeight, BufferDes, (float)mContrast);

  if ((choice = ppdFindMarkedChoice(ppd, "PrintEffect")) != NULL)
  {
    if (!strcmp(choice->choice, "Effect1"))
    {
      unsigned int *matrixP[3];

      matrixP[0] = effect1Matrix[0];
      matrixP[1] = effect1Matrix[1];
      matrixP[2] = effect1Matrix[2];

      threshold(BufferDes, biWidth, biHeight);
      imageShake(BufferDes, biWidth, biHeight, matrixP, 5, 3, 0x2a, false,
                 (unsigned char)mthreshold);
    }

    if (!strcmp(choice->choice, "Effect2"))
    {
      unsigned int *matrixP[3];

      matrixP[0] = effect2Matrix[0];
      matrixP[1] = effect2Matrix[1];
      matrixP[2] = effect2Matrix[2];

      threshold(BufferDes, biWidth, biHeight);
      imageShake(BufferDes, biWidth, biHeight, matrixP, 5, 3, 0x30, false,
                 (unsigned char)mthreshold);
    }
  }

 /*
  * Emit the 1bpp bitmap, inverted (0 = black).
  */

  index = 0;
  for (h = 0; h < biHeight; h ++)
  {
    for (i = 0; i < ((biWidth + 7) >> 3); i ++)
    {
      unsigned int pixelColor = 0;

      for (j = 0; j < 8; j ++)
        if ((i * 8 + j < biWidth) &&
            (BufferDes[index + i * 8 + j] <= mthreshold))
          pixelColor |= 0x80 >> j;

      putchar(~pixelColor & 0xff);
    }

    index += biWidth;
  }

  free(BufferDes);
  Buffer = NULL;
  fflush(stdout);
}

/*
 * 'OutputAllImageData()' - Alternate complete-page output routine.
 *
 * Not called by main(); retained because it is present in the vendor
 * binary.
 */

void
OutputAllImageData(ppd_file_t *ppd,     /* I - PPD file */
                   cups_page_header2_t *header) /* I - Page header */
{
  unsigned int i;                       /* Looping var */
  unsigned int j;                       /* Looping var */
  unsigned int h;                       /* Looping var */
  unsigned char *ptr;                   /* Pointer into bitmap */
  unsigned char temp;                   /* Temporary byte */
  unsigned int bytewidth;               /* Byte width of the bitmap */
  unsigned int index;                   /* Bitmap index */
  ppd_choice_t *choice;                 /* Marked choice */
  unsigned char *data;                  /* Bitmap */
  unsigned int pixelColor;              /* Output byte */

  (void)i;
  (void)j;
  (void)h;
  (void)ptr;
  (void)temp;
  (void)bytewidth;
  (void)index;

  if (ModelNumber != KNAON_LABEL)
    return;

  data = BufferBmp;

  if ((choice = ppdFindMarkedChoice(ppd, "PrintEffect")) != NULL)
  {
    if (!strcmp(choice->choice, "Effect1"))
    {
      unsigned int *matrixP[3];

      matrixP[0] = effect1Matrix[0];
      matrixP[1] = effect1Matrix[1];
      matrixP[2] = effect1Matrix[2];

      threshold(data, header->cupsWidth, header->cupsHeight);
      imageShake(data, header->cupsWidth, header->cupsHeight, matrixP, 5, 3,
                 0x2a, false, 0x80);
    }

    if (!strcmp(choice->choice, "Effect2"))
    {
      unsigned int *matrixP[3];

      matrixP[0] = effect2Matrix[0];
      matrixP[1] = effect2Matrix[1];
      matrixP[2] = effect2Matrix[2];

      threshold(data, header->cupsWidth, header->cupsHeight);
      imageShake(data, header->cupsWidth, header->cupsHeight, matrixP, 5, 3,
                 0x30, false, 0x80);
    }
  }

  for (h = 0; h < header->cupsHeight; h ++)
  {
    for (i = 0; i < ((header->cupsWidth + 7) >> 3); i ++)
    {
      pixelColor = 0;

      for (j = 0; j < 8; j ++)
        if ((i * 8 + j < header->cupsWidth) &&
            (data[header->cupsWidth * h + i * 8 + j] < 0xc9))
          pixelColor |= 0x80 >> j;

      putchar(~pixelColor & 0xff);
    }
  }

  fflush(stdout);
}

/*
 * 'PCLCompress()' - Output a PCL (mode 3) compressed line.
 */

void
PCLCompress(unsigned char *line,        /* I - Line to compress */
            unsigned length)            /* I - Length of line */
{
  unsigned char *line_ptr,              /* Current byte pointer */
                *line_end,              /* End-of-line byte pointer */
                *comp_ptr,              /* Pointer into compression buffer */
                *start,                 /* Start of compression sequence */
                *seed;                  /* Seed buffer pointer */
  unsigned count,                       /* Count of bytes for output */
           offset;                      /* Offset of bytes for output */

 /*
  * Do delta-row compression...
  */

  line_ptr = line;
  line_end = line + length;

  comp_ptr = CompBuffer;
  seed     = LastBuffer;

  while (line_ptr < line_end)
  {
   /*
    * Find the next non-matching sequence...
    */

    start = line_ptr;

    if (!LastSet)
    {
     /*
      * The seed buffer is invalid, so do the next 8 bytes, max...
      */

      offset = 0;

      if ((count = (unsigned)(line_end - line_ptr)) > 8)
        count = 8;

      line_ptr += count;
    }
    else
    {
     /*
      * The seed buffer is valid, so compare against it...
      */

      while (*line_ptr == *seed &&
             line_ptr < line_end)
      {
        line_ptr ++;
        seed ++;
      }

      if (line_ptr == line_end)
        break;

      offset = (unsigned)(line_ptr - start);

     /*
      * Find up to 8 non-matching bytes...
      */

      start = line_ptr;
      count = 0;
      while (*line_ptr != *seed &&
             line_ptr < line_end &&
             count < 8)
      {
        line_ptr ++;
        seed ++;
        count ++;
      }
    }

   /*
    * Place mode 3 compression data in the buffer; see HP manuals
    * for details...
    */

    if (offset >= 31)
    {
     /*
      * Output multi-byte offset...
      */

      *comp_ptr++ = (unsigned char)(((count - 1) << 5) | 31);

      offset -= 31;
      while (offset >= 255)
      {
        *comp_ptr++ = 255;
        offset    -= 255;
      }

      *comp_ptr++ = (unsigned char)offset;
    }
    else
    {
     /*
      * Output single-byte offset...
      */

      *comp_ptr++ = (unsigned char)(((count - 1) << 5) | offset);
    }

    memcpy(comp_ptr, start, count);
    comp_ptr += count;
  }

 /*
  * Set the length of the data and write it...
  */

  printf("\033*b%dW", (int)(comp_ptr - CompBuffer));
  fwrite(CompBuffer, (size_t)(comp_ptr - CompBuffer), 1, stdout);

 /*
  * Save this line as a "seed" buffer for the next...
  */

  memcpy(LastBuffer, line, length);
  LastSet = 1;
}

/*
 * 'ZPLCompress()' - Output a run-length compression sequence.
 */

void
ZPLCompress(unsigned char repeat_char,  /* I - Character to repeat */
            unsigned repeat_count)      /* I - Number of repeated characters */
{
  if (repeat_count > 1)
  {
   /*
    * Print as many z's as possible - they are the largest denomination
    * representing 400 characters (zC stands for 400 adjacent C's)
    */

    while (repeat_count >= 400)
    {
      putchar('z');
      repeat_count -= 400;
    }

   /*
    * Then print 'g' through 'y' as multiples of 20 characters...
    */

    if (repeat_count >= 20)
    {
      putchar((int)('f' + repeat_count / 20));
      repeat_count %= 20;
    }

   /*
    * Finally, print 'G' through 'Y' as 1 through 19 characters...
    */

    if (repeat_count > 0)
      putchar((int)('F' + repeat_count));
  }

 /*
  * Then the character to be repeated...
  */

  putchar((int)repeat_char);
}

/*
 * 'main()' - Main entry and processing of driver.
 */

int                                     /* O - Exit status */
main(int argc,                          /* I - Number of command-line args */
     char *argv[])                      /* I - Command-line arguments */
{
  int fd;                               /* File descriptor */
  cups_raster_t *ras;                   /* Raster stream for printing */
  cups_page_header2_t header;           /* Page header from file */
  unsigned y;                           /* Current line */
  ppd_file_t *ppd;                      /* PPD file */
  int num_options;                      /* Number of options */
  cups_option_t *options;               /* Options */
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  struct sigaction action;              /* Actions for POSIX signals */
#endif
#ifndef KNANO_SKIP_DEVICE_CHECK
  char *device_uri;                     /* CUPS device URI */
#endif

  (void)y;

 /*
  * Make sure status messages are not buffered...
  */

  setbuf(stderr, NULL);

 /*
  * Check command-line...
  */

  if (argc < 6 || argc > 7)
  {
   /*
    * We don't have the correct number of arguments; write an error message
    * and return.
    */

    _cupsLangPrintFilter(stderr, "ERROR",
                         "%s job-id user title copies options [file]",
                         "rastertolabel");
    return (1);
  }

 /*
  * Open the page stream...
  */

  if (argc == 7)
  {
    if ((fd = open(argv[6], O_RDONLY)) == -1)
    {
      _cupsLangPrintError("ERROR", "Unable to open raster file");
      sleep(1);
      return (1);
    }
  }
  else
    fd = 0;

#ifndef KNANO_SKIP_DEVICE_CHECK
  device_uri = getDeviceUriWithPrinterName();

  if (search_str(device_uri, "Y468") < 0 &&
      search_str(device_uri, "Y486") < 0 &&
      search_str(device_uri, "A318") < 0 &&
      search_str(device_uri, "Y810") < 0 &&
      search_str(device_uri, "Y528") < 0 &&
      search_str(device_uri, "Y528BT") < 0 &&
      search_str(device_uri, "Y812") < 0 &&
      search_str(device_uri, "Y813") < 0 &&
      search_str(device_uri, "RH40") < 0 &&
      search_str(device_uri, "RH40BT") < 0 &&
      search_str(device_uri, "Y480") < 0)
  {
    _cupsLangPrintFilter(stderr, "ERROR", "No Available Printer.");
    return (1);
  }
#endif

  ras = cupsRasterOpen(fd, CUPS_RASTER_READ);

 /*
  * Register a signal handler to eject the current page if the
  * job is cancelled.
  */

  Canceled = 0;

#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  sigset(SIGTERM, CancelJob);
#elif defined(HAVE_SIGACTION)
  memset(&action, 0, sizeof(action));

  sigemptyset(&action.sa_mask);
  action.sa_handler = CancelJob;
  sigaction(SIGTERM, &action, NULL);
#else
  signal(SIGTERM, CancelJob);
#endif

 /*
  * Open the PPD file and apply options...
  */

  num_options = cupsParseOptions(argv[5], 0, &options);

  ppd = ppdOpenFile(getenv("PPD"));
  if (!ppd)
  {
    ppd_status_t status;                /* PPD error */
    int linenum;                        /* Line number */

    _cupsLangPrintFilter(stderr, "ERROR",
                         "The PPD file could not be opened.");

    status = ppdLastError(&linenum);

    fprintf(stderr, "DEBUG: %s on line %d.\n", ppdErrorString(status), linenum);

    return (1);
  }

  ppdMarkDefaults(ppd);
  cupsMarkOptions(ppd, num_options, options);

 /*
  * Initialize the print device...
  */

  Setup(ppd);

 /*
  * Process pages as needed...
  */

  Page = 0;

  while (cupsRasterReadHeader2(ras, &header))
  {
   /*
    * Write a status message with the page number and number of copies.
    */

    if (Canceled)
      break;

    Page ++;

    fprintf(stderr, "PAGE: %d 1\n", Page);
    _cupsLangPrintFilter(stderr, "INFO", "Starting page %d.", Page);

   /*
    * Start the page...
    */

    StartPage(ppd, &header);

   /*
    * Read the whole page, transform it, and write it to the printer...
    */

    cupsRasterReadPixels(ras, BufferBmp,
                         header.cupsBytesPerLine * header.cupsHeight);

    ProcessImage(ppd, &header);
    putchar('\n');

    _cupsLangPrintFilter(stderr, "INFO", "Finished page %d.", Page);

    EndPage(ppd, &header);

    if (Canceled)
      break;
  }

 /*
  * Close the raster stream...
  */

  cupsRasterClose(ras);
  if (fd != 0)
    close(fd);

 /*
  * Close the PPD file and free the options...
  */

  ppdClose(ppd);
  cupsFreeOptions(num_options, options);

 /*
  * If no pages were printed, send an error message...
  */

  if (Page == 0)
  {
    _cupsLangPrintFilter(stderr, "ERROR", "No pages were found.");
    return (1);
  }
  else
    return (0);
}

/*
 * Image-processing helpers (in the vendor binary these are defined after
 * main()).
 */

/*
 * 'sobel()' - Simple horizontal edge detector.
 */

void
sobel(unsigned char *data, unsigned int width, unsigned int height)
{
  unsigned int i, j;
  int temp;
  unsigned long sum;
  unsigned char *tmpdata;

  tmpdata = malloc((size_t)height * width);
  memcpy(tmpdata, data, (size_t)height * width);

  for (i = 0; i < height; i ++)
  {
    for (j = 1; j < width - 1; j ++)
    {
      temp = (int)tmpdata[i * width + j] -
             (int)tmpdata[i * width + j - 1];
      sum  = (unsigned long)(temp < 0 ? -temp : temp);

      if (sum == 0xff)
        data[i * width + j - 1] = 0xff;
    }
  }

  free(tmpdata);
}

/*
 * 'erosion()' - 3x3 binary erosion.
 */

void
erosion(unsigned char *data, unsigned int width, unsigned int height)
{
  unsigned int i, j, m, n;
  unsigned int flag;
  unsigned long sum;
  unsigned char *tmpdata;

  tmpdata = malloc((size_t)height * width);
  memcpy(tmpdata, data, (size_t)height * width);

  for (i = 0; i + 1 < height; i ++)
  {
    for (j = 3; j - 2 < width - 1; j ++)
    {
      unsigned int index = i * width + width - 2 + j;
      unsigned int row   = i * width;

      flag = 0;

      for (m = i; m < i + 3; m ++)
      {
        for (n = j - 3; n < j; n ++)
        {
          if (tmpdata[index] == 0 || tmpdata[row + n] == 0)
          {
            data[index] = 0;
            flag = 1;
            break;
          }
        }

        if (flag)
          break;

        row += width;
      }

      if (!flag)
        data[index] = 0xff;
    }
  }

  free(tmpdata);
  (void)sum;
}

/*
 * 'dilation()' - 3x3 binary dilation.
 */

void
dilation(unsigned char *data, unsigned int width, unsigned int height)
{
  unsigned int i, j, m, n;
  unsigned int flag;
  unsigned long sum;
  unsigned char *tmpdata;

  tmpdata = malloc((size_t)height * width);
  memcpy(tmpdata, data, (size_t)height * width);

  for (i = 0; i + 1 < height; i ++)
  {
    for (j = 3; j - 2 < width - 1; j ++)
    {
      unsigned int index = i * width + width - 2 + j;
      unsigned int row   = i * width;

      flag = 0;

      for (m = i; m < i + 3; m ++)
      {
        for (n = j - 3; n < j; n ++)
        {
          if (tmpdata[index] == 0xff || tmpdata[row + n] == 0xff)
          {
            data[index] = 0xff;
            flag = 1;
            break;
          }
        }

        if (flag)
          break;

        row += width;
      }

      if (!flag)
        data[index] = 0;
    }
  }

  free(tmpdata);
  (void)sum;
}

/*
 * 'threshold()' - Return the average value of the image.
 */

unsigned char
threshold(unsigned char *data, unsigned int width, unsigned int height)
{
  unsigned int i, j;
  unsigned long sum = 0;
  unsigned long thre;
  unsigned char *tempdata;

  tempdata = malloc((size_t)height * width);
  memcpy(tempdata, data, (size_t)height * width);

  for (i = 0; i < height; i ++)
    for (j = 0; j < width; j ++)
      sum += tempdata[i * width + j];

  free(tempdata);

  thre = sum / ((size_t)height * width);
  return ((unsigned char)thre);
}

/*
 * 'imageShake()' - Apply error diffusion using a dither matrix.
 */

void
imageShake(unsigned char *data, unsigned int width, unsigned int height,
           unsigned int **matrix, unsigned int matrixWidth,
           unsigned int matrixHeight, unsigned int divisor,
           bool useShifting, unsigned char threshold)
{
  double tempthre;
  double fPrecompensation;
  unsigned int i;

  tempthre = (double)threshold;

  fPrecompensation = 1.0;

  if (tempthre > 138.0 || tempthre < 118.0)
    fPrecompensation = 0.454;

  if (tempthre > 138.0)
    fPrecompensation = (tempthre - 128.0) * 0.046875 + 1.0;

  if (tempthre < 118.0)
    fPrecompensation = 1.0 - 0.007578125 * (128.0 - tempthre);

  buildTable(fPrecompensation);

  mydivisor     = divisor;
  myuseShifting = useShifting;

  for (i = 0; i < matrixWidth; i ++)
  {
    if (matrix[0][i] != 0)
    {
      mystartingOffset = i - 1;
      break;
    }
  }

  mymatrixHeight = matrixHeight;
  mymatrixWidth  = matrixWidth;
  mymatrix       = matrix;

  createTransformedImage(data, width, height, threshold);
}

/*
 * 'buildTable()' - Build the gamma lookup table.
 */

void
buildTable(double fPrecompensation)
{
  int i;
  double f;

  for (i = 0; i < 256; i ++)
  {
    f = pow((double)(((float)i + 0.5f) * 0.00390625f), fPrecompensation);
    g_GammaLUT[i] = (UNIT8)(int)(f * 256.0 - 0.5);
  }
}

/*
 * 'TransformPixel()' - Threshold a gray pixel using the gamma table.
 */

unsigned char
TransformPixel(unsigned char gray, unsigned char threshold)
{
  return ((g_GammaLUT[gray] > threshold) ? 0xff : 0x00);
}

/*
 * 'createTransformedImage()' - Threshold and diffuse the whole image.
 */

void
createTransformedImage(unsigned char *data, unsigned int width,
                       unsigned int height, unsigned char threshold)
{
  unsigned int x, y;
  unsigned int index;
  unsigned char current;
  unsigned char transformed;

  index = 0;

  for (y = 0; y < height; y ++)
  {
    for (x = 0; x < width; x ++)
    {
      current = data[index + x];
      transformed = TransformPixel(current, threshold);
      data[index + x] = transformed;

      diffuse(data, current, transformed, x, y, width, height);
    }

    index += width;
  }
}

/*
 * 'diffuse()' - Diffuse quantization error to neighbouring pixels.
 */

void
diffuse(unsigned char *data, unsigned char ori, unsigned char trans,
        unsigned int x, unsigned int y, unsigned int width,
        unsigned int height)
{
  int grayError;
  unsigned int row;
  unsigned int offsetY;
  unsigned int col;
  unsigned int coefficient;
  unsigned int offsetX;
  unsigned char offsetPixel;
  unsigned int offsetIndex;
  int newGray;
  int mvalue;
  int rowOffset;

  rowOffset = (int)(y * width);

  for (row = y; row - y < mymatrixHeight; row ++)
  {
    for (col = 0; col < mymatrixWidth; col ++)
    {
      offsetX = (x - mystartingOffset) + col;

      if (offsetX < width && row < height && mymatrix[row - y][col] != 0)
      {
        coefficient = mymatrix[row - y][col];
        grayError   = (int)(coefficient *
                            ((unsigned int)ori - (unsigned int)trans));

        if (!myuseShifting)
          grayError = grayError / (int)mydivisor;
        else
          grayError = grayError >> (mydivisor & 0x1f);

        offsetPixel = data[offsetX + (unsigned int)rowOffset];
        mvalue      = grayError + offsetPixel;

        if (mvalue > 255)
          mvalue = 255;

        if (mvalue < 0)
          mvalue = 0;

        data[offsetX + (unsigned int)rowOffset] = (unsigned char)mvalue;
      }
    }

    rowOffset += (int)width;
  }

  (void)offsetY;
  (void)offsetIndex;
  (void)newGray;
}

/*
 * 'ImageBrightnessGray()' - Adjust image brightness.
 */

void
ImageBrightnessGray(int biWidth, int biHeight, unsigned char *srcData,
                    float strength)
{
  int biWidthBytes;
  int brightness;
  unsigned int lookupTable[256];
  int i, j;

  biWidthBytes = biWidth;
  brightness   = (int)(strength - 128.0f);

  for (i = 0; i < 256; i ++)
  {
    double value = sin((3.141592653589793 * (double)i) / 255.0) *
                       (double)brightness +
                   (double)i;
    float f = (float)value;

    if (f < 0.0f)
      lookupTable[i] = 0;
    else if (f > 255.0f)
      lookupTable[i] = 255;
    else
      lookupTable[i] = (unsigned int)(int)f;
  }

  for (i = 0; i < biHeight; i ++)
  {
    for (j = 0; j < biWidth; j ++)
    {
      unsigned int gray = srcData[i * biWidthBytes + j];

      srcData[i * biWidthBytes + j] = (unsigned char)lookupTable[gray];
    }
  }
}

/*
 * 'ImageContrastGray()' - Adjust image contrast.
 */

void
ImageContrastGray(int biWidth, int biHeight, unsigned char *srcData,
                  float strength)
{
  int biWidthBytes;
  int i, j;
  float scale;

  biWidthBytes = biWidth;
  scale = (strength - 128.0f) / 255.0f;

  for (i = 0; i < biHeight; i ++)
  {
    for (j = 0; j < biWidth; j ++)
    {
      unsigned int gray = srcData[i * biWidthBytes + j];
      float value = ((float)gray - 128.0f) * (scale + 1.0f) + 128.0f;

      if (value < 0.0f)
        value = 0.0f;
      else if (value > 255.0f)
        value = 255.0f;

      srcData[i * biWidthBytes + j] = (unsigned char)(int)value;
    }
  }
}
