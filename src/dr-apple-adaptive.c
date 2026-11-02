/* SPDX-License-Identifier: GPL-3.0-or-later AND MIT
 *
 * Apple's 8×8 tile encoding (0x3f3), what a Mac sends a viewer that asks for
 * Screen Sharing's Adaptive quality. A rectangle body is one of:
 * - a full update (0): two precisions, a command stream and a data stream;
 * - a partial update (1): one stream refining the last full update's tiles;
 * - the quantization tables (2), which come before the first picture.
 *
 * Tiles are white, black and white, one or two colours, copies of a
 * neighbour, cached tiles, or JPEG-like DCT blocks: zigzag order, the JPEG
 * reference library's integer inverse transform and its YCbCr, each block at
 * full resolution. This follows the layouts and decoding published by the
 * remotex project (https://github.com/andrewtheguy/remotex,
 * docs/apple-vnc-889.md and frontend/wasm/apple-adaptive, MIT licensed,
 * Copyright (c) 2026 Andrew Chen), down to what Apple's viewer does with a
 * stream that is not what it should be, since the Mac codes each update
 * against what its viewer made of the last.
 *
 * Ported from remotex, under its license:
 *
 * MIT License
 *
 * Copyright (c) 2026 Andrew Chen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "dr-apple-adaptive.h"

#include <gio/gio.h>
#include <string.h>

enum { FULL = 0, PARTIAL = 1, TABLES = 2 };

/* The quantization message: its kind, then 64 luminance and 64 chrominance
 * entries. */
#define TABLES_LEN 129
/* How far past a body's end its streams may be read, as Apple's viewer
 * allows: a stream is read a few bytes ahead of what it spends. */
#define OVERREAD 500
/* The end of both of a full update's streams; a partial update ends "mvs". */
#define MARK 0x6d
static const guint32 partial_marks[3] = { 0x6d, 0x76, 0x73 };

/* Cached refined tiles: indices run from 1, are written up to 64,999 and then
 * from 1 again, and are read up to 65,000. Each keeps 64 Y coefficients, 15 Cr
 * and 20 Cb, in zigzag order. */
#define CACHE_READ_MAX 65000
#define CACHE_WRITE_MAX 64999
#define CACHED_Y 64
#define CACHED_CR 15
#define CACHED_CB 20
#define CACHED (CACHED_Y + CACHED_CR + CACHED_CB)

#define OPAQUE 0xff000000u
#define WHITE 0xffffffffu
#define BLACK OPAQUE
/* What a two-colour tile that reuses its colours draws before any were read:
 * the viewer's own starting values, white and (254, 213, 181). */
#define FIRST_PAIR_0 0xffffffffu
#define FIRST_PAIR_1 0xfffed5b5u

/* JPEG's zigzag order, with entries past the end for a run that reads beyond it. */
static const guint8 natural[80] = {
  0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
  28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54,
  47, 55, 62, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
};

#define FAIL(...) (g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, __VA_ARGS__), FALSE)

/* A stream read most significant bit first, reading zeros past its end. */
typedef struct
{
  const guint8 *data;
  gsize len;
  gsize next;
  guint64 window;
  guint held;
} Bits;

static void
bits_init (Bits *bits, const guint8 *data, gsize len, gsize at)
{
  *bits = (Bits) { .data = data, .len = len, .next = at };
}

/* The next n bits, at most 24. */
static guint32
get (Bits *bits, guint n)
{
  while (bits->held < n)
    {
      guint8 byte = bits->next < bits->len ? bits->data[bits->next] : 0;

      bits->next++;
      bits->window = ((bits->window << 8) | byte) & G_GUINT64_CONSTANT (0xffffffffffff);
      bits->held += 8;
    }
  bits->held -= n;
  return (guint32) ((bits->window >> bits->held) & ((G_GUINT64_CONSTANT (1) << n) - 1));
}

static gboolean
bit (Bits *bits)
{
  return get (bits, 1) == 1;
}

static gboolean
past (const Bits *bits)
{
  return bits->next > bits->len + OVERREAD;
}

/* How many tiles a command covers: a 0 bit is one; a 1 and four bits below 15
 * are that many plus two; 1, 1111 and a number of up to three bytes, least
 * significant seven bits first, are that number plus 17. */
