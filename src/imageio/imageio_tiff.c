/*
    This file is part of darktable,
    Copyright (C) 2010-2025 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "imageio_tiff.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "imageio_common.h"

#include <inttypes.h>
#include <memory.h>
#include <stdio.h>
#include <strings.h>
#include <tiffio.h>
#ifdef HAVE_IMATH
#include "Imath/half.h"
#endif

#define LAB_CONVERSION_PROFILE DT_COLORSPACE_LIN_REC2020

typedef struct tiff_t
{
  TIFF *tiff;
  uint32_t width;
  uint32_t height;
  uint16_t bpp;
  uint16_t spp;
  uint16_t sampleformat;
  uint32_t scanlinesize;
  dt_image_t *image;
  float *mipbuf;
  tdata_t buf;
} tiff_t;

#ifndef HAVE_IMATH
typedef union fp32_t
{
  uint32_t u;
  float f;
} fp32_t;

/* fallback if Imath library implementation w/ intrinsics not available */
static inline float _half_to_float(uint16_t h)
{
  /* see https://en.wikipedia.org/wiki/Half-precision_floating-point_format#Exponent_encoding
     and https://en.wikipedia.org/wiki/Single-precision_floating-point_format#Exponent_encoding */

  /* from https://gist.github.com/rygorous/2156668 */
  static const fp32_t magic = { 113 << 23 };
  static const uint32_t shifted_exp = 0x7c00 << 13; // exponent mask after shift
  fp32_t o;

  o.u = (h & 0x7fff) << 13;     // exponent/mantissa bits
  uint32_t exp = shifted_exp & o.u;   // just the exponent
  o.u += (127 - 15) << 23;        // exponent adjust

  // handle exponent special cases
  if(exp == shifted_exp) // Inf/NaN?
    o.u += (128 - 16) << 23;    // extra exp adjust
  else if(exp == 0) // Zero/Denormal?
  {
    o.u += 1 << 23;             // extra exp adjust
    o.f -= magic.f;             // renormalize
  }

  o.u |= (h & 0x8000) << 16;    // sign bit
  return o.f;
}
#endif

static inline int _read_chunky_8(tiff_t *t, uint16_t photometric)
{
  const gboolean need_invert = (photometric == PHOTOMETRIC_MINISWHITE);

  for(uint32_t row = 0; row < t->height; row++)
  {
    uint8_t *in = ((uint8_t *)t->buf);
    float *out = ((float *)t->mipbuf) + (size_t)4 * row * t->width;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) return -1;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      /* set rgb to first sample from scanline */
      out[0] = need_invert
               ? 1.0f - ((float)in[0]) * (1.0f / 255.0f)
               :        ((float)in[0]) * (1.0f / 255.0f);

      if(t->spp < 3)  // mono, maybe plus alpha channel
      {
        out[1] = out[2] = out[0];
      }
      else
      {
        out[1] = ((float)in[1]) * (1.0f / 255.0f);
        out[2] = ((float)in[2]) * (1.0f / 255.0f);
      }

      out[3] = 0;
    }
  }

  return 1;
}

static inline int _read_chunky_16(tiff_t *t)
{
  for(uint32_t row = 0; row < t->height; row++)
  {
    uint16_t *in = ((uint16_t *)t->buf);
    float *out = ((float *)t->mipbuf) + (size_t)4 * row * t->width;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) return -1;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      out[0] = ((float)in[0]) * (1.0f / 65535.0f);

      if(t->spp < 3)  // mono, maybe plus alpha channel
      {
        out[1] = out[2] = out[0];
      }
      else
      {
        out[1] = ((float)in[1]) * (1.0f / 65535.0f);
        out[2] = ((float)in[2]) * (1.0f / 65535.0f);
      }

      out[3] = 0;
    }
  }

  return 1;
}

static inline int _read_chunky_h(tiff_t *t)
{
  for(uint32_t row = 0; row < t->height; row++)
  {
    uint16_t *in = ((uint16_t *)t->buf);
    float *out = ((float *)t->mipbuf) + (size_t)4 * row * t->width;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) return -1;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
#ifdef HAVE_IMATH
      out[0] = imath_half_to_float(in[0]);
#else
      out[0] = _half_to_float(in[0]);
#endif

      if(t->spp < 3)  // mono, maybe plus alpha channel
      {
        out[1] = out[2] = out[0];
      }
      else
      {
#ifdef HAVE_IMATH
        out[1] = imath_half_to_float(in[1]);
        out[2] = imath_half_to_float(in[2]);
#else
        out[1] = _half_to_float(in[1]);
        out[2] = _half_to_float(in[2]);
#endif
      }

      out[3] = 0;
    }
  }

  return 1;
}

