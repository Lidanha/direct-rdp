/* SPDX-License-Identifier: GPL-3.0-or-later AND MIT
 *
 * Tests for Apple's 8×8 tile decoder, after the remotex project's own
 * (frontend/wasm/apple-adaptive, MIT licensed).
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
#include "../src/dr-apple-adaptive.c"

/* A stream written most significant bit first, padded with zeros. */
typedef struct
{
  GByteArray *bytes;
  guint64 window;
  guint held;
} Writer;

static Writer *
writer_new (void)
{
  Writer *w = g_new0 (Writer, 1);

  w->bytes = g_byte_array_new ();
  return w;
}

static Writer *
put (Writer *w, guint32 value, guint n)
{
  w->window = (w->window << n) | (value & ((G_GUINT64_CONSTANT (1) << n) - 1));
  w->held += n;
  while (w->held >= 8)
    {
      guint8 byte;

      w->held -= 8;
      byte = (guint8) (w->window >> w->held);
      g_byte_array_append (w->bytes, &byte, 1);
    }
  return w;
}

static Writer *
put_bits (Writer *w, const char *text)
{
  for (; *text; text++)
    if (*text == '0' || *text == '1')
      put (w, *text == '1', 1);
  return w;
}

/* The bytes written, padded; frees the writer. */
static GByteArray *
done (Writer *w)
{
  GByteArray *bytes;

  if (w->held > 0)
    put (w, 0, 8 - w->held);
  bytes = w->bytes;
  g_free (w);
  return bytes;
}

/* Tables of ones, so a coefficient is its own dequantized value. */
static const guint8 *
tables (void)
{
  static guint8 body[TABLES_LEN];

  body[0] = TABLES;
  memset (body + 1, 1, 128);
  return body;
}

/* A full update of commands and data, each ended with its mark, at the coarse
 * precisions a Mac starts with. */
static GByteArray *
full_body (Writer *commands, Writer *data)
{
  g_autoptr (GByteArray) c = NULL;
  g_autoptr (GByteArray) d = NULL;
  GByteArray *body = g_byte_array_new ();
  gsize offset;
  guint8 header[6];

  put (commands, MARK, 8);
  put (data, MARK, 8);
  c = done (commands);
  d = done (data);
  offset = 6 + c->len;
  header[0] = FULL;
  header[1] = 3;
  header[2] = 5;
  header[3] = (guint8) (offset >> 16);
  header[4] = (guint8) (offset >> 8);
  header[5] = (guint8) offset;
  g_byte_array_append (body, header, 6);
  g_byte_array_append (body, c->data, c->len);
  g_byte_array_append (body, d->data, d->len);
  return body;
}

typedef struct
{
  DrAppleAdaptive *decoder;
  guint32 *pixels;
  int width, height;
} Fixture;

static Fixture *
fixture_new (int width, int height, gboolean with_tables)
{
  Fixture *f = g_new0 (Fixture, 1);

  f->decoder = dr_apple_adaptive_new ();
  f->width = width;
  f->height = height;
  f->pixels = g_new (guint32, width * height);
  for (int i = 0; i < width * height; i++)
    f->pixels[i] = BLACK;
  if (with_tables)
    g_assert_true (dr_apple_adaptive_decode (f->decoder, f->pixels, width, height, 0, 0, 0, 0,
                                             tables (), TABLES_LEN, NULL));
  return f;
}

static void
fixture_free (Fixture *f)
{
  dr_apple_adaptive_free (f->decoder);
  g_free (f->pixels);
  g_free (f);
}

static gboolean
decode (Fixture *f, int x, int y, int w, int h, GByteArray *body, GError **error)
{
  gboolean ok = dr_apple_adaptive_decode (f->decoder, f->pixels, f->width, f->height, x, y, w, h,
                                          body->data, body->len, error);

  g_byte_array_unref (body);
  return ok;
}

static gboolean
tile_is (Fixture *f, int x, int y, guint32 want)
{
  for (int i = 0; i < 64; i++)
    if (f->pixels[(y + i / 8) * f->width + x + i % 8] != want)
      return FALSE;
  return TRUE;
}

static guint32
rgb (guint8 r, guint8 g, guint8 b)
{
  return OPAQUE | (guint32) r << 16 | (guint32) g << 8 | b;
}

