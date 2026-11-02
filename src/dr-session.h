/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DR_SESSION_ERROR (dr_session_error_quark ())

typedef enum
{
  DR_SESSION_ERROR_FAILED,
  DR_SESSION_ERROR_AUTH,      /* wrong user name or password */
  DR_SESSION_ERROR_CANCELLED, /* the user gave up at a prompt */
} DrSessionError;

GQuark dr_session_error_quark (void);

/* Answers to a certificate prompt; the values match FreeRDP's. */
typedef enum
{
  DR_CERT_REJECT = 0,
  DR_CERT_ACCEPT_PERMANENTLY = 1,
  DR_CERT_ACCEPT_ONCE = 2,
} DrCertAnswer;

typedef enum
{
  DR_INPUT_MOTION,
  DR_INPUT_BUTTON,
  DR_INPUT_SCROLL,
  DR_INPUT_KEY,
  DR_INPUT_RESIZE,
} DrInputType;

/* One input event, queued on the main thread and sent from the session's thread. */
typedef struct
{
  DrInputType type;
  int x, y;          /* remote pixels (motion, button, scroll); new size (resize) */
  guint button;      /* GDK button number */
  gboolean down;     /* button, key */
  int dx, dy;        /* scroll, in wheel notches */
  guint keycode;     /* key: XKB keycode (evdev + 8) */
  guint keyval;      /* key: X keysym */
  int scale;         /* resize: desktop scale in percent */
} DrInputEvent;

#define DR_TYPE_SESSION (dr_session_get_type ())
G_DECLARE_DERIVABLE_TYPE (DrSession, dr_session, DR, SESSION, GObject)

struct _DrSessionClass
{
  GObjectClass parent_class;

  /* Runs on the session's own thread: connect, then process events until
   * dr_session_should_stop() or the connection ends. */
  gboolean (*run) (DrSession *self, GError **error);
  /* Called from the main thread after dr_session_should_stop() became TRUE and
   * after each queued input event, to wake run() up. */
  void (*wake) (DrSession *self);
};

const char *dr_session_get_host (DrSession *self);
guint dr_session_get_port (DrSession *self);
const char *dr_session_get_username (DrSession *self);
const char *dr_session_get_password (DrSession *self);
const char *dr_session_get_domain (DrSession *self);
void dr_session_set_credentials (DrSession *self, const char *username,
                                 const char *password, const char *domain);
gboolean dr_session_is_connected (DrSession *self);

/* Main thread */
void dr_session_start (DrSession *self, int width, int height, int scale);
void dr_session_disconnect (DrSession *self);
GdkTexture *dr_session_get_texture (DrSession *self);
GdkCursor *dr_session_get_cursor (DrSession *self);
void dr_session_send (DrSession *self, const DrInputEvent *event);
void dr_session_send_keys (DrSession *self, const guint *keycodes, const guint *keyvals, guint n);
void dr_session_answer_credentials (DrSession *self, gboolean ok, const char *username,
                                    const char *password, const char *domain);
void dr_session_answer_certificate (DrSession *self, DrCertAnswer answer);

/* Session thread (used by subclasses) */
gboolean dr_session_should_stop (DrSession *self);
void dr_session_get_initial_size (DrSession *self, int *width, int *height, int *scale);
DrInputEvent *dr_session_pop_input (DrSession *self);
void dr_session_notify_connected (DrSession *self);
void dr_session_push_frame (DrSession *self, const guint8 *data, int width, int height,
                            gsize stride, GdkMemoryFormat format,
                            int x, int y, int w, int h);
void dr_session_push_cursor (DrSession *self, const guint8 *bgra, int width, int height,
                             int hot_x, int hot_y);
void dr_session_push_cursor_named (DrSession *self, const char *name);
gboolean dr_session_ask_credentials (DrSession *self, const char *message,
                                     gboolean need_username,
                                     char **username, char **password, char **domain);
DrCertAnswer dr_session_ask_certificate (DrSession *self, const char *heading,
                                         const char *body, gboolean changed);

G_END_DECLS
