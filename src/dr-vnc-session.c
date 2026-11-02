/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VNC through libvncclient, including Apple Remote Desktop sign-in (Mac
 * Screen Sharing with the Mac account) and plain VNC passwords.
 */
#include "dr-vnc-session.h"

#include <rfb/rfbclient.h>

struct _DrVncSession
{
  DrSession parent_instance;

  rfbClient *client;
  guint buttons;            /* RFB button mask currently held */
  GHashTable *pressed_keys; /* keycode -> keysym sent on press */
  int dirty_x1, dirty_y1, dirty_x2, dirty_y2;
  gboolean asked_credentials;
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

  if (utf8 != NULL)
    dr_session_push_clipboard (DR_SESSION (SELF (cl)), utf8);
}

#ifdef HAVE_VNC_UTF8_CLIPBOARD
static void
vnc_got_cut_text_utf8 (rfbClient *cl, const char *text, int len)
{
  g_autofree char *utf8 = g_strndup (text, len);

  if (g_utf8_validate (utf8, -1, NULL))
    dr_session_push_clipboard (DR_SESSION (SELF (cl)), utf8);
}
#endif

static void
send_clipboard (rfbClient *cl, const char *text)
{
  g_autofree char *latin1 = NULL;
  gsize len = 0;

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