/* A command, after the one bit the stream starts with, covering one tile. */
static Writer *
one (guint32 command)
{
  Writer *w = writer_new ();

  put_bits (w, "0");
  put (w, command, 3);
  put_bits (w, "0");
  return w;
}

static void
test_white (void)
{
  Fixture *f = fixture_new (16, 8, TRUE);

  g_assert_true (decode (f, 0, 0, 8, 8, full_body (one (0), writer_new ()), NULL));
  g_assert_true (tile_is (f, 0, 0, WHITE));
  g_assert_true (tile_is (f, 8, 0, BLACK));
  fixture_free (f);
}

static void
test_repeat_clips (void)
{
  Fixture *f = fixture_new (24, 16, TRUE);
  Writer *commands = writer_new ();

  /* 1, four bits of 3: five tiles, which a second command of one finishes. */
  put_bits (commands, "0");
  put (commands, 0, 3);
  put_bits (commands, "1 0011");
  put (commands, 0, 3);
  put_bits (commands, "0");
  g_assert_true (decode (f, 0, 0, 20, 12, full_body (commands, writer_new ()), NULL));
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 24; x++)
      g_assert_cmphex (f->pixels[y * 24 + x], ==, x < 20 && y < 12 ? WHITE : BLACK);
  fixture_free (f);
}

static void
test_long_repeat (void)
{
  Writer *w = writer_new ();
  g_autoptr (GByteArray) bytes = NULL;
  Bits bits;

  put_bits (w, "0 1 0011 1 1111");
  put (w, 3, 8);
  put_bits (w, "1 1111");
  put (w, 0x80 | 5, 8);
  put (w, 2, 8);
  bytes = done (w);
  bits_init (&bits, bytes->data, bytes->len, 0);
  g_assert_cmpuint (tiles_covered (&bits), ==, 1);
  g_assert_cmpuint (tiles_covered (&bits), ==, 5);
  g_assert_cmpuint (tiles_covered (&bits), ==, 3 + 17);
  g_assert_cmpuint (tiles_covered (&bits), ==, ((2 << 7) | 5) + 17);
}

static void
test_black_and_white (void)
{
  Fixture *f = fixture_new (8, 8, TRUE);
  Writer *data = writer_new ();

  /* Every row but the first is white; the first is its pixel mask. */
  put (data, 0x7f, 8);
  put (data, 0x81, 8);
  g_assert_true (decode (f, 0, 0, 8, 8, full_body (one (3), data), NULL));
  g_assert_cmphex (f->pixels[0], ==, WHITE);
  for (int x = 1; x < 7; x++)
    g_assert_cmphex (f->pixels[x], ==, BLACK);
  g_assert_cmphex (f->pixels[7], ==, WHITE);
  for (int i = 8; i < 64; i++)
    g_assert_cmphex (f->pixels[i], ==, WHITE);
  fixture_free (f);
}

static void
test_colours (void)
{
  Fixture *f = fixture_new (32, 8, TRUE);
  Writer *commands = writer_new ();
  Writer *data = writer_new ();
  guint32 grey, red, blue;

  /* Four tiles of the one command. */
  put_bits (commands, "0");
  put (commands, 4, 3);
  put_bits (commands, "1 0010");
  /* One colour: Y 76, Cb and Cr at the centre, a grey. */
  put_bits (data, "0 0");
  put (data, 76, 8);
  put (data, 32, 6);
  put (data, 32, 6);
  /* Two, red and blue, every row their mask. */
  put_bits (data, "1 0");
  put (data, 76, 8);
  put (data, 21, 6);
  put (data, 63, 6);
  put (data, 29, 8);
  put (data, 63, 6);
  put (data, 26, 6);
  put (data, 0, 8);
  for (int i = 0; i < 8; i++)
    put (data, 0xf0, 8);
  /* The one colour again without reading it, and the two again. */
  put_bits (data, "0 1");
  put_bits (data, "1 1");
  put (data, 0xff, 8);
  g_assert_true (decode (f, 0, 0, 32, 8, full_body (commands, data), NULL));

  grey = ycc (76, 128, 128);
  g_assert_cmphex (grey, ==, rgb (76, 76, 76));
  g_assert_true (tile_is (f, 0, 0, grey));
  red = ycc (76, 84, 252);
  blue = ycc (29, 252, 104);
  for (int x = 0; x < 8; x++)
    g_assert_cmphex (f->pixels[8 + x], ==, x < 4 ? red : blue);
  g_assert_true (tile_is (f, 16, 0, grey));
  g_assert_true (tile_is (f, 24, 0, red));
  fixture_free (f);
}

