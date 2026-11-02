/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shows the remote screen, scaled to fit, and turns local mouse and keyboard
 * events into DrInputEvents in remote coordinates.
 */
#include "dr-display.h"

struct _DrDisplay
{
  GtkWidget parent_instance;

  DrSession *session;
  gulong frame_handler, cursor_handler, connected_handler;

  GHashTable *pressed_keys; /* keycode -> keyval */
  guint pressed_buttons;    /* bit per GDK button number */
  double scroll_dx, scroll_dy;
  double last_x, last_y;

  int alloc_width, alloc_height;
  guint resize_timeout;
};

G_DEFINE_FINAL_TYPE (DrDisplay, dr_display, GTK_TYPE_WIDGET)

static double
surface_scale (DrDisplay *self)
{
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
  GdkSurface *surface = native ? gtk_native_get_surface (native) : NULL;

  return surface ? gdk_surface_get_scale (surface) : 1.0;
}

/* The rectangle the remote screen is drawn into, in widget coordinates. */
static gboolean
get_view_rect (DrDisplay *self, graphene_rect_t *rect, double *scale)
{
  GdkTexture *texture = dr_session_get_texture (self->session);
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  int height = gtk_widget_get_height (GTK_WIDGET (self));
  int tw, th;
  double s;

  if (texture == NULL || width <= 0 || height <= 0)
    return FALSE;

  tw = gdk_texture_get_width (texture);
  th = gdk_texture_get_height (texture);
  s = MIN ((double) width / tw, (double) height / th);
  /* Draw pixel for pixel when the remote screen fits exactly in device pixels. */
  if (ABS (s * surface_scale (self) - 1.0) < 0.01)
    s = 1.0 / surface_scale (self);

  *scale = s;
  graphene_rect_init (rect, (width - tw * s) / 2, (height - th * s) / 2, tw * s, th * s);
  return TRUE;
}

static void
dr_display_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  DrDisplay *self = DR_DISPLAY (widget);
  GdkTexture *texture = dr_session_get_texture (self->session);
  graphene_rect_t rect;
  double scale;
  GdkRGBA black = { 0, 0, 0, 1 };

  gtk_snapshot_append_color (snapshot, &black,
                             &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget),
                                                  gtk_widget_get_height (widget)));
  if (!get_view_rect (self, &rect, &scale))
    return;

  gtk_snapshot_append_scaled_texture (snapshot, texture,
                                      ABS (scale * surface_scale (self) - 1.0) < 0.01
                                        ? GSK_SCALING_FILTER_NEAREST
                                        : GSK_SCALING_FILTER_LINEAR,
                                      &rect);
}

static gboolean
to_remote (DrDisplay *self, double x, double y, int *rx, int *ry)
{
  GdkTexture *texture = dr_session_get_texture (self->session);
  graphene_rect_t rect;
  double scale;

  if (!get_view_rect (self, &rect, &scale))
    return FALSE;

  *rx = CLAMP ((int) ((x - rect.origin.x) / scale), 0, gdk_texture_get_width (texture) - 1);
  *ry = CLAMP ((int) ((y - rect.origin.y) / scale), 0, gdk_texture_get_height (texture) - 1);
  return TRUE;
}

static void
send_pointer (DrDisplay *self, DrInputType type, guint button, gboolean down, int dx, int dy)
{
  DrInputEvent event = { .type = type, .button = button, .down = down, .dx = dx, .dy = dy };

  if (to_remote (self, self->last_x, self->last_y, &event.x, &event.y))
    dr_session_send (self->session, &event);
}

static void
release_everything (DrDisplay *self)
{
  GHashTableIter iter;
  gpointer keycode, keyval;

  g_hash_table_iter_init (&iter, self->pressed_keys);
  while (g_hash_table_iter_next (&iter, &keycode, &keyval))
    {
      DrInputEvent event = { .type = DR_INPUT_KEY, .down = FALSE,
                             .keycode = GPOINTER_TO_UINT (keycode),
                             .keyval = GPOINTER_TO_UINT (keyval) };
      dr_session_send (self->session, &event);
    }
  g_hash_table_remove_all (self->pressed_keys);

  for (guint button = 1; button < 32; button++)
    if (self->pressed_buttons & (1u << button))
      send_pointer (self, DR_INPUT_BUTTON, button, FALSE, 0, 0);
  self->pressed_buttons = 0;
}

static gboolean
on_legacy_event (GtkEventControllerLegacy *controller, GdkEvent *event, DrDisplay *self)
{
  GdkEventType type = gdk_event_get_event_type (event);
  double x, y;
  guint button;

  if (type != GDK_MOTION_NOTIFY && type != GDK_BUTTON_PRESS && type != GDK_BUTTON_RELEASE)
    return FALSE;

  /* Event positions are relative to the surface; make them widget-relative. */
  if (gdk_event_get_position (event, &x, &y))
    {
      GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
      graphene_point_t p;
      double nx, ny;

      gtk_native_get_surface_transform (native, &nx, &ny);
      if (gtk_widget_compute_point (GTK_WIDGET (native), GTK_WIDGET (self),
                                    &GRAPHENE_POINT_INIT (x - nx, y - ny), &p))
        {
          self->last_x = p.x;
          self->last_y = p.y;
        }
    }

  if (type == GDK_MOTION_NOTIFY)
    {
      send_pointer (self, DR_INPUT_MOTION, 0, FALSE, 0, 0);
      return FALSE;
    }

  button = gdk_button_event_get_button (event);
  if (button >= 32)
    return TRUE;

  if (type == GDK_BUTTON_PRESS)
    {
      gtk_widget_grab_focus (GTK_WIDGET (self));
      self->pressed_buttons |= 1u << button;
      send_pointer (self, DR_INPUT_BUTTON, button, TRUE, 0, 0);
    }
  else if (self->pressed_buttons & (1u << button))
    {
      self->pressed_buttons &= ~(1u << button);
      send_pointer (self, DR_INPUT_BUTTON, button, FALSE, 0, 0);
    }
  return TRUE;
}