static guint32
tiles_covered (Bits *bits)
{
  guint32 short_count, first, second, third;

  if (!bit (bits))
    return 1;
  short_count = get (bits, 4);
  if (short_count != 15)
    return short_count + 2;
  first = get (bits, 8);
  if (!(first & 0x80))
    return first + 17;
  second = get (bits, 8);
  if (!(second & 0x80))
    return ((second << 7) | (first & 0x7f)) + 17;
  third = get (bits, 8);
  return ((third << 14) | ((second & 0x7f) << 7) | (first & 0x7f)) + 17;
}

/* k one bits and a zero, then for k 0 a bit, which when set is 1 and is
 * followed by a sign; for 1 and 2, 2 or 4 plus one or two bits; and above,
 * 8(k − 2) plus three bits, each followed by a sign, set for negative. */
static gboolean
signed_number (Bits *bits, gint16 *out)
{
  guint32 ones = 0, magnitude;

  while (bit (bits))
    if (++ones == 39)
      return FALSE;

  switch (ones)
    {
    case 0:
      if (!bit (bits))
        {
          *out = 0;
          return TRUE;
        }
      magnitude = 1;
      break;
    case 1:
      magnitude = 2 + get (bits, 1);
      break;
    case 2:
      magnitude = 4 + get (bits, 2);
      break;
    default:
      magnitude = 8 * (ones - 2) + get (bits, 3);
      break;
    }
  *out = (gint16) (bit (bits) ? -(int) magnitude : (int) magnitude);
  return TRUE;
}

/* JPEG's standard Huffman table for luminance AC coefficients, which a
 * refinement's chrominance is coded with. */