static inline int _read_chunky_f(tiff_t *t)
{
  for(uint32_t row = 0; row < t->height; row++)
  {
    float *in = ((float *)t->buf);
    float *out = ((float *)t->mipbuf) + (size_t)4 * row * t->width;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) return -1;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      out[0] = in[0];

      if(t->spp < 3)  // mono, maybe plus alpha channel
      {
        out[1] = out[2] = out[0];
      }
      else
      {
        out[1] = in[1];
        out[2] = in[2];
      }

      out[3] = 0;
    }
  }

  return 1;
}

static inline int _read_chunky_8_Lab(tiff_t *t, uint16_t photometric)
{
  const cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  const cmsHPROFILE output_profile = dt_colorspaces_get_profile(LAB_CONVERSION_PROFILE, "", DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY)->profile;
  const cmsHTRANSFORM xform = cmsCreateTransform(Lab, TYPE_LabA_FLT, output_profile, TYPE_RGBA_FLT, INTENT_PERCEPTUAL, 0);

  for(uint32_t row = 0; row < t->height; row++)
  {
    uint8_t *in = ((uint8_t *)t->buf);
    float *output = ((float *)t->mipbuf) + (size_t)4 * row * t->width;
    float *out = output;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) goto failed;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      out[0] = ((float)in[0]) * (100.0f/255.0f);

      if(t->spp < 3)  // mono, maybe plus alpha channel
      {
        out[1] = out[2] = 0;
      }
      else
      {
        if(photometric == PHOTOMETRIC_CIELAB)
        {
          out[1] = ((float)((int8_t)in[1]));
          out[2] = ((float)((int8_t)in[2]));
        }
        else // photometric == PHOTOMETRIC_ICCLAB
        {
          out[1] = ((float)(in[1])) - 128.0f;
          out[2] = ((float)(in[2])) - 128.0f;
        }
      }

      out[3] = 0;
    }

    cmsDoTransform(xform, output, output, t->width);
  }

  cmsDeleteTransform(xform);

  return 1;

failed:
  cmsDeleteTransform(xform);
  return -1;
}


static inline int _read_chunky_16_Lab(tiff_t *t, uint16_t photometric)
{
  const cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  const cmsHPROFILE output_profile
      = dt_colorspaces_get_profile(LAB_CONVERSION_PROFILE, "", DT_PROFILE_DIRECTION_ANY)->profile;
  const cmsHTRANSFORM xform
      = cmsCreateTransform(Lab, TYPE_LabA_FLT, output_profile, TYPE_RGBA_FLT, INTENT_PERCEPTUAL, 0);

  // For CIELab, the L* is encoded in 16 bits as an integer range [0, 65535]
  // For ICCLab, the L* is encoded in 16 bits as an integer range [0, 65280]
  // See https://www.alternatiff.com/resources/TIFFphotoshop.pdf for reference
  const float range = (photometric == PHOTOMETRIC_CIELAB) ? 65535.0f : 65280.0f;

  for(uint32_t row = 0; row < t->height; row++)
  {
    uint16_t *in = ((uint16_t *)t->buf);
    float *output = ((float *)t->mipbuf) + (size_t)4 * row * t->width;
    float *out = output;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) goto failed;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      out[0] = ((float)in[0]) * (100.0f/range);

      if(t->spp < 3)  // mono, maybe plus alpha channel
      {
        out[1] = out[2] = 0;
      }
      else
      {
        if(photometric == PHOTOMETRIC_CIELAB)
        {
          out[1] = ((float)((int16_t)in[1])) / 256.0f;
          out[2] = ((float)((int16_t)in[2])) / 256.0f;
        }
        else // photometric == PHOTOMETRIC_ICCLAB
        {
          out[1] = (((float)(in[1])) - 32768.0f) / 256.0f;
          out[2] = (((float)(in[2])) - 32768.0f) / 256.0f;
        }
      }

      out[3] = 0;
    }

    cmsDoTransform(xform, output, output, t->width);
  }

  cmsDeleteTransform(xform);

  return 1;

failed:
  cmsDeleteTransform(xform);
  return -1;
}


