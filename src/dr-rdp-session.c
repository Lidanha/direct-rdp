/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RDP through FreeRDP 3. Everything that touches FreeRDP runs on the session
 * thread; the main thread only queues input and wakes the loop.
 */
#include "dr-rdp-session.h"

#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/disp.h>
#include <freerdp/channels/disp.h>
#include <freerdp/codec/color.h>
#include <freerdp/event.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/graphics.h>
#include <freerdp/input.h>
#include <winpr/synch.h>

struct _DrRdpSession
{
  DrSession parent_instance;

  GMutex context_lock; /* guards the context pointer, not FreeRDP itself */
  rdpContext *context;
  HANDLE wake_event;

  int wanted_width, wanted_height, wanted_scale;
  int sent_width, sent_height;
};

G_DEFINE_FINAL_TYPE (DrRdpSession, dr_rdp_session, DR_TYPE_SESSION)

typedef struct
{
  rdpClientContext common;
  DrRdpSession *session;
  DispClientContext *disp;
  gboolean disp_ready;
} DrRdpContext;

typedef struct
{
  rdpPointer pointer;
  guint8 *bgra;
} DrPointer;

#define SESSION(ctx) (((DrRdpContext *) (ctx))->session)
#define MAKE_SCANCODE(code, ext) ((code) | ((ext) ? KBDEXT : 0))

/* XKB keycode (Linux evdev code + 8) to RDP scancode. Evdev codes 1-88 are
 * the plain PC/XT set-1 scancodes; the rest needs a table. */
static guint32
scancode_from_keycode (guint keycode)
{
  guint evdev = keycode - 8;

  if (keycode < 8)
    return 0;
  if (evdev >= 1 && evdev <= 88)
    return evdev;

  switch (evdev)
    {
    case 89: return 0x73;                      /* RO */
    case 92: return 0x79;                      /* Henkan */
    case 93: return 0x70;                      /* Katakana/Hiragana */
    case 94: return 0x7B;                      /* Muhenkan */
    case 96: return MAKE_SCANCODE (0x1C, 1);   /* KP Enter */
    case 97: return MAKE_SCANCODE (0x1D, 1);   /* Right Ctrl */
    case 98: return MAKE_SCANCODE (0x35, 1);   /* KP / */
    case 99: return MAKE_SCANCODE (0x37, 1);   /* Print Screen */
    case 100: return MAKE_SCANCODE (0x38, 1);  /* Right Alt / AltGr */
    case 102: return MAKE_SCANCODE (0x47, 1);  /* Home */
    case 103: return MAKE_SCANCODE (0x48, 1);  /* Up */
    case 104: return MAKE_SCANCODE (0x49, 1);  /* Page Up */
    case 105: return MAKE_SCANCODE (0x4B, 1);  /* Left */
    case 106: return MAKE_SCANCODE (0x4D, 1);  /* Right */
    case 107: return MAKE_SCANCODE (0x4F, 1);  /* End */
    case 108: return MAKE_SCANCODE (0x50, 1);  /* Down */
    case 109: return MAKE_SCANCODE (0x51, 1);  /* Page Down */
    case 110: return MAKE_SCANCODE (0x52, 1);  /* Insert */
    case 111: return MAKE_SCANCODE (0x53, 1);  /* Delete */
    case 113: return MAKE_SCANCODE (0x20, 1);  /* Mute */
    case 114: return MAKE_SCANCODE (0x2E, 1);  /* Volume Down */
    case 115: return MAKE_SCANCODE (0x30, 1);  /* Volume Up */
    case 117: return 0x59;                     /* KP = */
    case 121: return 0x7E;                     /* KP , */
    case 124: return 0x7D;                     /* Yen */
    case 125: return MAKE_SCANCODE (0x5B, 1);  /* Left Super */
    case 126: return MAKE_SCANCODE (0x5C, 1);  /* Right Super */
    case 127: return MAKE_SCANCODE (0x5D, 1);  /* Menu */
    case 163: return MAKE_SCANCODE (0x19, 1);  /* Next track */
    case 164: return MAKE_SCANCODE (0x22, 1);  /* Play/Pause */
    case 165: return MAKE_SCANCODE (0x10, 1);  /* Previous track */
    case 166: return MAKE_SCANCODE (0x24, 1);  /* Stop */
    default: return 0;
    }
}

/* Frames */