static const guint8 ac_luminance_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const guint8 ac_luminance_values[162] = {
  0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
  0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
  0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
  0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
  0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
  0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
  0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
  0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
  0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

typedef struct
{
  int max[17];    /* the largest code of each length, or -1 */
  int offset[17]; /* the first value of each length, less its first code */
} Huffman;

static void
huffman_init (Huffman *huffman)
{
  int code = 0, first = 0;

  for (int len = 1; len <= 16; len++)
    {
      int count = ac_luminance_bits[len - 1];

      huffman->max[len] = -1;
      huffman->offset[len] = 0;
      if (count > 0)
        {
          huffman->offset[len] = first - code;
          code += count;
          huffman->max[len] = code - 1;
          first += count;
        }
      code <<= 1;
    }
}

/* The next symbol, or 0, an end of block, for a code no length has. */
static guint8
huffman_decode (const Huffman *huffman, Bits *bits)
{
  int code = 0;

  for (int len = 1; len <= 16; len++)
    {
      code = (code << 1) | (int) get (bits, 1);
      if (code <= huffman->max[len])
        return ac_luminance_values[code + huffman->offset[len]];
    }
  get (bits, 1);
  return 0;
}

/* The three blocks of a tile, each in natural order. */
typedef struct
{
  gint16 y[64];
  gint16 cb[64];
  gint16 cr[64];
} Block;

/* What the last full update that coded a tile with coefficients left in it,
 * for a partial update to refine. */
typedef struct
{
  guint8 precision;
  gint8 y[64]; /* zigzag order, the DC first */
  gboolean chroma;
  gint8 cr;
  gint8 cb;
} Coded;

typedef struct
{
  Coded coded;
  guint32 update;   /* the full update that last drew the tile */
  gssize copied;    /* the tile it was copied from by that update, or -1 */
  gsize copied_at;  /* where in the framebuffer the copy was taken from */
} Tile;

struct _DrAppleAdaptive
{
  /* The framebuffer of the current call. */
  guint32 *pixels;
  gsize width;
  gsize height;

  gsize tiles_across;
  gsize n_tiles;
  Tile *tiles;
  gboolean have_tables;
  int luminance[64];
  int chrominance[64];
  Huffman huffman;
  guint32 updates; /* full updates read so far */
  gint8 (*cache)[CACHED];
  guint32 cached;
  guint16 written;
  guint16 drawn; /* the index a tile was last drawn from the cache by */
};

typedef struct
{
  gsize x, y, w, h;
} Rect;

/* The tiles of a rectangle, in rows, left to right and top to bottom. */
typedef struct
{
  Rect rect;
  gsize dx, dy;
  gsize index; /* in the framebuffer's tiles */
  gsize row;   /* the index the row starts at */
  gsize across;
} Scan;

static void
scan_init (Scan *scan, Rect rect, gsize across)
{
  gsize index = (rect.y >> 3) * across + (rect.x >> 3);

  *scan = (Scan) { .rect = rect, .index = index, .row = index, .across = across };
}

static gsize scan_x (const Scan *scan) { return scan->rect.x + scan->dx; }
static gsize scan_y (const Scan *scan) { return scan->rect.y + scan->dy; }

/* The tile's top left pixel's place in the framebuffer. */
static gsize
scan_at (const Scan *scan, gsize width)
{
  return scan_y (scan) * width + scan_x (scan);
}

static void
scan_clipped (const Scan *scan, gsize *w, gsize *h)
{
  *w = MIN (scan->rect.w - scan->dx, 8);
  *h = MIN (scan->rect.h - scan->dy, 8);
}

/* On to the next tile. Returns whether the one just done was the last. */
static gboolean
scan_advance (Scan *scan)
{
  scan->dx += 8;
  if (scan->dx < scan->rect.w)
    {
      scan->index++;
      return FALSE;
    }
  scan->dy += 8;
  if (scan->dy >= scan->rect.h)
    return TRUE;
  scan->dx = 0;
  scan->row += scan->across;
  scan->index = scan->row;
  return FALSE;
}

/* JPEG's YCbCr to RGB, with the reference library's fixed-point tables. */
static guint32
ycc (int y, int cb, int cr)
{
  const int one_half = 1 << 15;
  int r, g, b;

  cb -= 128;
  cr -= 128;
  r = CLAMP (y + ((91881 * cr + one_half) >> 16), 0, 255);
  g = CLAMP (y + ((-46802 * cr + -22554 * cb + one_half) >> 16), 0, 255);
  b = CLAMP (y + ((116130 * cb + one_half) >> 16), 0, 255);
  return OPAQUE | (guint32) r << 16 | (guint32) g << 8 | (guint32) b;
}

/* 8-bit Y, then the top six bits of Cb and of Cr. */
static guint32
colour (Bits *data)
{
  int y = (int) get (data, 8);
  int cb = (int) ((get (data, 6) << 2) & 0xff);
  int cr = (int) ((get (data, 6) << 2) & 0xff);

  return ycc (y, cb, cr);
}

/* A row mask, then for each row whose bit is clear a pixel mask: a set bit
 * takes set, a clear one clear, and a row whose bit is set is set throughout. */
static void
masked (Bits *data, guint32 tile[64], guint32 set, guint32 clear)
{
  guint32 rows = get (data, 8);

  for (int row = 0; row < 8; row++)
    {
      guint32 pixels;

      if (rows & (0x80u >> row))
        {
          for (int column = 0; column < 8; column++)
            tile[row * 8 + column] = set;
          continue;
        }
      pixels = get (data, 8);
      for (int column = 0; column < 8; column++)
        tile[row * 8 + column] = (pixels & (0x80u >> column)) ? set : clear;
    }
}

/* The JPEG reference library's integer inverse DCT (jidctint.c), with its
 * output range limit. */
#define CONST_BITS 13
#define PASS1_BITS 2

static gint64
descale (gint64 x, int n)
{
  return (x + (G_GINT64_CONSTANT (1) << (n - 1))) >> n;
}

static void
butterfly (const gint64 v[8], gint64 out[8])
{
  gint64 z1, z2, z3, z4, z5, tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13, t0, t1, t2, t3;

  z2 = v[2];
  z3 = v[6];
  z1 = (z2 + z3) * 4433;
  tmp2 = z1 + z3 * -15137;
  tmp3 = z1 + z2 * 6270;
  tmp0 = (v[0] + v[4]) * (1 << CONST_BITS);
  tmp1 = (v[0] - v[4]) * (1 << CONST_BITS);
  tmp10 = tmp0 + tmp3;
  tmp13 = tmp0 - tmp3;
  tmp11 = tmp1 + tmp2;
  tmp12 = tmp1 - tmp2;

  t0 = v[7];
  t1 = v[5];
  t2 = v[3];
  t3 = v[1];
  z1 = t0 + t3;
  z2 = t1 + t2;
  z3 = t0 + t2;
  z4 = t1 + t3;
  z5 = (z3 + z4) * 9633;
  t0 *= 2446;
  t1 *= 16819;
  t2 *= 25172;
  t3 *= 12299;
  z1 *= -7373;
  z2 *= -20995;
  z3 = z3 * -16069 + z5;
  z4 = z4 * -3196 + z5;
  t0 += z1 + z3;
  t1 += z2 + z4;
  t2 += z2 + z3;
  t3 += z1 + z4;

  out[0] = tmp10 + t3;
  out[1] = tmp11 + t2;
  out[2] = tmp12 + t1;
  out[3] = tmp13 + t0;
  out[4] = tmp13 - t0;
  out[5] = tmp12 - t1;
  out[6] = tmp11 - t2;
  out[7] = tmp10 - t3;
}

/* What a sample past 0–255 becomes: the library's table, read through its
 * ten-bit mask. */
static guint8
range_limit (gint64 x)
{
  guint32 i = (guint32) x & 1023;

  if (i < 128)
    return (guint8) (i + 128);
  if (i < 512)
    return 255;
  if (i < 896)
    return 0;
  return (guint8) (i - 896);
}

static void
idct (const gint16 coefficients[64], const int quantization[64], guint8 samples[64])
{
  gint32 workspace[64];

  for (int column = 0; column < 8; column++)
    {
      gint64 v[8], out[8];
      gboolean ac = FALSE;

      for (int row = 0; row < 8; row++)
        v[row] = (gint64) coefficients[row * 8 + column] * quantization[row * 8 + column];
      for (int row = 1; row < 8; row++)
        ac |= coefficients[row * 8 + column] != 0;
      if (!ac)
        {
          gint32 dc = (gint32) (v[0] * (1 << PASS1_BITS));

          for (int row = 0; row < 8; row++)
            workspace[row * 8 + column] = dc;
          continue;
        }
      butterfly (v, out);
      for (int row = 0; row < 8; row++)
        workspace[row * 8 + column] = (gint32) descale (out[row], CONST_BITS - PASS1_BITS);
    }

  for (int row = 0; row < 8; row++)
    {
      const gint32 *line = workspace + row * 8;
      gint64 v[8], out[8];
      gboolean ac = FALSE;

      for (int i = 1; i < 8; i++)
        ac |= line[i] != 0;
      if (!ac)
        {
          guint8 flat = range_limit (descale (line[0], PASS1_BITS + 3));

          memset (samples + row * 8, flat, 8);
          continue;
        }
      for (int i = 0; i < 8; i++)
        v[i] = line[i];
      butterfly (v, out);
      for (int i = 0; i < 8; i++)
        samples[row * 8 + i] = range_limit (descale (out[i], CONST_BITS + PASS1_BITS + 3));
    }
}

/* The three blocks dequantized, inverted and converted to pixels. */
static void
inverse (const DrAppleAdaptive *self, const Block *block, guint32 tile[64])
{
  guint8 y[64], cb[64], cr[64];

  idct (block->y, self->luminance, y);
  idct (block->cb, self->chrominance, cb);
  idct (block->cr, self->chrominance, cr);
  for (int i = 0; i < 64; i++)
    tile[i] = ycc (y[i], cb[i], cr[i]);
}

/* The tile under scan as the framebuffer has it, its pixels past the
 * rectangle's edge black. */
static void
read_tile (const DrAppleAdaptive *self, const Scan *scan, guint32 tile[64])
{
  gsize w, h;

  for (int i = 0; i < 64; i++)
    tile[i] = BLACK;
  scan_clipped (scan, &w, &h);
  for (gsize row = 0; row < h; row++)
    memcpy (tile + row * 8, self->pixels + (scan_y (scan) + row) * self->width + scan_x (scan), w * 4);
}

/* tile onto the framebuffer, as much of it as the rectangle covers. */
static void
write_tile (DrAppleAdaptive *self, const Scan *scan, const guint32 tile[64])
{
  gsize w, h;

  scan_clipped (scan, &w, &h);
  for (gsize row = 0; row < h; row++)
    memcpy (self->pixels + (scan_y (scan) + row) * self->width + scan_x (scan), tile + row * 8, w * 4);
}

/* Eight rows of eight pixels from at, a pixel's place in the framebuffer read
 * as one run of rows, into tile. A row that starts before the framebuffer is
 * left as it was, and so is a pixel after its end. */
static void
copy_into (const DrAppleAdaptive *self, guint32 tile[64], gssize at)
{
  gsize total = self->width * self->height;

  for (int row = 0; row < 8; row++)
    {
      gssize start = at + row * (gssize) self->width;

      if (start < 0)
        continue;
      for (int column = 0; column < 8; column++)
        if ((gsize) start + column < total)
          tile[row * 8 + column] = self->pixels[start + column];
    }
}

/* The cached tile at entry, where there is one, which is then the one "the
 * next" counts from. */
static void
draw_cached (DrAppleAdaptive *self, guint16 entry, guint32 tile[64])
{
  const gint8 *cached;
  Block block = { 0 };

  if (entry == 0 || entry > CACHE_READ_MAX || entry > self->cached || self->cache == NULL)
    return;
  cached = self->cache[entry];
  for (int k = 0; k < CACHED_Y; k++)
    block.y[natural[k]] = cached[k];
  for (int k = 0; k < CACHED_CR; k++)
    block.cr[natural[k]] = cached[CACHED_Y + k];
  for (int k = 0; k < CACHED_CB; k++)
    block.cb[natural[k]] = cached[CACHED_Y + CACHED_CR + k];
  inverse (self, &block, tile);
  self->drawn = entry;
}

typedef enum
{
  SAVED_NOTHING,
  SAVED_CODED,
  SAVED_LAST, /* the previous DCT tile's record over again */
} Saved;

/* A DCT tile of a full update: the copy bit, the chroma and precision bits,
 * the chroma's DCs where they are sent, Y's DC, then Y's AC coefficients. */
static gboolean
expand_block (Bits *data, const Block *previous, const guint8 precisions[2], const Tile *last,
              Block *block, Saved *saved, Coded *coded)
{
  guint8 precision;
  gboolean fine;
  gint16 number;
  int k = 1;

  memset (block, 0, sizeof *block);
  if (bit (data))
    {
      if (last == NULL)
        {
          *saved = SAVED_NOTHING;
          return TRUE;
        }
      memcpy (block->y, previous->y, sizeof block->y);
      block->cb[0] = previous->cb[0];
      block->cr[0] = previous->cr[0];
      *saved = SAVED_LAST;
      return TRUE;
    }

  {
    gboolean keep = bit (data);

    precision = bit (data) ? precisions[1] : precisions[0];
    if (keep)
      {
        block->cb[0] = previous->cb[0];
        block->cr[0] = previous->cr[0];
      }
    else
      {
        /* Half the previous DC, rounded towards zero, less the number, doubled. */
        if (!signed_number (data, &number))
          return FALSE;
        block->cb[0] = (gint16) ((previous->cb[0] / 2 - number) * 2);
        if (!signed_number (data, &number))
          return FALSE;
        block->cr[0] = (gint16) ((previous->cr[0] / 2 - number) * 2);
      }
    if (!signed_number (data, &number))
      return FALSE;
    block->y[0] = (gint16) (previous->y[0] - number);
  }

  fine = precision >= 15;
  *saved = SAVED_CODED;
  while (k < 64)
    {
      gboolean early = k < 6;
      gboolean below = k < precision;
      /* A coefficient is sent short of its low bits: a unit is 1 shifted left
       * by that many. */
      int shift = fine ? (below ? 1 : 3) : (below ? 3 : 4);

      if (bit (data))
        {
          guint32 ones = 0;
          int magnitude;
          gboolean stop = FALSE;

          while (bit (data))
            {
              ones++;
              if (early && ones == 33)
                return FALSE;
              if (!early && ones == 19)
                {
                  /* The viewer keeps what it has and leaves the record as it was. */
                  stop = TRUE;
                  break;
                }
            }
          if (stop)
            {
              *saved = SAVED_NOTHING;
              break;
            }
          if (early)
            magnitude = ones < 4 ? 2 + 4 * (int) ones + (int) get (data, 2)
                                 : 18 + 8 * ((int) ones - 4) + (int) get (data, 3);
          else if (ones == 0)
            magnitude = 2 + (int) get (data, 1);
          else if (ones == 1)
            magnitude = 4 + (int) get (data, 2);
          else
            magnitude = 8 * ((int) ones - 1) + (int) get (data, 3);
          if (bit (data))
            magnitude = -magnitude;
          block->y[natural[k]] = (gint16) (guint32) ((guint32) magnitude << shift);
          k++;
          continue;
        }

      switch (get (data, 2))
        {
        case 0:
          k++;
          break;
        case 2:
          block->y[natural[k++]] = (gint16) (1 << shift);
          break;
        case 3:
          block->y[natural[k++]] = (gint16) -(1 << shift);
          break;
        default:
          {
            guint32 r, run;

            if (!bit (data))
              goto done;
            r = get (data, 2);
            run = r + 3;
            if (r == 3)
              {
                for (;;)
                  {
                    guint32 more = get (data, 3);

                    run += more;
                    if (more != 7 || k + (int) (run & 0xff) >= 64)
                      break;
                  }
                if (k + (int) (run & 0xff) > 63)
                  return FALSE;
                k += (int) (run & 0xff);
              }
            else
              k += (int) run;
          }
        }
    }
done:
  if (*saved == SAVED_CODED)
    {
      coded->precision = precision;
      for (int i = 0; i < 64; i++)
        coded->y[i] = (gint8) block->y[natural[i]];
      coded->chroma = TRUE;
      coded->cr = (gint8) block->cr[0];
      coded->cb = (gint8) block->cb[0];
    }
  return TRUE;
}

/* A refined tile, in zigzag order as the cache keeps it, and whole chroma blocks. */
typedef struct
{
  gint8 y[CACHED_Y];
  gint16 cr_block[64];
  gint16 cb_block[64];
} Refined;

/* Moved further from zero by n more bits. */
static gint8
further (Bits *bits, gint8 coefficient, guint n)
{
  gint8 more = (gint8) get (bits, n);

  return (gint8) (coefficient + (coefficient < 0 ? -more : more));
}

static gboolean
refine_chroma (Bits *bits, const Huffman *huffman, gint8 dc, guint8 count, gint16 block[64])
{
  gboolean set = bit (bits);
  int k = 0;

  memset (block, 0, 64 * sizeof *block);
  if (dc == 0)
    block[0] = !set ? 0 : (bit (bits) ? -1 : 1);
  else
    block[0] = (gint8) (dc + (dc < 0 ? -(int) set : (int) set));

  while (k < count)
    {
      guint8 symbol = huffman_decode (huffman, bits);
      int run = symbol >> 4;
      guint size = symbol & 15;

      if (size == 0)
        {
          if (run != 15)
            break;
          k += 15;
        }
      else
        {
          int value;

          k += run;
          value = (int) get (bits, size);
          if (value < (1 << (size - 1)))
            value += 1 - (1 << size);
          block[natural[k + 1]] = (gint16) value;
        }
      k++;
    }
  return TRUE;
}

/* A partial update's code 1: the bits the full update left out of a tile's Y
 * coefficients, and its chrominance as JPEG codes it. */
static gboolean
refine (Bits *bits, const Coded *coded, guint8 cr_count, guint8 cb_count, const Huffman *huffman,
        Refined *refined)
{
  gsize precision = coded->precision;
  gsize count = get (bits, 6) + 1;
  gsize below = MIN (count, precision);
  gint16 number;

  if (!coded->chroma)
    return FALSE;

  memset (refined->y, 0, sizeof refined->y);
  refined->y[0] = coded->y[0];
  for (gsize k = 1; k < below; k++)
    {
      gint8 coefficient = coded->y[k];

      if (precision < 15)
        {
          if (coefficient != 0)
            refined->y[k] = further (bits, coefficient, 3);
          else if (!signed_number (bits, &number))
            return FALSE;
          else
            refined->y[k] = (gint8) number;
        }
      else if (coefficient == 0)
        /* Below the precision of a fine tile a zero is near enough: a bit, and
         * when it is set a sign for 1 or −1. */
        refined->y[k] = !bit (bits) ? 0 : (bit (bits) ? -1 : 1);
      else
        refined->y[k] = further (bits, coefficient, 1);
    }
  for (gsize k = MAX (precision, 1); k < count; k++)
    {
      gint8 coefficient = coded->y[k];

      if (coefficient != 0)
        refined->y[k] = further (bits, coefficient, precision < 15 ? 4 : 3);
      else if (!signed_number (bits, &number))
        return FALSE;
      else
        refined->y[k] = (gint8) number;
    }

  return refine_chroma (bits, huffman, coded->cr, cr_count, refined->cr_block)
         && refine_chroma (bits, huffman, coded->cb, cb_count, refined->cb_block);
}

static void
cache_entry (DrAppleAdaptive *self, const Refined *refined)
{
  gint8 *slot;

  if (self->cache == NULL)
    self->cache = g_malloc0 ((CACHE_READ_MAX + 1) * sizeof *self->cache);
  self->cached++;
  self->written = self->written == CACHE_WRITE_MAX ? 1 : self->written + 1;
  slot = self->cache[self->written];
  memcpy (slot, refined->y, CACHED_Y);
  for (int k = 0; k < CACHED_CR; k++)
    slot[CACHED_Y + k] = (gint8) refined->cr_block[natural[k]];
  for (int k = 0; k < CACHED_CB; k++)
    slot[CACHED_Y + CACHED_CR + k] = (gint8) refined->cb_block[natural[k]];
}

static gboolean
full (DrAppleAdaptive *self, Rect rect, const guint8 *body, gsize len, GError **error)
{
  guint8 precisions[2];
  gsize offset;
  Bits commands, data;
  Scan scan;
  guint32 single = BLACK, pair[2] = { FIRST_PAIR_0, FIRST_PAIR_1 };
  Block previous = { 0 };
  gssize last_coded = -1;
  gsize before;

  if (len < 6)
    return FAIL ("A full update in %zu bytes", len);
  if (!self->have_tables)
    return FAIL ("A picture before the quantization tables");
  precisions[0] = body[1];
  precisions[1] = body[2];
  offset = (gsize) body[3] << 16 | (gsize) body[4] << 8 | body[5];
  if (offset > len)
    return FAIL ("A data stream at %zu of a %zu-byte body", offset, len);
  bits_init (&commands, body, len, 6);
  bits_init (&data, body, len, offset);
  get (&commands, 1);
  self->updates++;

  scan_init (&scan, rect, self->tiles_across);
  /* The tile before this one, which "a copy of the previous tile" copies, and
   * at the update's first tile the tile itself. */
  before = scan_at (&scan, self->width);
  for (;;)
    {
      guint32 command, covered;

      if (past (&commands))
        return FAIL ("An update that ran past its body");
      command = get (&commands, 3);
      covered = tiles_covered (&commands);
      for (guint32 i = 0; i < covered; i++)
        {
          gsize index = scan.index;
          gsize here = scan_at (&scan, self->width);
          guint32 drawn[64];
          Tile *tile;

          if (index >= self->n_tiles)
            return FAIL ("A tile outside the framebuffer");
          tile = &self->tiles[index];
          tile->update = self->updates;
          tile->copied = -1;
          read_tile (self, &scan, drawn);

          switch (command)
            {
            case 0:
              for (int p = 0; p < 64; p++)
                drawn[p] = WHITE;
              break;

            case 1:
              if (index >= 1)
                {
                  tile->copied = (gssize) index - 1;
                  tile->copied_at = before;
                }
              copy_into (self, drawn, (gssize) before);
              break;

            case 2:
              {
                gssize above = (gssize) here - 8 * (gssize) self->width;

                if (index >= self->tiles_across)
                  {
                    tile->copied = (gssize) (index - self->tiles_across);
                    tile->copied_at = (gsize) MAX (above, 0);
                  }
                copy_into (self, drawn, above);
              }
              break;

            case 3:
              masked (&data, drawn, WHITE, BLACK);
              break;

            case 4:
              {
                gboolean two = bit (&data);
                gboolean reuse = bit (&data);

                if (two)
                  {
                    if (!reuse)
                      {
                        pair[0] = colour (&data);
                        pair[1] = colour (&data);
                      }
                    masked (&data, drawn, pair[0], pair[1]);
                  }
                else
                  {
                    if (!reuse)
                      single = colour (&data);
                    for (int p = 0; p < 64; p++)
                      drawn[p] = single;
                  }
              }
              break;

            case 5:
              {
                Block block;
                Saved saved;
                Coded coded;
                Tile last = { 0 };

                if (last_coded >= 0)
                  last = self->tiles[last_coded];
                if (!expand_block (&data, &previous, precisions, last_coded >= 0 ? &last : NULL,
                                   &block, &saved, &coded))
                  return FAIL ("A tile's coefficients that do not read");
                if (saved == SAVED_CODED)
                  tile->coded = coded;
                else if (saved == SAVED_LAST)
                  *tile = last;
                previous = block;
                last_coded = (gssize) index;
                inverse (self, &block, drawn);
              }
              break;

            case 6:
              draw_cached (self, (guint16) get (&data, 16), drawn);
              break;

            default:
              draw_cached (self, (guint16) (self->drawn + 1), drawn);
              break;
            }

          write_tile (self, &scan, drawn);
          before = here;
          if (scan_advance (&scan))
            {
              guint32 got;

              if ((got = get (&commands, 8)) != MARK || (got = get (&data, 8)) != MARK)
                return FAIL ("An update that ended on 0x%02x", got);
              return TRUE;
            }
        }
      if (past (&data))
        return FAIL ("An update that ran past its body");
    }
}

static gboolean
partial (DrAppleAdaptive *self, Rect rect, const guint8 *body, gsize len, GError **error)
{
  guint8 cr_count, cb_count;
  Bits bits;
  Scan scan;

  if (len < 3)
    return FAIL ("A partial update in %zu bytes", len);
  if (!self->have_tables)
    return FAIL ("A picture before the quantization tables");
  cr_count = MIN (body[1], 64);
  cb_count = MIN (body[2], 64);
  bits_init (&bits, body, len, 3);
  scan_init (&scan, rect, self->tiles_across);
  for (;;)
    {
      gsize index = scan.index;
      guint32 drawn[64];
      Tile tile;

      if (index >= self->n_tiles)
        return FAIL ("A tile outside the framebuffer");
      tile = self->tiles[index];

      switch (get (&bits, 2))
        {
        case 0:
          break;

        case 1:
          {
            Refined refined;
            Block block = { 0 };

            read_tile (self, &scan, drawn);
            if (!refine (&bits, &tile.coded, cr_count, cb_count, &self->huffman, &refined))
              return FAIL ("A refinement of a tile that does not read");
            for (int k = 0; k < 64; k++)
              block.y[natural[k]] = refined.y[k];
            memcpy (block.cr, refined.cr_block, sizeof block.cr);
            memcpy (block.cb, refined.cb_block, sizeof block.cb);
            inverse (self, &block, drawn);
            cache_entry (self, &refined);
            write_tile (self, &scan, drawn);
          }
          break;

        case 2:
          /* The copy the full update made, while the tile it copied is still
           * that update's. */
          if (tile.copied >= 0 && (gsize) tile.copied < self->n_tiles
              && self->tiles[tile.copied].update == tile.update)
            {
              read_tile (self, &scan, drawn);
              copy_into (self, drawn, (gssize) tile.copied_at);
              write_tile (self, &scan, drawn);
            }
          break;

        default:
          {
            guint16 entry = bit (&bits) ? (guint16) (self->drawn + 1) : (guint16) get (&bits, 16);

            read_tile (self, &scan, drawn);
            draw_cached (self, entry, drawn);
            write_tile (self, &scan, drawn);
          }
          break;
        }

      if (scan_advance (&scan))
        {
          for (int i = 0; i < 3; i++)
            {
              guint32 got = get (&bits, 8);

              if (got != partial_marks[i])
                return FAIL ("A refinement that ended on 0x%02x", got);
            }
          return TRUE;
        }
      if (past (&bits))
        return FAIL ("A refinement that ran past its body");
    }
}

DrAppleAdaptive *
dr_apple_adaptive_new (void)
{
  DrAppleAdaptive *self = g_new0 (DrAppleAdaptive, 1);

  huffman_init (&self->huffman);
  return self;
}

void
dr_apple_adaptive_free (DrAppleAdaptive *self)
{
  if (self == NULL)
    return;
  g_free (self->tiles);
  g_free (self->cache);
  g_free (self);
}

/* A framebuffer of a new size, with no tile coded. The tables and the cache
 * outlive it, as the Mac's do. */
static void
resize (DrAppleAdaptive *self, gsize width, gsize height)
{
  self->width = width;
  self->height = height;
  self->tiles_across = (width + 7) / 8;
  self->n_tiles = self->tiles_across * ((height + 7) / 8);
  g_free (self->tiles);
  self->tiles = g_new0 (Tile, self->n_tiles);
  for (gsize i = 0; i < self->n_tiles; i++)
    self->tiles[i].copied = -1;
}

gboolean
dr_apple_adaptive_decode (DrAppleAdaptive *self, guint32 *pixels, int width, int height,
                          int x, int y, int w, int h, const guint8 *body, gsize len, GError **error)
{
  Rect rect;
  gboolean ok;

  g_return_val_if_fail (width >= 0 && height >= 0, FALSE);

  if (len == 0)
    return FAIL ("An empty rectangle body");

  if (body[0] == TABLES)
    {
      if (len != TABLES_LEN)
        return FAIL ("Quantization tables in %zu bytes", len);
      for (int i = 0; i < 64; i++)
        {
          self->luminance[i] = body[1 + i];
          self->chrominance[i] = body[65 + i];
        }
      self->have_tables = TRUE;
      return TRUE;
    }
  if (body[0] != FULL && body[0] != PARTIAL)
    return FAIL ("A body of kind %u", body[0]);

  if ((gsize) width != self->width || (gsize) height != self->height || self->tiles == NULL)
    resize (self, width, height);
  if (x < 0 || y < 0 || w < 0 || h < 0 || x >= width || x + w > width || y >= height || y + h > height)
    return FAIL ("A rectangle %dx%d at %d,%d on a %dx%d framebuffer", w, h, x, y, width, height);

  rect = (Rect) { x, y, w, h };
  self->pixels = pixels;
  ok = body[0] == FULL ? full (self, rect, body, len, error) : partial (self, rect, body, len, error);
  self->pixels = NULL;
  return ok;
}
