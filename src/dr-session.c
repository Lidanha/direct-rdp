/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The protocol-neutral half of a connection. A subclass (RDP or VNC) runs the
 * protocol on its own thread; this class moves frames, cursors, prompts and
 * input between that thread and the GTK main thread.
 */
#include "dr-session.h"

G_DEFINE_QUARK (dr-session-error-quark, dr_session_error)

typedef struct
{
  char *host;
  guint port;
  char *username;
  char *password;
  char *domain;

  GThread *thread;
  gint stop;
  gboolean connected;
  int initial_width, initial_height, initial_scale;
  GAsyncQueue *input;

  GMutex frame_lock;
  guint8 *frame;
  int frame_width, frame_height;
  gsize frame_stride;
  GdkMemoryFormat frame_format;
  gboolean frame_dirty;
  guint frame_idle;
  GdkTexture *texture;

  GMutex cursor_lock;
  GBytes *cursor_bytes;
  char *cursor_name;
  int cursor_width, cursor_height, cursor_hot_x, cursor_hot_y;
  guint cursor_idle;
  GdkCursor *cursor;

  GMutex prompt_lock;
  GCond prompt_cond;
  gboolean prompt_answered;
  gboolean prompt_ok;
  DrCertAnswer cert_answer;
  char *answer_username, *answer_password, *answer_domain;
} DrSessionPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (DrSession, dr_session, G_TYPE_OBJECT)

enum { PROP_0, PROP_HOST, PROP_PORT, N_PROPS };
static GParamSpec *props[N_PROPS];

enum
{
  SIGNAL_CONNECTED,
  SIGNAL_DISCONNECTED,
  SIGNAL_FRAME_CHANGED,
  SIGNAL_CURSOR_CHANGED,
  SIGNAL_CREDENTIALS_REQUESTED,
  SIGNAL_CERTIFICATE_REQUESTED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void
dr_session_finalize (GObject *object)
{
  DrSession *self = DR_SESSION (object);
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  g_free (priv->host);
  g_free (priv->username);
  g_free (priv->password);
  g_free (priv->domain);
  g_async_queue_unref (priv->input);
  g_free (priv->frame);
  g_clear_object (&priv->texture);
  g_clear_pointer (&priv->cursor_bytes, g_bytes_unref);
  g_free (priv->cursor_name);
  g_clear_object (&priv->cursor);
  g_free (priv->answer_username);
  g_free (priv->answer_password);
  g_free (priv->answer_domain);
  g_mutex_clear (&priv->frame_lock);
  g_mutex_clear (&priv->cursor_lock);
  g_mutex_clear (&priv->prompt_lock);
  g_cond_clear (&priv->prompt_cond);

  G_OBJECT_CLASS (dr_session_parent_class)->finalize (object);
}

static void
dr_session_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (DR_SESSION (object));