static void
test_copies (void)
{
  Fixture *f = fixture_new (16, 16, TRUE);
  Writer *commands = writer_new ();
  Writer *data = writer_new ();

  /* White, a copy of it to the right; then a copy from above, and black. */
  put_bits (commands, "0");
  for (guint32 c = 0; c < 4; c++)
    {
      put (commands, c, 3);
      put_bits (commands, "0");
    }
  for (int i = 0; i < 9; i++)
    put (data, 0, 8);
  g_assert_true (decode (f, 0, 0, 16, 16, full_body (commands, data), NULL));
  g_assert_true (tile_is (f, 8, 0, WHITE));
  g_assert_true (tile_is (f, 0, 8, WHITE));
  g_assert_true (tile_is (f, 8, 8, BLACK));
  fixture_free (f);
}

/* A DCT tile whose Y DC is −8 and the rest nothing: a flat 127 grey through
 * the reference transform. */
static void
flat_tile (Writer *data)
{
  put_bits (data, "0 0 0 00 00");
  put_bits (data, "1110 000 0");
  put_bits (data, "0 01 0");
}

static void
test_dct (void)
{
  Fixture *f = fixture_new (8, 8, TRUE);
  Writer *data = writer_new ();

  flat_tile (data);
  g_assert_true (decode (f, 0, 0, 8, 8, full_body (one (5), data), NULL));
  g_assert_true (tile_is (f, 0, 0, rgb (127, 127, 127)));
  fixture_free (f);
}

static GByteArray *
partial_body (Writer *w)
{
  GByteArray *body = g_byte_array_new ();
  g_autoptr (GByteArray) bits = done (w);
  const guint8 header[3] = { PARTIAL, 14, 19 };

  g_byte_array_append (body, header, 3);
  g_byte_array_append (body, bits->data, bits->len);
  return body;
}

static void
test_refine_and_cache (void)
{
  Fixture *f = fixture_new (16, 8, TRUE);
  Writer *commands = writer_new ();
  Writer *data = writer_new ();
  Writer *refinement = writer_new ();

  put_bits (commands, "0");
  put (commands, 5, 3);
  put_bits (commands, "0");
  put (commands, 0, 3);
  put_bits (commands, "0");
  flat_tile (data);
  g_assert_true (decode (f, 0, 0, 16, 8, full_body (commands, data), NULL));

  /* One Y coefficient, the DC; each chroma DC left at 0 and its AC an end of block. */
  put_bits (refinement, "01");
  put (refinement, 0, 6);
  put_bits (refinement, "0 1010 0 1010");
  put (refinement, 0x6d, 8);
  put (refinement, 0x76, 8);
  put (refinement, 0x73, 8);
  g_assert_true (decode (f, 0, 0, 8, 8, partial_body (refinement), NULL));
  g_assert_true (tile_is (f, 0, 0, rgb (127, 127, 127)));
  g_assert_true (tile_is (f, 8, 0, WHITE));

  /* The next cached tile after none drawn yet is the first: the refined one. */
  g_assert_true (decode (f, 8, 0, 8, 8, full_body (one (7), writer_new ()), NULL));
  g_assert_true (tile_is (f, 8, 0, rgb (127, 127, 127)));
  fixture_free (f);
}

static void
test_unrefinable (void)
{
  Fixture *f = fixture_new (8, 8, TRUE);
  Writer *refinement = writer_new ();
  g_autoptr (GError) error = NULL;

  g_assert_true (decode (f, 0, 0, 8, 8, full_body (one (0), writer_new ()), NULL));
  put_bits (refinement, "01");
  put (refinement, 0, 6);
  put_bits (refinement, "0 1010 0 1010");
  g_assert_false (decode (f, 0, 0, 8, 8, partial_body (refinement), &error));
  g_assert_nonnull (error);
  fixture_free (f);
}