static gboolean
on_scroll (GtkEventControllerScroll *controller, double dx, double dy, DrDisplay *self)
{
  int notches_x, notches_y;

  self->scroll_dx += dx;
  self->scroll_dy += dy;
  notches_x = (int) self->scroll_dx;
  notches_y = (int) self->scroll_dy;
  self->scroll_dx -= notches_x;
  self->scroll_dy -= notches_y;

  if (notches_x != 0 || notches_y != 0)
    send_pointer (self, DR_INPUT_SCROLL, 0, FALSE, notches_x, notches_y);
  return TRUE;
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller, guint keyval, guint keycode,
                GdkModifierType state, DrDisplay *self)
{
  DrInputEvent event = { .type = DR_INPUT_KEY, .down = TRUE,
                         .keycode = keycode, .keyval = keyval };

  g_hash_table_insert (self->pressed_keys, GUINT_TO_POINTER (keycode), GUINT_TO_POINTER (keyval));
  dr_session_send (self->session, &event);
  return TRUE;
}

static void
on_key_released (GtkEventControllerKey *controller, guint keyval, guint keycode,
                 GdkModifierType state, DrDisplay *self)
{
  DrInputEvent event = { .type = DR_INPUT_KEY, .down = FALSE,
                         .keycode = keycode, .keyval = keyval };

  if (g_hash_table_remove (self->pressed_keys, GUINT_TO_POINTER (keycode)))
    dr_session_send (self->session, &event);
}

static void
on_focus_leave (GtkEventControllerFocus *controller, DrDisplay *self)
{
  release_everything (self);
}

/* Ask the remote desktop to match the window (RDP only; VNC ignores it). */
static gboolean
send_size (gpointer data)
{
  DrDisplay *self = data;
  double scale = surface_scale (self);
  DrInputEvent event = { .type = DR_INPUT_RESIZE };

  self->resize_timeout = 0;
  if (self->alloc_width <= 0 || self->alloc_height <= 0)
    return G_SOURCE_REMOVE;

  event.x = (int) (self->alloc_width * scale);
  event.y = (int) (self->alloc_height * scale);
  event.scale = (int) (scale * 100);
  dr_session_send (self->session, &event);
  return G_SOURCE_REMOVE;
}

static void
queue_send_size (DrDisplay *self)
{
  g_clear_handle_id (&self->resize_timeout, g_source_remove);
  self->resize_timeout = g_timeout_add (300, send_size, self);
}

static void
dr_display_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  DrDisplay *self = DR_DISPLAY (widget);

  if (width != self->alloc_width || height != self->alloc_height)
    {
      self->alloc_width = width;
      self->alloc_height = height;
      queue_send_size (self);
    }
}

static void
on_frame_changed (DrDisplay *self)
{
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_cursor_changed (DrDisplay *self)
{
  gtk_widget_set_cursor (GTK_WIDGET (self), dr_session_get_cursor (self->session));
}

static void
on_connected (DrDisplay *self)
{
  queue_send_size (self);
}

static void
dr_display_dispose (GObject *object)
{
  DrDisplay *self = DR_DISPLAY (object);

  g_clear_handle_id (&self->resize_timeout, g_source_remove);
  if (self->session)
    {
      g_clear_signal_handler (&self->frame_handler, self->session);
      g_clear_signal_handler (&self->cursor_handler, self->session);
      g_clear_signal_handler (&self->connected_handler, self->session);
      g_clear_object (&self->session);
    }
  g_clear_pointer (&self->pressed_keys, g_hash_table_unref);
  G_OBJECT_CLASS (dr_display_parent_class)->dispose (object);
}

static void
dr_display_class_init (DrDisplayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = dr_display_dispose;
  widget_class->snapshot = dr_display_snapshot;
  widget_class->size_allocate = dr_display_size_allocate;
}

static void
dr_display_init (DrDisplay *self)
{
  GtkEventController *controller;

  self->pressed_keys = g_hash_table_new (NULL, NULL);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_set_focus_on_click (GTK_WIDGET (self), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);

  controller = gtk_event_controller_legacy_new ();
  g_signal_connect (controller, "event", G_CALLBACK (on_legacy_event), self);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  controller = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect (controller, "scroll", G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  controller = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect (controller, "key-pressed", G_CALLBACK (on_key_pressed), self);
  g_signal_connect (controller, "key-released", G_CALLBACK (on_key_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  controller = gtk_event_controller_focus_new ();
  g_signal_connect (controller, "leave", G_CALLBACK (on_focus_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);
}

GtkWidget *
dr_display_new (DrSession *session)
{
  DrDisplay *self = g_object_new (DR_TYPE_DISPLAY, NULL);

  self->session = g_object_ref (session);
  self->frame_handler = g_signal_connect_swapped (session, "frame-changed",
                                                  G_CALLBACK (on_frame_changed), self);
  self->cursor_handler = g_signal_connect_swapped (session, "cursor-changed",
                                                   G_CALLBACK (on_cursor_changed), self);
  self->connected_handler = g_signal_connect_swapped (session, "connected",
                                                      G_CALLBACK (on_connected), self);
  return GTK_WIDGET (self);
}