static void
push_full_frame (rdpContext *context)
{
  rdpGdi *gdi = context->gdi;

  if (gdi && gdi->primary_buffer)
    dr_session_push_frame (DR_SESSION (SESSION (context)), gdi->primary_buffer,
                           gdi->width, gdi->height, gdi->stride, GDK_MEMORY_B8G8R8X8,
                           0, 0, gdi->width, gdi->height);
}

static BOOL
rdp_begin_paint (rdpContext *context)
{
  HGDI_WND hwnd = context->gdi->primary->hdc->hwnd;

  hwnd->invalid->null = TRUE;
  hwnd->ninvalid = 0;
  return TRUE;
}

static BOOL
rdp_end_paint (rdpContext *context)
{
  rdpGdi *gdi = context->gdi;
  HGDI_WND hwnd = gdi->primary->hdc->hwnd;
  HGDI_RGN r = hwnd->invalid;

  if (r->null)
    return TRUE;

  dr_session_push_frame (DR_SESSION (SESSION (context)), gdi->primary_buffer,
                         gdi->width, gdi->height, gdi->stride, GDK_MEMORY_B8G8R8X8,
                         r->x, r->y, r->w, r->h);
  r->null = TRUE;
  hwnd->ninvalid = 0;
  return TRUE;
}

static BOOL
rdp_desktop_resize (rdpContext *context)
{
  rdpSettings *settings = context->settings;

  if (!gdi_resize (context->gdi,
                   freerdp_settings_get_uint32 (settings, FreeRDP_DesktopWidth),
                   freerdp_settings_get_uint32 (settings, FreeRDP_DesktopHeight)))
    return FALSE;
  push_full_frame (context);
  return TRUE;
}

/* Pointer */

static BOOL
pointer_new (rdpContext *context, rdpPointer *pointer)
{
  DrPointer *p = (DrPointer *) pointer;

  if (pointer->width == 0 || pointer->height == 0)
    return TRUE;

  p->bgra = g_malloc0 ((gsize) pointer->width * pointer->height * 4);
  if (!freerdp_image_copy_from_pointer_data (p->bgra, PIXEL_FORMAT_BGRA32, pointer->width * 4,
                                             0, 0, pointer->width, pointer->height,
                                             pointer->xorMaskData, pointer->lengthXorMask,
                                             pointer->andMaskData, pointer->lengthAndMask,
                                             pointer->xorBpp, &context->gdi->palette))
    {
      g_clear_pointer (&p->bgra, g_free);
      return FALSE;
    }
  return TRUE;
}

static void
pointer_free (rdpContext *context, rdpPointer *pointer)
{
  g_clear_pointer (&((DrPointer *) pointer)->bgra, g_free);
}

static BOOL
pointer_set (rdpContext *context, rdpPointer *pointer)
{
  DrPointer *p = (DrPointer *) pointer;

  if (p->bgra)
    dr_session_push_cursor (DR_SESSION (SESSION (context)), p->bgra,
                            pointer->width, pointer->height, pointer->xPos, pointer->yPos);
  return TRUE;
}

static BOOL
pointer_set_null (rdpContext *context)
{
  dr_session_push_cursor_named (DR_SESSION (SESSION (context)), "none");
  return TRUE;
}

static BOOL
pointer_set_default (rdpContext *context)
{
  dr_session_push_cursor_named (DR_SESSION (SESSION (context)), "default");
  return TRUE;
}

static BOOL
pointer_set_position (rdpContext *context, UINT32 x, UINT32 y)
{
  return TRUE;
}

/* Display control (dynamic resolution) */

static void
send_wanted_size (DrRdpSession *self)
{
  DrRdpContext *ctx = (DrRdpContext *) self->context;
  DISPLAY_CONTROL_MONITOR_LAYOUT layout = { 0 };
  int scale = self->wanted_scale;

  if (ctx == NULL || ctx->disp == NULL || !ctx->disp_ready || self->wanted_width <= 0)
    return;
  if (self->wanted_width == self->sent_width && self->wanted_height == self->sent_height)
    return;

  layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
  layout.Width = self->wanted_width;
  layout.Height = self->wanted_height;
  layout.PhysicalWidth = 0;
  layout.PhysicalHeight = 0;
  layout.Orientation = ORIENTATION_LANDSCAPE;
  layout.DesktopScaleFactor = CLAMP (scale, 100, 500);
  layout.DeviceScaleFactor = scale >= 180 ? 180 : scale >= 140 ? 140 : 100;

  if (ctx->disp->SendMonitorLayout (ctx->disp, 1, &layout) == CHANNEL_RC_OK)
    {
      self->sent_width = self->wanted_width;
      self->sent_height = self->wanted_height;
    }
}