  switch (prop_id)
    {
    case PROP_HOST:
      g_value_set_string (value, priv->host);
      break;
    case PROP_PORT:
      g_value_set_uint (value, priv->port);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
dr_session_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (DR_SESSION (object));

  switch (prop_id)
    {
    case PROP_HOST:
      g_free (priv->host);
      priv->host = g_value_dup_string (value);
      break;
    case PROP_PORT:
      priv->port = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
dr_session_class_init (DrSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = dr_session_finalize;
  object_class->get_property = dr_session_get_property;
  object_class->set_property = dr_session_set_property;

  props[PROP_HOST] = g_param_spec_string ("host", NULL, NULL, NULL,
                                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                          G_PARAM_STATIC_STRINGS);
  props[PROP_PORT] = g_param_spec_uint ("port", NULL, NULL, 0, 65535, 0,
                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                        G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);

  signals[SIGNAL_CONNECTED] =
    g_signal_new ("connected", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  /* The GError is NULL when the user disconnected. */
  signals[SIGNAL_DISCONNECTED] =
    g_signal_new ("disconnected", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);
  signals[SIGNAL_FRAME_CHANGED] =
    g_signal_new ("frame-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_CURSOR_CHANGED] =
    g_signal_new ("cursor-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  /* (message, need_username); answer with dr_session_answer_credentials() */
  signals[SIGNAL_CREDENTIALS_REQUESTED] =
    g_signal_new ("credentials-requested", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
  /* (heading, body, changed); answer with dr_session_answer_certificate() */
  signals[SIGNAL_CERTIFICATE_REQUESTED] =
    g_signal_new ("certificate-requested", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
}

static void
dr_session_init (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  priv->input = g_async_queue_new_full (g_free);
  g_mutex_init (&priv->frame_lock);
  g_mutex_init (&priv->cursor_lock);
  g_mutex_init (&priv->prompt_lock);
  g_cond_init (&priv->prompt_cond);
}

const char *
dr_session_get_host (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  return priv->host;
}

guint
dr_session_get_port (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  return priv->port;
}

const char *
dr_session_get_username (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  return priv->username;
}

const char *
dr_session_get_password (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  return priv->password;
}

const char *
dr_session_get_domain (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  return priv->domain;
}

void
dr_session_set_credentials (DrSession *self, const char *username,
                            const char *password, const char *domain)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  g_free (priv->username);
  g_free (priv->password);
  g_free (priv->domain);
  priv->username = g_strdup (username && *username ? username : NULL);
  priv->password = g_strdup (password && *password ? password : NULL);
  priv->domain = g_strdup (domain && *domain ? domain : NULL);
}

gboolean
dr_session_is_connected (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  return priv->connected;
}

/* Main-thread dispatch */

typedef struct
{
  DrSession *self;
  GError *error;
  char *a, *b;
  gboolean flag;
} Dispatch;

static void
dispatch_free (gpointer data)
{
  Dispatch *d = data;

  g_object_unref (d->self);
  g_clear_error (&d->error);
  g_free (d->a);
  g_free (d->b);
  g_free (d);
}

static void
dispatch (DrSession *self, GSourceFunc func, Dispatch *d)
{
  d->self = g_object_ref (self);
  g_idle_add_full (G_PRIORITY_DEFAULT, func, d, dispatch_free);
}

static gboolean
emit_connected (gpointer data)
{
  Dispatch *d = data;
  DrSessionPrivate *priv = dr_session_get_instance_private (d->self);

  if (!g_atomic_int_get (&priv->stop))
    {
      priv->connected = TRUE;
      g_signal_emit (d->self, signals[SIGNAL_CONNECTED], 0);
    }
  return G_SOURCE_REMOVE;
}

static gboolean
emit_disconnected (gpointer data)
{
  Dispatch *d = data;
  DrSessionPrivate *priv = dr_session_get_instance_private (d->self);

  g_clear_pointer (&priv->thread, g_thread_join);
  priv->connected = FALSE;
  g_signal_emit (d->self, signals[SIGNAL_DISCONNECTED], 0, d->error);
  return G_SOURCE_REMOVE;
}

static gpointer
session_thread (gpointer data)
{
  DrSession *self = data;
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  g_autoptr (GError) error = NULL;
  gboolean ok;
  Dispatch *d;

  ok = DR_SESSION_GET_CLASS (self)->run (self, &error);
  if (g_atomic_int_get (&priv->stop))
    g_clear_error (&error);
  else if (!ok && error == NULL)
    g_set_error_literal (&error, DR_SESSION_ERROR, DR_SESSION_ERROR_FAILED,
                         "The connection failed.");
  else if (ok && error == NULL)
    g_set_error_literal (&error, DR_SESSION_ERROR, DR_SESSION_ERROR_FAILED,
                         "The remote computer ended the session.");

  d = g_new0 (Dispatch, 1);
  d->error = g_steal_pointer (&error);
  dispatch (self, emit_disconnected, d);
  g_object_unref (self); /* the thread's own reference, taken in dr_session_start() */
  return NULL;
}

void
dr_session_start (DrSession *self, int width, int height, int scale)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  g_return_if_fail (priv->thread == NULL);

  priv->initial_width = width;
  priv->initial_height = height;
  priv->initial_scale = scale;
  g_atomic_int_set (&priv->stop, 0);
  priv->thread = g_thread_new ("dr-session", session_thread, g_object_ref (self));
}

void
dr_session_disconnect (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  if (priv->thread == NULL)
    return;

  g_atomic_int_set (&priv->stop, 1);
  g_mutex_lock (&priv->prompt_lock);
  g_cond_broadcast (&priv->prompt_cond);
  g_mutex_unlock (&priv->prompt_lock);
  DR_SESSION_GET_CLASS (self)->wake (self);
}

gboolean
dr_session_should_stop (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  return g_atomic_int_get (&priv->stop);
}

void
dr_session_get_initial_size (DrSession *self, int *width, int *height, int *scale)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  *width = priv->initial_width;
  *height = priv->initial_height;
  *scale = priv->initial_scale;
}

void
dr_session_notify_connected (DrSession *self)
{
  dispatch (self, emit_connected, g_new0 (Dispatch, 1));
}

/* Input */

void
dr_session_send (DrSession *self, const DrInputEvent *event)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  if (!priv->connected)
    return;
  g_async_queue_push (priv->input, g_memdup2 (event, sizeof *event));
  DR_SESSION_GET_CLASS (self)->wake (self);
}

void
dr_session_send_keys (DrSession *self, const guint *keycodes, const guint *keyvals, guint n)
{
  DrInputEvent event = { .type = DR_INPUT_KEY };

  for (guint i = 0; i < n; i++)
    {
      event.keycode = keycodes[i];
      event.keyval = keyvals[i];
      event.down = TRUE;
      dr_session_send (self, &event);
    }
  for (guint i = n; i > 0; i--)
    {
      event.keycode = keycodes[i - 1];
      event.keyval = keyvals[i - 1];
      event.down = FALSE;
      dr_session_send (self, &event);
    }
}

DrInputEvent *
dr_session_pop_input (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  return g_async_queue_try_pop (priv->input);
}

/* Frames */

static gboolean
emit_frame_changed (gpointer data)
{
  DrSession *self = data;
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  g_mutex_lock (&priv->frame_lock);
  priv->frame_idle = 0;
  g_mutex_unlock (&priv->frame_lock);
  g_signal_emit (self, signals[SIGNAL_FRAME_CHANGED], 0);
  return G_SOURCE_REMOVE;
}

void
dr_session_push_frame (DrSession *self, const guint8 *data, int width, int height,
                       gsize stride, GdkMemoryFormat format,
                       int x, int y, int w, int h)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  if (width <= 0 || height <= 0)
    return;

  g_mutex_lock (&priv->frame_lock);

  if (priv->frame == NULL || width != priv->frame_width || height != priv->frame_height ||
      format != priv->frame_format)
    {
      g_free (priv->frame);
      priv->frame_width = width;
      priv->frame_height = height;
      priv->frame_stride = (gsize) width * 4;
      priv->frame_format = format;
      priv->frame = g_malloc (priv->frame_stride * height);
      x = y = 0;
      w = width;
      h = height;
    }

  x = CLAMP (x, 0, width);
  y = CLAMP (y, 0, height);
  w = CLAMP (w, 0, width - x);
  h = CLAMP (h, 0, height - y);
  for (int row = y; row < y + h; row++)
    memcpy (priv->frame + row * priv->frame_stride + (gsize) x * 4,
            data + row * stride + (gsize) x * 4, (gsize) w * 4);

  priv->frame_dirty = TRUE;
  if (priv->frame_idle == 0)
    priv->frame_idle = g_idle_add_full (G_PRIORITY_DEFAULT, emit_frame_changed,
                                        g_object_ref (self), g_object_unref);

  g_mutex_unlock (&priv->frame_lock);
}

GdkTexture *
dr_session_get_texture (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  g_mutex_lock (&priv->frame_lock);
  if (priv->frame_dirty && priv->frame != NULL)
    {
      g_autoptr (GBytes) bytes = g_bytes_new (priv->frame,
                                              priv->frame_stride * priv->frame_height);

      g_clear_object (&priv->texture);
      priv->texture = gdk_memory_texture_new (priv->frame_width, priv->frame_height,
                                              priv->frame_format, bytes, priv->frame_stride);
      priv->frame_dirty = FALSE;
    }
  g_mutex_unlock (&priv->frame_lock);

  return priv->texture;
}

/* Cursor */

static gboolean
emit_cursor_changed (gpointer data)
{
  DrSession *self = data;
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  g_mutex_lock (&priv->cursor_lock);
  priv->cursor_idle = 0;
  g_clear_object (&priv->cursor);
  if (priv->cursor_bytes != NULL)
    {
      g_autoptr (GdkTexture) texture =
        gdk_memory_texture_new (priv->cursor_width, priv->cursor_height,
                                GDK_MEMORY_B8G8R8A8, priv->cursor_bytes,
                                (gsize) priv->cursor_width * 4);
      priv->cursor = gdk_cursor_new_from_texture (texture, priv->cursor_hot_x,
                                                  priv->cursor_hot_y, NULL);
    }
  else if (priv->cursor_name != NULL)
    {
      priv->cursor = gdk_cursor_new_from_name (priv->cursor_name, NULL);
    }
  g_mutex_unlock (&priv->cursor_lock);

  g_signal_emit (self, signals[SIGNAL_CURSOR_CHANGED], 0);
  return G_SOURCE_REMOVE;
}

static void
queue_cursor_changed (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  if (priv->cursor_idle == 0)
    priv->cursor_idle = g_idle_add_full (G_PRIORITY_DEFAULT, emit_cursor_changed,
                                         g_object_ref (self), g_object_unref);
}

void
dr_session_push_cursor (DrSession *self, const guint8 *bgra, int width, int height,
                        int hot_x, int hot_y)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  if (width <= 0 || height <= 0)
    return;

  g_mutex_lock (&priv->cursor_lock);
  g_clear_pointer (&priv->cursor_bytes, g_bytes_unref);
  g_clear_pointer (&priv->cursor_name, g_free);
  priv->cursor_bytes = g_bytes_new (bgra, (gsize) width * height * 4);
  priv->cursor_width = width;
  priv->cursor_height = height;
  priv->cursor_hot_x = CLAMP (hot_x, 0, width - 1);
  priv->cursor_hot_y = CLAMP (hot_y, 0, height - 1);
  queue_cursor_changed (self);
  g_mutex_unlock (&priv->cursor_lock);
}

void
dr_session_push_cursor_named (DrSession *self, const char *name)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  g_mutex_lock (&priv->cursor_lock);
  g_clear_pointer (&priv->cursor_bytes, g_bytes_unref);
  g_free (priv->cursor_name);
  priv->cursor_name = g_strdup (name);
  queue_cursor_changed (self);
  g_mutex_unlock (&priv->cursor_lock);
}

GdkCursor *
dr_session_get_cursor (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  return priv->cursor;
}

/* Prompts: the session thread blocks until the main thread answers. */

static gboolean
emit_credentials_requested (gpointer data)
{
  Dispatch *d = data;

  if (!dr_session_should_stop (d->self))
    g_signal_emit (d->self, signals[SIGNAL_CREDENTIALS_REQUESTED], 0, d->a, d->flag);
  return G_SOURCE_REMOVE;
}

static gboolean
emit_certificate_requested (gpointer data)
{
  Dispatch *d = data;

  if (!dr_session_should_stop (d->self))
    g_signal_emit (d->self, signals[SIGNAL_CERTIFICATE_REQUESTED], 0, d->a, d->b, d->flag);
  return G_SOURCE_REMOVE;
}

static void
wait_for_answer (DrSession *self)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  while (!priv->prompt_answered && !dr_session_should_stop (self))
    g_cond_wait (&priv->prompt_cond, &priv->prompt_lock);
}