static void
test_partial_copy (void)
{
  Fixture *f = fixture_new (16, 8, TRUE);
  Writer *commands = writer_new ();
  Writer *refinement = writer_new ();

  put_bits (commands, "0");
  put (commands, 0, 3);
  put_bits (commands, "0");
  put (commands, 1, 3);
  put_bits (commands, "0");
  g_assert_true (decode (f, 0, 0, 16, 8, full_body (commands, writer_new ()), NULL));
  /* Something else over the copy, then the partial update's copy again. */
  f->pixels[8] = BLACK;
  f->pixels[0] = rgb (1, 2, 3);
  put_bits (refinement, "00 10");
  put (refinement, 0x6d, 8);
  put (refinement, 0x76, 8);
  put (refinement, 0x73, 8);
  g_assert_true (decode (f, 0, 0, 16, 8, partial_body (refinement), NULL));
  g_assert_cmphex (f->pixels[8], ==, rgb (1, 2, 3));
  fixture_free (f);
}

static void
test_refused (void)
{
  Fixture *f = fixture_new (8, 8, FALSE);
  GByteArray *body = full_body (one (0), writer_new ());
  GByteArray *cut = g_byte_array_new ();
  const guint8 short_tables[2] = { TABLES, 1 };
  const guint8 kind[1] = { 7 };

  g_byte_array_append (cut, body->data, body->len);
  cut->data[cut->len - 1] = 0;

  g_assert_false (decode (f, 0, 0, 8, 8, g_byte_array_ref (body), NULL));
  g_assert_true (dr_apple_adaptive_decode (f->decoder, f->pixels, 8, 8, 0, 0, 0, 0, tables (), TABLES_LEN, NULL));
  g_assert_false (decode (f, 0, 0, 16, 8, g_byte_array_ref (body), NULL));
  g_assert_true (decode (f, 0, 0, 8, 8, body, NULL));
  g_assert_false (decode (f, 0, 0, 8, 8, cut, NULL));
  g_assert_false (dr_apple_adaptive_decode (f->decoder, f->pixels, 8, 8, 0, 0, 0, 0, short_tables, 2, NULL));
  g_assert_false (dr_apple_adaptive_decode (f->decoder, f->pixels, 8, 8, 0, 0, 8, 8, kind, 1, NULL));
  fixture_free (f);
}

static void
test_idct (void)
{
  gint16 coefficients[64] = { 0 };
  int ones[64];
  guint8 samples[64];

  for (int i = 0; i < 64; i++)
    ones[i] = 1;

  /* A DC alone is the mean, the transform's rounding on top. */
  coefficients[0] = 80;
  idct (coefficients, ones, samples);
  for (int i = 0; i < 64; i++)
    g_assert_cmpuint (samples[i], ==, 138);

  /* One horizontal frequency: every row the same, odd about the centre. */
  memset (coefficients, 0, sizeof coefficients);
  coefficients[1] = 100;
  idct (coefficients, ones, samples);
  for (int row = 0; row < 8; row++)
    {
      g_assert_cmpmem (samples + row * 8, 8, samples, 8);
      for (int x = 0; x < 4; x++)
        g_assert_cmpint ((int) samples[row * 8 + x] - 128, ==, 128 - (int) samples[row * 8 + 7 - x]);
      g_assert_cmpuint (samples[row * 8], >, samples[row * 8 + 3]);
      g_assert_cmpuint (samples[row * 8 + 3], >, 128);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/apple-adaptive/white", test_white);
  g_test_add_func ("/apple-adaptive/repeat-clips", test_repeat_clips);
  g_test_add_func ("/apple-adaptive/long-repeat", test_long_repeat);
  g_test_add_func ("/apple-adaptive/black-and-white", test_black_and_white);
  g_test_add_func ("/apple-adaptive/colours", test_colours);
  g_test_add_func ("/apple-adaptive/copies", test_copies);
  g_test_add_func ("/apple-adaptive/dct", test_dct);
  g_test_add_func ("/apple-adaptive/refine-and-cache", test_refine_and_cache);
  g_test_add_func ("/apple-adaptive/unrefinable", test_unrefinable);
  g_test_add_func ("/apple-adaptive/partial-copy", test_partial_copy);
  g_test_add_func ("/apple-adaptive/refused", test_refused);
  g_test_add_func ("/apple-adaptive/idct", test_idct);
  return g_test_run ();
}
