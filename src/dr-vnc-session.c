/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VNC through libvncclient, including Apple Remote Desktop sign-in (Mac
 * Screen Sharing with the Mac account) and plain VNC passwords.
 */
#include "dr-vnc-session.h"

#include "dr-apple-adaptive.h"

#include <netinet/in.h>
#include <linux/tcp.h> /* tcpi_bytes_received, missing from older glibc */
#include <rfb/rfbclient.h>
#include <sys/socket.h>
#include <zlib.h>

struct _DrVncSession
{
  DrSession parent_instance;

  rfbClient *client;
  guint buttons;            /* RFB button mask currently held */
  GHashTable *pressed_keys; /* keycode -> keysym sent on press */
  int dirty_x1, dirty_y1, dirty_x2, dirty_y2;
  gboolean asked_credentials;
  gboolean apple; /* a Mac's Screen Sharing, which has its own clipboard messages */
  DrAppleAdaptive *adaptive; /* the Mac's Adaptive picture */
  int picture;               /* atomic DrPicture */
  gboolean is_adaptive;      /* what the Mac was last asked for */
  gboolean busy;             /* Automatic: much of the screen keeps moving */
  gint64 motion_since;       /* start of the second being measured */
  guint64 motion_bytes;      /* bytes received by then */
  gint64 motion_area;        /* pixels changed since */
  int busy_seconds, calm_seconds;
  gint64 settled_at;         /* back in Full, whose first repaint is no motion */
  gboolean armed;            /* AutoFrameBufferUpdate covers the screen */
  double scroll_x, scroll_y; /* points of a Mac scroll not sent yet */
  guint updates;
  gint64 updates_since;
  gboolean cancelled;
};

G_DEFINE_FINAL_TYPE (DrVncSession, dr_vnc_session, DR_TYPE_SESSION)

static int client_tag;
static GPrivate last_error = G_PRIVATE_INIT (g_free);

#define SELF(cl) ((DrVncSession *) rfbClientGetClientData ((cl), &client_tag))

static void
vnc_log (const char *format, ...)
{
  g_autofree char *message = NULL;
  va_list args;

  /* libvncclient is chatty, so only with debug messages on */
  if (!g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, G_LOG_DOMAIN))
    {
      va_start (args, format);
      message = g_strdup_vprintf (format, args);
      va_end (args);
      g_debug ("VNC: %s", g_strstrip (message));
    }
}

static void
vnc_error (const char *format, ...)
{
  va_list args;

  va_start (args, format);
  g_private_replace (&last_error, g_strstrip (g_strdup_vprintf (format, args)));
  va_end (args);
  g_debug ("VNC: %s", (const char *) g_private_get (&last_error));
}

/* Also called when the screen changes size. A Mac announces its size again
 * when its display goes to sleep, and stops pushing updates until it is
 * armed again, even when the size is the same. */
static rfbBool
vnc_malloc_framebuffer (rfbClient *cl)
{
  SELF (cl)->armed = FALSE;
  free (cl->frameBuffer);
  cl->frameBuffer = calloc ((size_t) cl->width * cl->height, 4);
  return cl->frameBuffer != NULL;
}

static void
vnc_got_update (rfbClient *cl, int x, int y, int w, int h)
{
  DrVncSession *self = SELF (cl);

  self->dirty_x1 = MIN (self->dirty_x1, x);
  self->dirty_y1 = MIN (self->dirty_y1, y);
  self->dirty_x2 = MAX (self->dirty_x2, x + w);
  self->dirty_y2 = MAX (self->dirty_y2, y + h);
  self->motion_area += (gint64) w * h;
}

static void
reset_dirty (DrVncSession *self)
{
  self->dirty_x1 = self->dirty_y1 = G_MAXINT;
  self->dirty_x2 = self->dirty_y2 = G_MININT;
}

static void
vnc_finished_update (rfbClient *cl)
{
  DrVncSession *self = SELF (cl);
  gint64 now = g_get_monotonic_time ();

  /* How many updates a second the server sends, with debug messages on. */
  self->updates++;
  if (now - self->updates_since >= 5 * G_USEC_PER_SEC)
    {
      if (self->updates_since)
        g_debug ("VNC: %.1f updates a second", self->updates * (double) G_USEC_PER_SEC / (now - self->updates_since));
      self->updates = 0;
      self->updates_since = now;
    }

  if (self->dirty_x2 > self->dirty_x1)
    dr_session_push_frame (DR_SESSION (self), cl->frameBuffer, cl->width, cl->height,
                           (gsize) cl->width * 4, GDK_MEMORY_B8G8R8X8,
                           self->dirty_x1, self->dirty_y1,
                           self->dirty_x2 - self->dirty_x1, self->dirty_y2 - self->dirty_y1);
  reset_dirty (self);
}