gboolean
dr_session_ask_credentials (DrSession *self, const char *message, gboolean need_username,
                            char **username, char **password, char **domain)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  Dispatch *d = g_new0 (Dispatch, 1);
  gboolean ok;

  g_mutex_lock (&priv->prompt_lock);
  priv->prompt_answered = FALSE;
  d->a = g_strdup (message);
  d->flag = need_username;
  dispatch (self, emit_credentials_requested, d);

  wait_for_answer (self);
  ok = priv->prompt_answered && priv->prompt_ok && !dr_session_should_stop (self);
  if (ok)
    {
      *username = g_steal_pointer (&priv->answer_username);
      *password = g_steal_pointer (&priv->answer_password);
      *domain = g_steal_pointer (&priv->answer_domain);
    }
  g_mutex_unlock (&priv->prompt_lock);

  return ok;
}

void
dr_session_answer_credentials (DrSession *self, gboolean ok, const char *username,
                               const char *password, const char *domain)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  if (ok)
    dr_session_set_credentials (self, username, password, domain);

  g_mutex_lock (&priv->prompt_lock);
  priv->prompt_answered = TRUE;
  priv->prompt_ok = ok;
  g_free (priv->answer_username);
  g_free (priv->answer_password);
  g_free (priv->answer_domain);
  priv->answer_username = g_strdup (username ? username : "");
  priv->answer_password = g_strdup (password ? password : "");
  priv->answer_domain = g_strdup (domain ? domain : "");
  g_cond_broadcast (&priv->prompt_cond);
  g_mutex_unlock (&priv->prompt_lock);
}

DrCertAnswer
dr_session_ask_certificate (DrSession *self, const char *heading, const char *body,
                            gboolean changed)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);
  Dispatch *d = g_new0 (Dispatch, 1);
  DrCertAnswer answer;

  g_mutex_lock (&priv->prompt_lock);
  priv->prompt_answered = FALSE;
  priv->cert_answer = DR_CERT_REJECT;
  d->a = g_strdup (heading);
  d->b = g_strdup (body);
  d->flag = changed;
  dispatch (self, emit_certificate_requested, d);

  wait_for_answer (self);
  answer = dr_session_should_stop (self) ? DR_CERT_REJECT : priv->cert_answer;
  g_mutex_unlock (&priv->prompt_lock);

  return answer;
}

void
dr_session_answer_certificate (DrSession *self, DrCertAnswer answer)
{
  DrSessionPrivate *priv = dr_session_get_instance_private (self);

  g_mutex_lock (&priv->prompt_lock);
  priv->prompt_answered = TRUE;
  priv->cert_answer = answer;
  g_cond_broadcast (&priv->prompt_cond);
  g_mutex_unlock (&priv->prompt_lock);
}