static UINT
disp_caps (DispClientContext *disp, UINT32 max_monitors, UINT32 factor_a, UINT32 factor_b)
{
  DrRdpContext *ctx = disp->custom;

  ctx->disp_ready = TRUE;
  send_wanted_size (ctx->session);
  return CHANNEL_RC_OK;
}

static void
on_channel_connected (void *context, const ChannelConnectedEventArgs *e)
{
  DrRdpContext *ctx = context;

  if (g_strcmp0 (e->name, DISP_DVC_CHANNEL_NAME) == 0)
    {
      ctx->disp = e->pInterface;
      ctx->disp->custom = ctx;
      ctx->disp->DisplayControlCaps = disp_caps;
    }
  else
    {
      freerdp_client_OnChannelConnectedEventHandler (context, e);
    }
}

static void
on_channel_disconnected (void *context, const ChannelDisconnectedEventArgs *e)
{
  DrRdpContext *ctx = context;

  if (g_strcmp0 (e->name, DISP_DVC_CHANNEL_NAME) == 0)
    {
      ctx->disp = NULL;
      ctx->disp_ready = FALSE;
    }
  else
    {
      freerdp_client_OnChannelDisconnectedEventHandler (context, e);
    }
}

/* Connection callbacks */

static BOOL
rdp_pre_connect (freerdp *instance)
{
  rdpContext *context = instance->context;

  PubSub_SubscribeChannelConnected (context->pubSub, on_channel_connected);
  PubSub_SubscribeChannelDisconnected (context->pubSub, on_channel_disconnected);
  return TRUE;
}

static BOOL
rdp_post_connect (freerdp *instance)
{
  rdpContext *context = instance->context;
  rdpPointer pointer = { 0 };

  if (!gdi_init (instance, PIXEL_FORMAT_BGRX32))
    return FALSE;

  pointer.size = sizeof (DrPointer);
  pointer.New = pointer_new;
  pointer.Free = pointer_free;
  pointer.Set = pointer_set;
  pointer.SetNull = pointer_set_null;
  pointer.SetDefault = pointer_set_default;
  pointer.SetPosition = pointer_set_position;
  graphics_register_pointer (context->graphics, &pointer);

  context->update->BeginPaint = rdp_begin_paint;
  context->update->EndPaint = rdp_end_paint;
  context->update->DesktopResize = rdp_desktop_resize;

  push_full_frame (context);
  dr_session_notify_connected (DR_SESSION (SESSION (context)));
  return TRUE;
}

static void
rdp_post_disconnect (freerdp *instance)
{
  gdi_free (instance);
}

static void
replace_string (char **target, char *value)
{
  free (*target);
  *target = value ? strdup (value) : NULL;
}

static BOOL
rdp_authenticate (freerdp *instance, char **username, char **password, char **domain,
                  rdp_auth_reason reason)
{
  DrSession *session = DR_SESSION (SESSION (instance->context));
  g_autofree char *message = NULL;
  g_autofree char *new_username = NULL;
  g_autofree char *new_password = NULL;
  g_autofree char *new_domain = NULL;

  switch (reason)
    {
    case AUTH_NLA:
    case AUTH_TLS:
    case AUTH_RDP:
    case AUTH_RDSTLS:
      break;
    default:
      return FALSE; /* gateways and smartcards are not supported */
    }

  if (*username && **username && *password && **password)
    return TRUE;

  message = g_strdup_printf ("Sign in to %s with an account on that computer.",
                             dr_session_get_host (session));
  if (!dr_session_ask_credentials (session, message, TRUE,
                                   &new_username, &new_password, &new_domain))
    return FALSE;

  replace_string (username, new_username);
  replace_string (password, new_password);
  replace_string (domain, new_domain && *new_domain ? new_domain : NULL);
  return TRUE;
}