static void
vnc_got_cursor (rfbClient *cl, int xhot, int yhot, int width, int height, int bpp)
{
  g_autofree guint8 *bgra = NULL;

  if (bpp != 4 || cl->rcSource == NULL || cl->rcMask == NULL || width <= 0 || height <= 0)
    return;

  bgra = g_malloc ((gsize) width * height * 4);
  for (int i = 0; i < width * height; i++)
    {
      memcpy (bgra + i * 4, cl->rcSource + i * 4, 3);
      bgra[i * 4 + 3] = cl->rcMask[i] ? 0xff : 0x00;
    }
  dr_session_push_cursor (DR_SESSION (SELF (cl)), bgra, width, height, xhot, yhot);
}

static void
vnc_got_cut_text (rfbClient *cl, const char *text, int len)
{
  g_autofree char *utf8 = g_convert (text, len, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);

  g_debug ("VNC: received %d bytes of Latin-1 clipboard text", len);

  if (utf8 != NULL)
    dr_session_push_clipboard (DR_SESSION (SELF (cl)), utf8);
}

#ifdef HAVE_VNC_UTF8_CLIPBOARD
static void
vnc_got_cut_text_utf8 (rfbClient *cl, const char *text, int len)
{
  g_autofree char *utf8 = g_strndup (text, len);

  g_debug ("VNC: received %d bytes of UTF-8 clipboard text", len);

  if (g_utf8_validate (utf8, -1, NULL))
    dr_session_push_clipboard (DR_SESSION (SELF (cl)), utf8);
}
#endif

/* Apple's clipboard
 *
 * A Mac never sends or takes standard RFB cut text. Its own viewer enables
 * AutoPasteboard, is told by MiscStatus when the Mac's pasteboard changes,
 * fetches it, and sends its own the same way: a zlib-compressed archive of
 * pasteboard flavors. Layouts follow the measurements published by remotex
 * (https://github.com/andrewtheguy/remotex, docs/apple-vnc-889.md).
 */

#define APPLE_MISC_STATUS 0x14
#define APPLE_PASTEBOARD 0x1f
#define APPLE_PASTEBOARD_CHANGED 2
#define APPLE_MAX_ARCHIVE (100 * 1024 * 1024)
#define APPLE_TEXT_FLAVOR "public.utf8-plain-text"

static void
apple_start (rfbClient *cl)
{
  /* ViewerInfo: who we are, and the bitmap of server messages we handle. Its
   * bit 20 lets the Mac send MiscStatus. */
  guint8 viewer_info[66] = { 0x21, 0, 0, 62, 0, 1, 0, 0, 0, 2, 0, 0, 0, 6, 0, 0, 0, 1 };
  const guint8 set_mode_control[4] = { 0x0a, 0, 0, 1 };
  const guint8 auto_pasteboard[8] = { 0x15, 0, 0, 1, 0, 0, 0, 0 };

  viewer_info[25] = 26; /* macOS 26.6.2 */
  viewer_info[29] = 6;
  viewer_info[33] = 2;
  viewer_info[34] = 0xb0;
  viewer_info[36] = 0x0c;
  viewer_info[37] = 0x03;
  viewer_info[38] = 0x90;
  viewer_info[44] = 0x40;

  WriteToRFBServer (cl, (const char *) viewer_info, sizeof viewer_info);
  WriteToRFBServer (cl, (const char *) set_mode_control, sizeof set_mode_control);
  WriteToRFBServer (cl, (const char *) auto_pasteboard, sizeof auto_pasteboard);
}

static guint32
read_u32 (const guint8 *p)
{
  return ((guint32) p[0] << 24) | ((guint32) p[1] << 16) | ((guint32) p[2] << 8) | p[3];
}

static void
write_u32 (GByteArray *out, guint32 v)
{
  guint8 b[4] = { v >> 24, v >> 16, v >> 8, v };
  g_byte_array_append (out, b, 4);
}