static void _warning_error_handler(const char *type,
                                   const char* module,
                                   const char* fmt,
                                   va_list ap)
{
  // We prepend the wall time from the program start to output line
  // to be consistent with dt_print
  fprintf(stderr,
          "%11.4f [tiff_open] %s: %s: ",
          dt_get_wtime() - darktable.start_wtime,
          type,
          module);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

static void _warning_handler(const char* module, const char* fmt, va_list ap)
{
  if(darktable.unmuted & DT_DEBUG_IMAGEIO)
  {
    _warning_error_handler("warning", module, fmt, ap);
  }
}

static void _error_handler(const char* module, const char* fmt, va_list ap)
{
  _warning_error_handler("error", module, fmt, ap);
}

dt_imageio_retval_t dt_imageio_open_tiff(dt_image_t *img,
                                         const char *filename,
                                         dt_mipmap_buffer_t *mbuf)
{
  // Doing this once would be enough, but our imageio reading code is
  // compiled into darktable's core and doesn't have an init routine.
  TIFFSetWarningHandler(_warning_handler);
  TIFFSetErrorHandler(_error_handler);

  char *ext = g_strrstr(filename, ".");
  if(ext && g_ascii_strcasecmp(ext, ".tif") && g_ascii_strcasecmp(ext, ".tiff"))
    return DT_IMAGEIO_UNSUPPORTED_FORMAT;

  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  tiff_t t;
  uint16_t config;
  uint16_t photometric;

  t.image = img;

#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  t.tiff = TIFFOpenW(wfilename, "rb");
  g_free(wfilename);
#else
  t.tiff = TIFFOpen(filename, "rb");
#endif

  if(t.tiff == NULL) return DT_IMAGEIO_LOAD_FAILED;

  // This loader does not implement reading of tiled files, so we explicitly
  // offload them to the fallback (Graphics/Image)Magic loader ASAP instead
  // of going as far as trying to read the scanline and exiting the loader
  // due to TIFFReadScanline failure.
  if(TIFFIsTiled(t.tiff))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[tiff_open] hand over to fallback loader: "
             "tiled TIFF is not supported in '%s'",
             filename);
    TIFFClose(t.tiff);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  TIFFGetField(t.tiff, TIFFTAG_IMAGEWIDTH, &t.width);
  TIFFGetField(t.tiff, TIFFTAG_IMAGELENGTH, &t.height);
  TIFFGetField(t.tiff, TIFFTAG_BITSPERSAMPLE, &t.bpp);
  TIFFGetFieldDefaulted(t.tiff, TIFFTAG_SAMPLESPERPIXEL, &t.spp);
  TIFFGetFieldDefaulted(t.tiff, TIFFTAG_SAMPLEFORMAT, &t.sampleformat);
  TIFFGetField(t.tiff, TIFFTAG_PLANARCONFIG, &config);
  TIFFGetField(t.tiff, TIFFTAG_PHOTOMETRIC, &photometric);

  // Citing the TIFF 6.0 specification for SampleFormat:
  // A reader would typically treat an image with “undefined” data as
  // if the field were not present (i.e. as unsigned integer data).
  if(t.sampleformat == SAMPLEFORMAT_VOID)
    t.sampleformat = SAMPLEFORMAT_UINT;

  if(photometric == PHOTOMETRIC_SEPARATED)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[tiff_open] hand over to fallback loader: "
             "CMYK colorspace not supported in '%s'",
             filename);
    TIFFClose(t.tiff);
    return DT_IMAGEIO_UNSUPPORTED_FORMAT;
  }

  if(photometric == PHOTOMETRIC_PALETTE)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[tiff_open] hand over to fallback loader: "
             "indexed color map (palette) not supported in '%s'",
             filename);
    TIFFClose(t.tiff);
    return DT_IMAGEIO_UNSUPPORTED_FORMAT;
  }

  if(TIFFRasterScanlineSize(t.tiff) != TIFFScanlineSize(t.tiff))
  {
    TIFFClose(t.tiff);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  t.scanlinesize = TIFFScanlineSize(t.tiff);

  dt_print(DT_DEBUG_IMAGEIO,
           "[tiff_open] %dx%d %dbpp, %d samples per pixel",
           t.width, t.height, t.bpp, t.spp);

  // we only support 8, 16 and 32 bits per pixel formats.
  if(t.bpp != 8 && t.bpp != 16 && t.bpp != 32)
  {
    TIFFClose(t.tiff);
    dt_print(DT_DEBUG_ALWAYS,
             "[tiff_open] hand over to fallback loader: "
             "unsupported bit depth other than 8, 16 or 32 in '%s'",
             filename);
    return DT_IMAGEIO_UNSUPPORTED_FORMAT;
  }

  // Planar config is irrelevant if spp == 1
  if(t.spp > 1 && config != PLANARCONFIG_CONTIG)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[tiff_open] hand over to fallback loader: "
             "unsupported non-chunky PlanarConfiguration in '%s'",
             filename);
    TIFFClose(t.tiff);
    return DT_IMAGEIO_UNSUPPORTED_FORMAT;
  }

  /* initialize cached image buffer */
  t.image->width = t.width;
  t.image->height = t.height;

  t.image->buf_dsc.channels = 4;
  t.image->buf_dsc.datatype = TYPE_FLOAT;
  t.image->buf_dsc.cst = IOP_CS_RGB;
  t.image->buf_dsc.filters = 0u;

  t.mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, t.image);
  if(!t.mipbuf)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[tiff_open] error: could not alloc full buffer for '%s'",
             t.image->filename);
    TIFFClose(t.tiff);
    return DT_IMAGEIO_CACHE_FULL;
  }

  if((t.buf = _TIFFmalloc(t.scanlinesize)) == NULL)
  {
    TIFFClose(t.tiff);
    return DT_IMAGEIO_CACHE_FULL;
  }

  // flag the image buffer properly depending on sample format
  if(t.sampleformat == SAMPLEFORMAT_IEEEFP)
  {
    // HDR TIFF
    t.image->flags &= ~DT_IMAGE_LDR;
    t.image->flags |= DT_IMAGE_HDR;
  }
  else
  {
    // LDR TIFF
    t.image->flags |= DT_IMAGE_LDR;
    t.image->flags &= ~DT_IMAGE_HDR;
  }

  int ok = 1;
  dt_imageio_retval_t ret = DT_IMAGEIO_LOAD_FAILED;

  if((photometric == PHOTOMETRIC_CIELAB || photometric == PHOTOMETRIC_ICCLAB)
     && t.bpp == 8
     && t.sampleformat == SAMPLEFORMAT_UINT)
  {
    ok = _read_chunky_8_Lab(&t, photometric);
  }
  else if((photometric == PHOTOMETRIC_CIELAB || photometric == PHOTOMETRIC_ICCLAB)
          && t.bpp == 16
          && t.sampleformat == SAMPLEFORMAT_UINT)
  {
    ok = _read_chunky_16_Lab(&t, photometric);
  }
  else if(t.bpp == 8 && t.sampleformat == SAMPLEFORMAT_UINT)
  {
    ok = _read_chunky_8(&t, photometric);
  }
  else if(t.bpp == 16 && t.sampleformat == SAMPLEFORMAT_UINT)
  {
    ok = _read_chunky_16(&t);
  }
  else if(t.bpp == 16 && t.sampleformat == SAMPLEFORMAT_IEEEFP)
  {
    ok = _read_chunky_h(&t);
  }
  else if(t.bpp == 32 && t.sampleformat == SAMPLEFORMAT_IEEEFP)
  {
    ok = _read_chunky_f(&t);
  }
  else
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[tiff_open] hand over to fallback loader: "
             "unsupported TIFF format feature in '%s'",
             filename);
    ok = 0;
    ret = DT_IMAGEIO_UNSUPPORTED_FORMAT;
  }

  _TIFFfree(t.buf);
  TIFFClose(t.tiff);

  if(ok == 1)
  {
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_S_RAW;
    img->loader = LOADER_TIFF;
    return DT_IMAGEIO_OK;
  }
  else
    return ret;
}

