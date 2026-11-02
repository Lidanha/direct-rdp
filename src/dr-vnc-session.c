/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VNC through libvncclient, including Apple Remote Desktop sign-in (Mac
 * Screen Sharing with the Mac account) and plain VNC passwords.
 */
#include "dr-vnc-session.h"

#include <rfb/rfbclient.h>
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
  gboolean cancelled;
};

G_DEFINE_FINAL_TYPE (DrVncSession, dr_vnc_session, DR_TYPE_SESSION)

static int client_tag;
static GPrivate last_error = G_PRIVATE_INIT (g_free);

#define SELF(cl) ((DrVncSession *) rfbClientGetClientData ((cl), &client_tag))

static void
vnc_log (const char *format, ...)
{
  /* libvncclient is chatty; keep it quiet */
}

static void
vnc_error (const char *format, ...)
{
  va_list args;

  va_start (args, format);
  g_private_replace (&last_error, g_strstrip (g_strdup_vprintf (format, args)));
  va_end (args);
}

static rfbBool
vnc_malloc_framebuffer (rfbClient *cl)
{
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

static rfbClientProtocolExtension apple_extension = {
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
  if (!get_credentials (SELF (cl), TRUE, &username, &password))
    return NULL;

  credential = calloc (1, sizeof (rfbCredential));
  credential->userCredential.username = strdup (username ? username : "");
  credential->userCredential.password = strdup (password ? password : "");
  return credential;
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
        default: return;
        }
      if (e->down)
        self->buttons |= bit;
      else
        self->buttons &= ~bit;
      SendPointerEvent (cl, e->x, e->y, self->buttons);
      break;

    case DR_INPUT_SCROLL:
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
    apple_start (cl);
  dr_session_notify_connected (session);

  while (!dr_session_should_stop (session))
    {
      int status;

      while ((event = dr_session_pop_input (session)) != NULL)
        {
          send_input (self, event);
          dr_input_event_free (event);
        }

      status = WaitForMessage (cl, 10000);
      /* The server closed the connection; the base class reports it. */
      if (status < 0 || (status > 0 && !HandleRFBServerMessage (cl)))
        break;
    }

  self->client = NULL;
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

DrSession *
dr_vnc_session_new (const char *host, guint port)
{
  return g_object_new (DR_TYPE_VNC_SESSION, "host", host, "port", port ? port : 5900, NULL);
}