/* Finds the text in an archive: items of flavors, each a counted name, a
 * reserved word, counted key/value tags and counted data. */
static char *
apple_archive_text (const guint8 *data, gsize len)
{
  gsize pos = 0;

#define NEED(n) if ((n) > len - pos) return NULL
  while (pos < len)
    {
      guint32 flavors;

      NEED (4);
      flavors = read_u32 (data + pos);
      pos += 4;
      for (guint32 f = 0; f < flavors; f++)
        {
          guint32 name_len, tags, data_len;
          const guint8 *name;

          NEED (4);
          name_len = read_u32 (data + pos);
          pos += 4;
          NEED (name_len);
          name = data + pos;
          pos += name_len;
          NEED (8);
          tags = read_u32 (data + pos + 4);
          pos += 8;
          for (guint32 t = 0; t < tags * 2; t++)
            {
              guint32 n;

              NEED (4);
              n = read_u32 (data + pos);
              pos += 4;
              NEED (n);
              pos += n;
            }
          NEED (4);
          data_len = read_u32 (data + pos);
          pos += 4;
          NEED (data_len);
          if (name_len == strlen (APPLE_TEXT_FLAVOR) &&
              memcmp (name, APPLE_TEXT_FLAVOR, name_len) == 0 &&
              g_utf8_validate ((const char *) data + pos, data_len, NULL))
            return g_strndup ((const char *) data + pos, data_len);
          pos += data_len;
        }
    }
#undef NEED
  return NULL;
}

static void
apple_receive_pasteboard (rfbClient *cl)
{
  guint8 header[15];
  guint32 uncompressed, compressed;
  g_autofree guint8 *in = NULL;
  g_autofree guint8 *out = NULL;
  g_autofree char *text = NULL;
  z_stream zs = { 0 };
  int status;

  if (!ReadFromRFBServer (cl, (char *) header, sizeof header))
    return;
  uncompressed = read_u32 (header + 7);
  compressed = read_u32 (header + 11);
  if (compressed > APPLE_MAX_ARCHIVE || uncompressed > APPLE_MAX_ARCHIVE)
    return;
  in = g_malloc (MAX (compressed, 1));
  if (!ReadFromRFBServer (cl, (char *) in, compressed))
    return;

  out = g_malloc (MAX (uncompressed, 1));
  if (inflateInit (&zs) != Z_OK)
    return;
  zs.next_in = in;
  zs.avail_in = compressed;
  zs.next_out = out;
  zs.avail_out = uncompressed;
  /* The Mac sync-flushes the stream rather than ending it. */
  status = inflate (&zs, Z_SYNC_FLUSH);
  inflateEnd (&zs);
  if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR)
    return;

  g_debug ("VNC: received a %u-byte Mac pasteboard", uncompressed);
  text = apple_archive_text (out, zs.total_out);
  if (text != NULL)
    dr_session_push_clipboard (DR_SESSION (SELF (cl)), text);
}

static rfbBool
apple_handle_message (rfbClient *cl, rfbServerToClientMsg *message)
{
  DrVncSession *self = SELF (cl);

  if (self == NULL || !self->apple)
    return FALSE;

  switch (message->type)
    {
    case APPLE_MISC_STATUS:
      {
        guint8 rest[7];

        if (!ReadFromRFBServer (cl, (char *) rest, sizeof rest))
          return FALSE;
        if (((rest[5] << 8) | rest[6]) == APPLE_PASTEBOARD_CHANGED)
          {
            const guint8 fetch[8] = { 0x0b, 0, 0, 0, 0, 0, 0, 1 };
            WriteToRFBServer (cl, (const char *) fetch, sizeof fetch);
          }
        return TRUE;
      }

    case APPLE_PASTEBOARD:
      apple_receive_pasteboard (cl);
      return TRUE;

    default:
      return FALSE;
    }
}

static gboolean
apple_want_adaptive (DrVncSession *self)
{
  DrPicture picture = g_atomic_int_get (&self->picture);

  return picture == DR_PICTURE_ADAPTIVE || (picture == DR_PICTURE_AUTOMATIC && self->busy);
}

/* Lists Adaptive's tiles or Full quality's zlib first. Switching later is
 * safe: the Mac keeps a deflate stream per encoding, and never sends the ZRLE
 * that shares libvncclient's zlib stream. */