int dt_imageio_tiff_read_profile(const char *filename, uint8_t **out)
{
  TIFFSetWarningHandler(_warning_handler);
  TIFFSetErrorHandler(_error_handler);

  TIFF *tiff = NULL;
  uint32_t profile_len = 0;
  uint8_t *profile = NULL;
  uint16_t photometric;

  if(!(filename && *filename && out)) return 0;

#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  tiff = TIFFOpenW(wfilename, "rb");
  g_free(wfilename);
#else
  tiff = TIFFOpen(filename, "rb");
#endif

  if(tiff == NULL) return 0;

  TIFFGetField(tiff, TIFFTAG_PHOTOMETRIC, &photometric);

  if(photometric == PHOTOMETRIC_CIELAB || photometric == PHOTOMETRIC_ICCLAB)
  {
    profile = dt_colorspaces_get_profile(LAB_CONVERSION_PROFILE, "", DT_PROFILE_DIRECTION_ANY)->profile;

    cmsSaveProfileToMem(profile, 0, &profile_len);
    if(profile_len > 0)
    {
      *out = g_try_malloc(profile_len);
      if(*out)
        cmsSaveProfileToMem(profile, *out, &profile_len);
    }
  }
  else if(TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &profile_len, &profile))
  {
    if(profile_len > 0)
    {
      *out = g_try_malloc(profile_len);
      if(*out)
        memcpy(*out, profile, profile_len);
    }
  }
  else
    profile_len = 0;

  TIFFClose(tiff);

  return profile_len;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