static DWORD
rdp_verify_certificate (freerdp *instance, const char *host, UINT16 port,
                        const char *common_name, const char *subject, const char *issuer,
                        const char *fingerprint, DWORD flags)
{
  DrSession *session = DR_SESSION (SESSION (instance->context));
  g_autofree char *body = NULL;

  body = g_strdup_printf ("This is the first connection to %s, so its identity can't be "
                          "checked yet. This is normal the first time you connect to your own "
                          "computer.\n\nCertificate issued to: %s\nFingerprint: %s",
                          host, common_name ? common_name : subject, fingerprint);
  return dr_session_ask_certificate (session, "Trust This Computer?", body, FALSE);
}

static DWORD
rdp_verify_changed_certificate (freerdp *instance, const char *host, UINT16 port,
                                const char *common_name, const char *subject,
                                const char *issuer, const char *new_fingerprint,
                                const char *old_subject, const char *old_issuer,
                                const char *old_fingerprint, DWORD flags)
{
  DrSession *session = DR_SESSION (SESSION (instance->context));
  g_autofree char *body = NULL;

  body = g_strdup_printf ("The identity of %s changed since the last connection. If you didn't "
                          "reinstall or reconfigure it, someone may be intercepting the "
                          "connection.\n\nNew fingerprint: %s\nOld fingerprint: %s",
                          host, new_fingerprint, old_fingerprint);
  return dr_session_ask_certificate (session, "Computer Identity Changed", body, TRUE);
}

static BOOL
rdp_client_new (freerdp *instance, rdpContext *context)
{
  instance->PreConnect = rdp_pre_connect;
  instance->PostConnect = rdp_post_connect;
  instance->PostDisconnect = rdp_post_disconnect;
  instance->AuthenticateEx = rdp_authenticate;
  instance->VerifyCertificateEx = rdp_verify_certificate;
  instance->VerifyChangedCertificateEx = rdp_verify_changed_certificate;
  instance->LoadChannels = freerdp_client_load_channels;
  return TRUE;
}

/* Input */

static void
send_input (DrRdpSession *self, rdpInput *input, const DrInputEvent *e)
{
  UINT16 flags = 0;

  switch (e->type)
    {
    case DR_INPUT_MOTION:
      freerdp_input_send_mouse_event (input, PTR_FLAGS_MOVE, e->x, e->y);
      break;

    case DR_INPUT_BUTTON:
      switch (e->button)
        {
        case GDK_BUTTON_PRIMARY: flags = PTR_FLAGS_BUTTON1; break;
        case GDK_BUTTON_SECONDARY: flags = PTR_FLAGS_BUTTON2; break;
        case GDK_BUTTON_MIDDLE: flags = PTR_FLAGS_BUTTON3; break;
        case 8:
        case 9:
          freerdp_input_send_extended_mouse_event (input,
                                                   (e->button == 8 ? PTR_XFLAGS_BUTTON1
                                                                   : PTR_XFLAGS_BUTTON2) |
                                                   (e->down ? PTR_XFLAGS_DOWN : 0),
                                                   e->x, e->y);
          return;
        default:
          return;
        }
      freerdp_input_send_mouse_event (input, flags | (e->down ? PTR_FLAGS_DOWN : 0), e->x, e->y);
      break;

    case DR_INPUT_SCROLL:
      for (int i = 0; i < ABS (e->dy); i++)
        freerdp_input_send_mouse_event (input,
                                        e->dy < 0 ? PTR_FLAGS_WHEEL | 0x78
                                                  : PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x88,
                                        e->x, e->y);
      for (int i = 0; i < ABS (e->dx); i++)
        freerdp_input_send_mouse_event (input,
                                        e->dx > 0 ? PTR_FLAGS_HWHEEL | 0x78
                                                  : PTR_FLAGS_HWHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x88,
                                        e->x, e->y);
      break;

    case DR_INPUT_KEY:
      {
        guint32 scancode = scancode_from_keycode (e->keycode);
        if (scancode != 0)
          freerdp_input_send_keyboard_event_ex (input, e->down, FALSE, scancode);
      }
      break;

    case DR_INPUT_RESIZE:
      self->wanted_width = CLAMP (e->x & ~1, 200, 8192);
      self->wanted_height = CLAMP (e->y, 200, 8192);
      self->wanted_scale = e->scale;
      send_wanted_size (self);
      break;

    case DR_INPUT_CLIPBOARD:
      break;
    }
}

/* Errors */