static void
apple_set_quality (DrVncSession *self, rfbClient *cl)
{
  self->is_adaptive = apple_want_adaptive (self);
  cl->appData.encodingsString = self->is_adaptive ? "raw" : "zlib raw";
}

static guint64
received_bytes (rfbClient *cl)
{
  struct tcp_info info = { 0 };
  socklen_t len = sizeof info;

  if (getsockopt (cl->sock, IPPROTO_TCP, TCP_INFO, &info, &len) != 0)
    return 0;
  return info.tcpi_bytes_received;
}

/* Automatic quality, measured a second at a time. A Mac sends lossless video
 * at only about 10 MB/s, so a picture that keeps moving falls further and
 * further behind in Full quality: two seconds of over 4 MB/s switch to
 * Adaptive, as Apple's viewer does. Two seconds with less than a quarter of
 * the screen changing switch back, and the repaint that follows in Full
 * quality is not counted as motion. */
static void
apple_watch_motion (DrVncSession *self, rfbClient *cl)
{
  gint64 now = g_get_monotonic_time ();
  guint64 bytes = received_bytes (cl);
  gint64 elapsed = now - self->motion_since;

  if (elapsed < G_USEC_PER_SEC)
    return;

  if (self->motion_since != 0 && now - self->settled_at > 3 * G_USEC_PER_SEC)
    {
      double rate = (bytes - self->motion_bytes) * (double) G_USEC_PER_SEC / elapsed;
      double screens = self->motion_area * (double) G_USEC_PER_SEC / elapsed /
                       ((double) cl->width * cl->height);

      if (!self->busy)
        {
          self->busy_seconds = rate > 4e6 ? self->busy_seconds + 1 : 0;
          if (self->busy_seconds >= 2)
            {
              g_debug ("VNC: motion at %.1f MB/s, Adaptive picture", rate / 1e6);
              self->busy = TRUE;
              self->calm_seconds = 0;
            }
        }
      else
        {
          self->calm_seconds = screens < 0.25 ? self->calm_seconds + 1 : 0;
          if (self->calm_seconds >= 2)
            {
              g_debug ("VNC: still, Full quality picture");
              self->busy = FALSE;
              self->busy_seconds = 0;
              self->settled_at = now;
            }
        }
    }

  self->motion_since = now;
  self->motion_bytes = bytes;
  self->motion_area = 0;
}

/* Apple's viewer arms AutoFrameBufferUpdate at 0, so the Mac pushes every
 * change of the screen as soon as it captures it. Answering requests alone,
 * a Mac's own display draws a few updates a second while a window moves.
 * The arming covers the screen, and is sent again whenever the Mac announces
 * its size, which it does when its display sleeps. */
static void
apple_arm (DrVncSession *self, rfbClient *cl)
{
  const guint8 message[16] = {
    0x09, 0, 0, 1,                        /* version 1 */
    0, 0, 0, 0,                           /* interval: as fast as it changes */
    0, 0, 0, 0,                           /* x, y */
    cl->width >> 8, cl->width & 0xff, cl->height >> 8, cl->height & 0xff,
  };

  WriteToRFBServer (cl, (const char *) message, sizeof message);
  self->armed = TRUE;
}

/* Apple's picture
 *
 * A Mac codes every rectangle in the first of zlib, ZRLE and its own codecs
 * that the viewer lists, and keeps the last of zlib and ZRLE as a lossless one.
 * Its own viewer's Adaptive quality lists the 8×8 tiles first, which send a
 * changed area coarsely at once and refine it after, a fraction of ZRLE's
 * size; dr-apple-adaptive.c decodes them. libvncclient lists extension
 * encodings after its own, so a Mac session lists only raw of those
 * (vnc_get_credential) and lets this list put the tiles ahead of ZRLE.
 * Full quality asks for lossless zlib instead, as Apple's viewer does.
 * Anything else ignores 0x3f3.
 */
static int apple_encodings[] = { DR_APPLE_ADAPTIVE_ENCODING, rfbEncodingZRLE, 0 };

static rfbBool
apple_handle_encoding (rfbClient *cl, rfbFramebufferUpdateRectHeader *rect)
{
  DrVncSession *self = SELF (cl);
  g_autoptr (GError) error = NULL;
  g_autofree guint8 *body = NULL;
  guint32 len;

  if (self == NULL || rect->encoding != DR_APPLE_ADAPTIVE_ENCODING)
    return FALSE;

  if (!ReadFromRFBServer (cl, (char *) &len, sizeof len))
    return FALSE;
  len = GUINT32_FROM_BE (len);
  if (len == 0 || len > 100000000)
    {
      g_warning ("VNC: Apple tiles of %u bytes", len);
      return FALSE;
    }
  body = g_malloc (len);
  if (!ReadFromRFBServer (cl, (char *) body, len))
    return FALSE;

  if (self->adaptive == NULL)
    self->adaptive = dr_apple_adaptive_new ();
  /* The Mac codes the next update against what its viewer made of this one,
   * so a rectangle that does not decode only spoils its own area. */
  if (!dr_apple_adaptive_decode (self->adaptive, (guint32 *) cl->frameBuffer, cl->width, cl->height,
                                 rect->r.x, rect->r.y, rect->r.w, rect->r.h, body, len, &error))
    g_debug ("VNC: Apple tiles: %s", error->message);
  return TRUE;
}

static rfbClientProtocolExtension apple_extension = {
  .encodings = apple_encodings,
  .handleEncoding = apple_handle_encoding,
  .handleMessage = apple_handle_message,
};

static void
apple_send_pasteboard (rfbClient *cl, const char *text)
{
  g_autoptr (GByteArray) archive = g_byte_array_new ();
  g_autoptr (GByteArray) msg = g_byte_array_new ();
  gsize len = strlen (text);
  z_stream zs = { 0 };
  guint8 *out;
  gsize bound;
  const guint8 type[4] = { APPLE_PASTEBOARD, 0, 0, 0 };

  /* One item with one text flavor; an item with no flavors clears the pasteboard. */
  if (len == 0)
    write_u32 (archive, 0);
  else
    {
      write_u32 (archive, 1);
      write_u32 (archive, strlen (APPLE_TEXT_FLAVOR));
      g_byte_array_append (archive, (const guint8 *) APPLE_TEXT_FLAVOR, strlen (APPLE_TEXT_FLAVOR));
      write_u32 (archive, 0); /* reserved */
      write_u32 (archive, 0); /* no tags */
      write_u32 (archive, len);
      g_byte_array_append (archive, (const guint8 *) text, len);
    }

  if (deflateInit (&zs, Z_DEFAULT_COMPRESSION) != Z_OK)
    return;
  bound = deflateBound (&zs, archive->len) + 64;
  out = g_malloc (bound);
  zs.next_in = archive->data;
  zs.avail_in = archive->len;
  zs.next_out = out;
  zs.avail_out = bound;
  deflate (&zs, Z_SYNC_FLUSH);
  deflateEnd (&zs);

  g_byte_array_append (msg, type, sizeof type);
  write_u32 (msg, 1); /* the Mac ignores this word */
  write_u32 (msg, archive->len);
  write_u32 (msg, zs.total_out);
  g_byte_array_append (msg, out, zs.total_out);
  g_free (out);
  WriteToRFBServer (cl, (const char *) msg->data, msg->len);
}

static void
send_clipboard (rfbClient *cl, const char *text)
{
  g_autofree char *latin1 = NULL;
  gsize len = 0;

  g_debug ("VNC: sending %zu bytes of clipboard text", strlen (text));
  if (SELF (cl)->apple)
    {
      apple_send_pasteboard (cl, text);
      return;
    }

#ifdef HAVE_VNC_UTF8_CLIPBOARD
  if (SendClientCutTextUTF8 (cl, (char *) text, strlen (text)))
    return;
#endif
  /* Classic VNC servers (including the Mac's) only take Latin-1 text. */
  latin1 = g_convert_with_fallback (text, -1, "ISO-8859-1", "UTF-8", "?", NULL, &len, NULL);
  if (latin1 != NULL)
    SendClientCutText (cl, latin1, len);
}