static void
set_connect_error (DrRdpSession *self, rdpContext *context, GError **error)
{
  DrSession *session = DR_SESSION (self);
  UINT32 code = freerdp_get_last_error (context);
  const char *host = dr_session_get_host (session);

  switch (code)
    {
    case FREERDP_ERROR_AUTHENTICATION_FAILED:
    case FREERDP_ERROR_CONNECT_LOGON_FAILURE:
    case FREERDP_ERROR_CONNECT_WRONG_PASSWORD:
      g_set_error_literal (error, DR_SESSION_ERROR, DR_SESSION_ERROR_AUTH,
                           "The user name or password is incorrect.");
      break;
    case FREERDP_ERROR_CONNECT_PASSWORD_EXPIRED:
    case FREERDP_ERROR_CONNECT_PASSWORD_MUST_CHANGE:
      g_set_error_literal (error, DR_SESSION_ERROR, DR_SESSION_ERROR_AUTH,
                           "The password has expired. Sign in on the computer itself to change it.");
      break;
    case FREERDP_ERROR_CONNECT_ACCOUNT_RESTRICTION:
    case FREERDP_ERROR_CONNECT_ACCOUNT_DISABLED:
      g_set_error_literal (error, DR_SESSION_ERROR, DR_SESSION_ERROR_AUTH,
                           "This account isn't allowed to sign in remotely.");
      break;
    case FREERDP_ERROR_DNS_NAME_NOT_FOUND:
      g_set_error (error, DR_SESSION_ERROR, DR_SESSION_ERROR_FAILED,
                   "Couldn't find \"%s\". Check the address.", host);
      break;
    case FREERDP_ERROR_CONNECT_CANCELLED:
      g_set_error_literal (error, DR_SESSION_ERROR, DR_SESSION_ERROR_CANCELLED,
                           "The connection was cancelled.");
      break;
    case FREERDP_ERROR_CONNECT_FAILED:
    case FREERDP_ERROR_CONNECT_TRANSPORT_FAILED:
      g_set_error (error, DR_SESSION_ERROR, DR_SESSION_ERROR_FAILED,
                   "Couldn't reach %s on port %u. Check that the computer is on and that "
                   "Remote Desktop is turned on.", host, dr_session_get_port (session));
      break;
    case FREERDP_ERROR_SUCCESS:
      break;
    default:
      g_set_error (error, DR_SESSION_ERROR, DR_SESSION_ERROR_FAILED, "%s",
                   freerdp_get_last_error_string (code));
    }
}

/* Session thread */