static gboolean
get_credentials (DrVncSession *self, gboolean need_username, char **username, char **password)
{
  DrSession *session = DR_SESSION (self);
  g_autofree char *message = NULL;
  g_autofree char *domain = NULL;

  if (!self->asked_credentials && dr_session_get_password (session) &&
      (!need_username || dr_session_get_username (session)))
    {
      self->asked_credentials = TRUE;
      *username = g_strdup (dr_session_get_username (session));
      *password = g_strdup (dr_session_get_password (session));
      return TRUE;
    }

  self->asked_credentials = TRUE;
  message = need_username
    ? g_strdup_printf ("Sign in to %s with an account on that computer.",
                       dr_session_get_host (session))
    : g_strdup_printf ("Enter the VNC password of %s.", dr_session_get_host (session));
  if (!dr_session_ask_credentials (session, message, need_username, username, password, &domain))
    {
      self->cancelled = TRUE;
      return FALSE;
    }
  return TRUE;
}

static char *
vnc_get_password (rfbClient *cl)
{
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;

  if (!get_credentials (SELF (cl), FALSE, &username, &password))
    return NULL;
  return strdup (password ? password : "");
}

static rfbCredential *
vnc_get_credential (rfbClient *cl, int type)
{
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;
  rfbCredential *credential;

  if (type != rfbCredentialTypeUser)
    return NULL;
  /* A Mac signs in before the encodings are sent, which is the time to ask
   * for its own picture: libvncclient's zlib and ZRLE share one inflate stream,
   * so switching from ZRLE later would decode zlib on the wrong stream. */
  if (cl->authScheme == rfbARD)
    apple_set_quality (SELF (cl), cl);
  if (!get_credentials (SELF (cl), TRUE, &username, &password))
    return NULL;

  credential = calloc (1, sizeof (rfbCredential));
  credential->userCredential.username = strdup (username ? username : "");
  credential->userCredential.password = strdup (password ? password : "");
  return credential;
}

/* RFB has no Back and Forward buttons, and X servers take bit 7 as Back. A
 * Mac posts a wheel bit together with another button as Back (wheel up) or
 * Forward (wheel down), plus that button; with bit 7 that is the Mac's eighth
 * button, which applications leave alone. Measured on macOS 26. */
#define APPLE_BACK (rfbWheelUpMask | 0x80)
#define APPLE_FORWARD (rfbWheelDownMask | 0x80)

/* A Mac scrolls only about two pixels for a wheel button of the pointer
 * event, so like Apple's viewer this sends the distance in Apple's own
 * scroll-wheel event: message 0x17, kind 11, with deltas in lines, in lines
 * as 16.16 fixed point and in points, each horizontal, vertical and a third
 * axis, positive up and to the left. Continuous makes the points count.
 * Layout from zDesk's docs/apple-vnc-889.md. */
#define POINTS_PER_NOTCH 50.0
#define POINTS_PER_LINE 10

static void
apple_scroll (DrVncSession *self, rfbClient *cl, const DrInputEvent *e)
{
  guint8 message[58] = { 0x17, 0, 0, 54, 0, 1, 0, 11 };
  gint32 points[3] = { 0 };
  guint8 *p = message + 8;

  self->scroll_x -= e->notches_x * POINTS_PER_NOTCH;
  self->scroll_y -= e->notches_y * POINTS_PER_NOTCH;
  points[0] = (gint32) self->scroll_x;
  points[1] = (gint32) self->scroll_y;
  self->scroll_x -= points[0];
  self->scroll_y -= points[1];
  if (points[0] == 0 && points[1] == 0)
    return;

  for (int i = 0; i < 3; i++, p += 2)
    {
      gint16 lines = points[i] / POINTS_PER_LINE;
      p[0] = (guint16) lines >> 8;
      p[1] = (guint16) lines & 0xff;
    }
  for (int i = 0; i < 3; i++, p += 4)
    {
      guint32 fixed = (guint32) (points[i] * 65536 / POINTS_PER_LINE);
      p[0] = fixed >> 24; p[1] = fixed >> 16; p[2] = fixed >> 8; p[3] = fixed;
    }
  for (int i = 0; i < 3; i++, p += 4)
    {
      guint32 value = (guint32) points[i];
      p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
    }
  p += 12;                                /* scroll phase, momentum, count */
  p[3] = 0x02;                            /* continuous */
  p += 4;
  p[0] = e->x >> 8; p[1] = e->x & 0xff;
  p[2] = e->y >> 8; p[3] = e->y & 0xff;

  WriteToRFBServer (cl, (const char *) message, sizeof message);
}

static void
send_input (DrVncSession *self, const DrInputEvent *e)
{
  rfbClient *cl = self->client;
  guint bit = 0;

  switch (e->type)
    {
    case DR_INPUT_MOTION:
      SendPointerEvent (cl, e->x, e->y, self->buttons);
      break;

    case DR_INPUT_BUTTON:
      switch (e->button)
        {
        case GDK_BUTTON_PRIMARY: bit = rfbButton1Mask; break;
        case GDK_BUTTON_MIDDLE: bit = rfbButton2Mask; break;
        case GDK_BUTTON_SECONDARY: bit = rfbButton3Mask; break;
        case 8: bit = self->apple ? APPLE_BACK : 0x80; break;
        case 9: bit = self->apple ? APPLE_FORWARD : 0; break;
        default: return;
        }
      if (bit == 0)
        return;
      if (e->down)
        self->buttons |= bit;
      else
        self->buttons &= ~bit;
      SendPointerEvent (cl, e->x, e->y, self->buttons);
      break;

    case DR_INPUT_SCROLL:
      if (self->apple)
        {
          apple_scroll (self, cl, e);
          break;
        }
      for (int i = 0; i < ABS (e->dy); i++)
        {
          bit = e->dy < 0 ? rfbButton4Mask : rfbButton5Mask;
          SendPointerEvent (cl, e->x, e->y, self->buttons | bit);
          SendPointerEvent (cl, e->x, e->y, self->buttons);
        }
      for (int i = 0; i < ABS (e->dx); i++)
        {
          bit = e->dx < 0 ? 0x20 : 0x40; /* buttons 6 and 7 */
          SendPointerEvent (cl, e->x, e->y, self->buttons | bit);
          SendPointerEvent (cl, e->x, e->y, self->buttons);
        }
      break;

    case DR_INPUT_KEY:
      if (e->down)
        {
          g_hash_table_insert (self->pressed_keys, GUINT_TO_POINTER (e->keycode),
                               GUINT_TO_POINTER (e->keyval));
          SendKeyEvent (cl, e->keyval, TRUE);
        }
      else
        {
          gpointer keyval;

          /* Release the same keysym that was pressed, even if Shift changed since. */
          if (g_hash_table_steal_extended (self->pressed_keys, GUINT_TO_POINTER (e->keycode),
                                           NULL, &keyval))
            SendKeyEvent (cl, GPOINTER_TO_UINT (keyval), FALSE);
          else
            SendKeyEvent (cl, e->keyval, FALSE);
        }
      break;

    case DR_INPUT_RESIZE:
      break; /* the picture is scaled to fit instead */

    case DR_INPUT_CLIPBOARD:
      send_clipboard (cl, e->text);
      break;
    }
}

static void
set_connect_error (DrVncSession *self, GError **error)
{
  DrSession *session = DR_SESSION (self);
  const char *message = g_private_get (&last_error);
  g_autofree char *lower = g_ascii_strdown (message ? message : "", -1);

  if (self->cancelled)
    g_set_error_literal (error, DR_SESSION_ERROR, DR_SESSION_ERROR_CANCELLED,
                         "The sign-in was cancelled.");
  else if (strstr (lower, "auth") || strstr (lower, "password"))
    g_set_error_literal (error, DR_SESSION_ERROR, DR_SESSION_ERROR_AUTH,
                         "The user name or password is incorrect.");
  else if (strstr (lower, "unable to connect") || strstr (lower, "connect to"))
    g_set_error (error, DR_SESSION_ERROR, DR_SESSION_ERROR_FAILED,
                 "Couldn't reach %s on port %u. Check that the computer is on and that "
                 "screen sharing is turned on.",
                 dr_session_get_host (session), dr_session_get_port (session));
  else if (message && *message)
    g_set_error (error, DR_SESSION_ERROR, DR_SESSION_ERROR_FAILED, "%s", message);
  else
    g_set_error (error, DR_SESSION_ERROR, DR_SESSION_ERROR_FAILED,
                 "Couldn't connect to %s.", dr_session_get_host (session));
}