static gboolean
dr_rdp_session_run (DrSession *session, GError **error)
{
  DrRdpSession *self = DR_RDP_SESSION (session);
  RDP_CLIENT_ENTRY_POINTS entry = { 0 };
  rdpContext *context;
  rdpSettings *settings;
  freerdp *instance;
  int width, height, scale;
  gboolean ok = TRUE;
  DrInputEvent *event;

  entry.Size = sizeof (RDP_CLIENT_ENTRY_POINTS_V1);
  entry.Version = RDP_CLIENT_INTERFACE_VERSION;
  entry.ContextSize = sizeof (DrRdpContext);
  entry.ClientNew = rdp_client_new;

  context = freerdp_client_context_new (&entry);
  if (context == NULL)
    {
      g_set_error_literal (error, DR_SESSION_ERROR, DR_SESSION_ERROR_FAILED,
                           "Couldn't start the RDP client.");
      return FALSE;
    }
  ((DrRdpContext *) context)->session = self;
  instance = context->instance;
  settings = context->settings;

  dr_session_get_initial_size (session, &width, &height, &scale);
  self->wanted_width = self->sent_width = CLAMP (width & ~1, 200, 8192);
  self->wanted_height = self->sent_height = CLAMP (height, 200, 8192);
  self->wanted_scale = scale;

  freerdp_settings_set_string (settings, FreeRDP_ServerHostname, dr_session_get_host (session));
  freerdp_settings_set_uint32 (settings, FreeRDP_ServerPort, dr_session_get_port (session));
  freerdp_settings_set_string (settings, FreeRDP_Username, dr_session_get_username (session));
  freerdp_settings_set_string (settings, FreeRDP_Password, dr_session_get_password (session));
  freerdp_settings_set_string (settings, FreeRDP_Domain, dr_session_get_domain (session));
  freerdp_settings_set_uint32 (settings, FreeRDP_DesktopWidth, self->wanted_width);
  freerdp_settings_set_uint32 (settings, FreeRDP_DesktopHeight, self->wanted_height);
  if (scale > 100)
    {
      freerdp_settings_set_uint32 (settings, FreeRDP_DesktopScaleFactor, CLAMP (scale, 100, 500));
      freerdp_settings_set_uint32 (settings, FreeRDP_DeviceScaleFactor,
                                   scale >= 180 ? 180 : scale >= 140 ? 140 : 100);
    }
  freerdp_settings_set_uint32 (settings, FreeRDP_ColorDepth, 32);
  freerdp_settings_set_bool (settings, FreeRDP_SupportGraphicsPipeline, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_RemoteFxCodec, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_SupportDisplayControl, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_DynamicResolutionUpdate, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_NetworkAutoDetect, TRUE);
  freerdp_settings_set_uint32 (settings, FreeRDP_ConnectionType, CONNECTION_TYPE_AUTODETECT);
  freerdp_settings_set_bool (settings, FreeRDP_AudioPlayback, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_AutoReconnectionEnabled, FALSE);
  /* Home computers have no Kerberos realm; trying it first only adds a delay. */
  freerdp_settings_set_string (settings, FreeRDP_AuthenticationPackageList, "!kerberos");
  freerdp_settings_set_uint32 (settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
  freerdp_settings_set_uint32 (settings, FreeRDP_OsMinorType, OSMINORTYPE_NATIVE_XSERVER);

  g_mutex_lock (&self->context_lock);
  self->context = context;
  g_mutex_unlock (&self->context_lock);

  if (dr_session_should_stop (session) || !freerdp_connect (instance))
    {
      set_connect_error (self, context, error);
      if (error && *error == NULL)
        g_set_error (error, DR_SESSION_ERROR, DR_SESSION_ERROR_FAILED,
                     "Couldn't connect to %s.", dr_session_get_host (session));
      ok = FALSE;
    }
  else
    {
      while (!dr_session_should_stop (session) && !freerdp_shall_disconnect_context (context))
        {
          HANDLE handles[MAXIMUM_WAIT_OBJECTS];
          DWORD count;

          handles[0] = self->wake_event;
          count = freerdp_get_event_handles (context, &handles[1], G_N_ELEMENTS (handles) - 1);
          if (count == 0)
            break;

          if (WaitForMultipleObjects (count + 1, handles, FALSE, 250) == WAIT_FAILED)
            break;

          if (!freerdp_check_event_handles (context))
            {
              set_connect_error (self, context, error);
              break;
            }

          ResetEvent (self->wake_event);
          while ((event = dr_session_pop_input (session)) != NULL)
            {
              send_input (self, context->input, event);
              dr_input_event_free (event);
            }
        }
      freerdp_disconnect (instance);
    }

  g_mutex_lock (&self->context_lock);
  self->context = NULL;
  g_mutex_unlock (&self->context_lock);
  freerdp_client_context_free (context);

  return ok;
}

static void
dr_rdp_session_wake (DrSession *session)
{
  DrRdpSession *self = DR_RDP_SESSION (session);

  g_mutex_lock (&self->context_lock);
  if (self->context && dr_session_should_stop (session))
    freerdp_abort_connect_context (self->context);
  g_mutex_unlock (&self->context_lock);
  SetEvent (self->wake_event);
}

static void
dr_rdp_session_finalize (GObject *object)
{
  DrRdpSession *self = DR_RDP_SESSION (object);

  CloseHandle (self->wake_event);
  g_mutex_clear (&self->context_lock);
  G_OBJECT_CLASS (dr_rdp_session_parent_class)->finalize (object);
}

static void
dr_rdp_session_class_init (DrRdpSessionClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = dr_rdp_session_finalize;
  DR_SESSION_CLASS (klass)->run = dr_rdp_session_run;
  DR_SESSION_CLASS (klass)->wake = dr_rdp_session_wake;
}

static void
dr_rdp_session_init (DrRdpSession *self)
{
  g_mutex_init (&self->context_lock);
  self->wake_event = CreateEventA (NULL, TRUE, FALSE, NULL);
}

DrSession *
dr_rdp_session_new (const char *host, guint port)
{
  return g_object_new (DR_TYPE_RDP_SESSION, "host", host, "port", port ? port : 3389, NULL);
}