static gboolean
dr_vnc_session_run (DrSession *session, GError **error)
{
  DrVncSession *self = DR_VNC_SESSION (session);
  rfbClient *cl;
  DrInputEvent *event;

  g_private_replace (&last_error, NULL);
  reset_dirty (self);

  cl = rfbGetClient (8, 3, 4);
  cl->format.redShift = 16;
  cl->format.greenShift = 8;
  cl->format.blueShift = 0;
  cl->format.bigEndian = FALSE;
  cl->canHandleNewFBSize = TRUE;
  cl->appData.useRemoteCursor = TRUE;
  cl->appData.encodingsString = "tight zrle ultra copyrect hextile zlib corre rre raw";
  cl->appData.compressLevel = 3;
  cl->appData.qualityLevel = 7;
  cl->connectTimeout = 15;
  cl->MallocFrameBuffer = vnc_malloc_framebuffer;
  cl->GotFrameBufferUpdate = vnc_got_update;
  cl->FinishedFrameBufferUpdate = vnc_finished_update;
  cl->GotCursorShape = vnc_got_cursor;
  cl->GotXCutText = vnc_got_cut_text;
#ifdef HAVE_VNC_UTF8_CLIPBOARD
  cl->GotXCutTextUTF8 = vnc_got_cut_text_utf8;
#endif
  cl->GetPassword = vnc_get_password;
  cl->GetCredential = vnc_get_credential;
  cl->serverHost = strdup (dr_session_get_host (session));
  cl->serverPort = dr_session_get_port (session);
  rfbClientSetClientData (cl, &client_tag, self);

  /* rfbInitClient frees the client when it fails */
  if (!rfbInitClient (cl, NULL, NULL))
    {
      set_connect_error (self, error);
      return FALSE;
    }

  self->client = cl;
  self->apple = cl->authScheme == rfbARD;
  if (self->apple)
    {
      apple_start (cl);
      apple_arm (self, cl);
    }
  dr_session_notify_connected (session);

  while (!dr_session_should_stop (session))
    {
      int status;

      while ((event = dr_session_pop_input (session)) != NULL)
        {
          send_input (self, event);
          dr_input_event_free (event);
        }

      if (self->apple && g_atomic_int_get (&self->picture) == DR_PICTURE_AUTOMATIC)
        apple_watch_motion (self, cl);
      if (self->apple && apple_want_adaptive (self) != self->is_adaptive)
        {
          apple_set_quality (self, cl);
          SetFormatAndEncodings (cl);
          /* What Adaptive left is repainted losslessly. */
          if (!self->is_adaptive)
            SendFramebufferUpdateRequest (cl, 0, 0, cl->width, cl->height, FALSE);
        }
      if (self->apple && !self->armed)
        apple_arm (self, cl);

      status = WaitForMessage (cl, 10000);
      /* The server closed the connection; the base class reports it. */
      if (status < 0 || (status > 0 && !HandleRFBServerMessage (cl)))
        break;
    }

  self->client = NULL;
  g_clear_pointer (&self->adaptive, dr_apple_adaptive_free);
  free (cl->frameBuffer);
  cl->frameBuffer = NULL;
  rfbClientCleanup (cl);
  return TRUE;
}

static void
dr_vnc_session_wake (DrSession *session)
{
  /* The loop polls every 10 ms, so nothing to do. */
}

static void
dr_vnc_session_finalize (GObject *object)
{
  DrVncSession *self = DR_VNC_SESSION (object);

  g_hash_table_unref (self->pressed_keys);
  G_OBJECT_CLASS (dr_vnc_session_parent_class)->finalize (object);
}

static void
dr_vnc_session_class_init (DrVncSessionClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = dr_vnc_session_finalize;
  DR_SESSION_CLASS (klass)->run = dr_vnc_session_run;
  DR_SESSION_CLASS (klass)->wake = dr_vnc_session_wake;

  rfbClientLog = vnc_log;
  rfbClientErr = vnc_error;
  rfbClientRegisterExtension (&apple_extension);
}

static void
dr_vnc_session_init (DrVncSession *self)
{
  self->pressed_keys = g_hash_table_new (NULL, NULL);
}

void
dr_vnc_session_set_picture (DrVncSession *self, DrPicture picture)
{
  g_return_if_fail (DR_IS_VNC_SESSION (self));

  g_atomic_int_set (&self->picture, picture);
}

DrSession *
dr_vnc_session_new (const char *host, guint port)
{
  return g_object_new (DR_TYPE_VNC_SESSION, "host", host, "port", port ? port : 5900, NULL);
}
