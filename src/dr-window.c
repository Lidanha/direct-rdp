/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The main window: a home page with the address field and saved computers,
 * and one page per open connection.
 */
#include "config.h"
#include "dr-window.h"

#include "dr-connection.h"
#include "dr-display.h"
#include "dr-rdp-session.h"
#include "dr-vnc-session.h"
#include "dr-wol.h"

struct _DrWindow
{
  AdwApplicationWindow parent_instance;

  GPtrArray *connections;

  AdwToastOverlay *toasts;
  AdwNavigationView *nav;
  AdwEntryRow *address_row;
  GtkToggleButton *rdp_toggle;
  GtkToggleButton *vnc_toggle;
  AdwPreferencesGroup *saved_group;
  GtkListBox *saved_list;
};

G_DEFINE_FINAL_TYPE (DrWindow, dr_window, ADW_TYPE_APPLICATION_WINDOW)

/* One open connection page. */
typedef struct
{
  DrWindow *window;
  DrConnection *conn;
  DrSession *session;
  char *password;          /* the password the current attempt uses */
  gboolean remember;       /* save the typed password once connected */
  gboolean was_connected;
  gboolean closed;
  gboolean lookup_pending;  /* a keyring lookup holds the view */

  AdwNavigationPage *page;
  AdwToolbarView *toolbar;
  GtkStack *stack;
  GtkLabel *status_label;
  AdwStatusPage *error_page;
  GtkWidget *wake_button;
  GtkWidget *display;
  AdwDialog *prompt;
} View;

static void view_start (View *view);
static void refresh_saved (DrWindow *self);

static void
toast (DrWindow *self, const char *text)
{
  adw_toast_overlay_add_toast (self->toasts, adw_toast_new (text));
}

static DrConnection *
find_connection (DrWindow *self, const char *id)
{
  for (guint i = 0; i < self->connections->len; i++)
    {
      DrConnection *conn = g_ptr_array_index (self->connections, i);
      if (g_strcmp0 (conn->id, id) == 0)
        return conn;
    }
  return NULL;
}

/* Session lifetime */

static void
view_drop_session (View *view)
{
  if (view->prompt)
    adw_dialog_force_close (view->prompt);
  if (view->session)
    {
      g_signal_handlers_disconnect_by_data (view->session, view);
      dr_session_disconnect (view->session);
      g_clear_object (&view->session);
    }
  if (view->display)
    {
      gtk_stack_remove (view->stack, view->display);
      view->display = NULL;
    }
}

static void
view_free (View *view)
{
  view_drop_session (view);
  dr_connection_free (view->conn);
  g_free (view->password);
  g_free (view);
}

static void
set_fullscreen (View *view, gboolean fullscreen)
{
  GtkWindow *window = GTK_WINDOW (view->window);

  if (fullscreen)
    gtk_window_fullscreen (window);
  else
    gtk_window_unfullscreen (window);
}

static void
on_prompt_closed (AdwDialog *dialog, View *view)
{
  if (view->prompt == dialog)
    view->prompt = NULL;
}

static void
show_prompt (View *view, AdwDialog *dialog)
{
  view->prompt = dialog;
  g_signal_connect (dialog, "closed", G_CALLBACK (on_prompt_closed), view);
  adw_dialog_present (dialog, GTK_WIDGET (view->window));
}

static void
on_credentials_response (AdwAlertDialog *dialog, const char *response, View *view)
{
  AdwEntryRow *user_row = g_object_get_data (G_OBJECT (dialog), "user-row");
  AdwEntryRow *password_row = g_object_get_data (G_OBJECT (dialog), "password-row");
  GtkCheckButton *remember = g_object_get_data (G_OBJECT (dialog), "remember");
  g_autofree char *user = NULL;
  g_autofree char *domain = NULL;
  const char *password;
  char *sep;

  if (view->session == NULL)
    return;

  if (g_strcmp0 (response, "sign-in") != 0)
    {
      dr_session_answer_credentials (view->session, FALSE, NULL, NULL, NULL);
      return;
    }

  /* "DOMAIN\user" carries the domain; "user@domain" is passed through. */
  user = g_strdup (user_row ? gtk_editable_get_text (GTK_EDITABLE (user_row))
                            : dr_session_get_username (view->session));
  if (user && (sep = strchr (user, '\\')) != NULL)
    {
      *sep = '\0';
      domain = g_strdup (user);
      memmove (user, sep + 1, strlen (sep + 1) + 1);
    }
  else if (user_row == NULL)
    {
      domain = g_strdup (dr_session_get_domain (view->session));
    }

  password = gtk_editable_get_text (GTK_EDITABLE (password_row));
  g_free (view->password);
  view->password = g_strdup (password);
  view->remember = gtk_check_button_get_active (remember);
  dr_session_answer_credentials (view->session, TRUE, user, password, domain);
}

static void
on_credentials_requested (DrSession *session, const char *message, gboolean need_username,
                          View *view)
{
  AdwDialog *dialog = adw_alert_dialog_new ("Sign In", message);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *list = gtk_list_box_new ();
  GtkWidget *password_row = adw_password_entry_row_new ();
  GtkWidget *remember = gtk_check_button_new_with_label ("Remember password");
  GtkWidget *focus = password_row;

  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");

  if (need_username)
    {
      GtkWidget *user_row = adw_entry_row_new ();
      const char *user = dr_session_get_username (session);
      const char *domain = dr_session_get_domain (session);
      g_autofree char *full = domain && user ? g_strdup_printf ("%s\\%s", domain, user)
                                             : g_strdup (user);

      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (user_row), "User name");
      gtk_editable_set_text (GTK_EDITABLE (user_row), full ? full : "");
      adw_entry_row_set_activates_default (ADW_ENTRY_ROW (user_row), TRUE);
      gtk_list_box_append (GTK_LIST_BOX (list), user_row);
      g_object_set_data (G_OBJECT (dialog), "user-row", user_row);
      if (full == NULL || *full == '\0')
        focus = user_row;
    }

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (password_row), "Password");
  adw_entry_row_set_activates_default (ADW_ENTRY_ROW (password_row), TRUE);
  gtk_list_box_append (GTK_LIST_BOX (list), password_row);
  g_object_set_data (G_OBJECT (dialog), "password-row", password_row);

  gtk_check_button_set_active (GTK_CHECK_BUTTON (remember), TRUE);
  g_object_set_data (G_OBJECT (dialog), "remember", remember);

  gtk_box_append (GTK_BOX (box), list);
  gtk_box_append (GTK_BOX (box), remember);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), box);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                  "cancel", "_Cancel", "sign-in", "_Sign In", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "sign-in",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "sign-in");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect (dialog, "response", G_CALLBACK (on_credentials_response), view);

  show_prompt (view, dialog);
  gtk_widget_grab_focus (focus);
}

static void
on_certificate_response (AdwAlertDialog *dialog, const char *response, View *view)
{
  if (view->session)
    dr_session_answer_certificate (view->session, g_strcmp0 (response, "connect") == 0
                                                    ? DR_CERT_ACCEPT_PERMANENTLY
                                                    : DR_CERT_REJECT);
}

static void
on_certificate_requested (DrSession *session, const char *heading, const char *body,
                          gboolean changed, View *view)
{
  AdwDialog *dialog = adw_alert_dialog_new (heading, body);

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                  "cancel", "_Cancel", "connect", "C_onnect", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "connect",
                                            changed ? ADW_RESPONSE_DESTRUCTIVE
                                                    : ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog),
                                         changed ? "cancel" : "connect");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect (dialog, "response", G_CALLBACK (on_certificate_response), view);
  show_prompt (view, dialog);
}

static void
on_connected (DrSession *session, View *view)
{
  DrWindow *self = view->window;
  DrConnection *saved = find_connection (self, view->conn->id);

  view->was_connected = TRUE;
  gtk_stack_set_visible_child_name (view->stack, "display");
  gtk_widget_grab_focus (view->display);

  /* Every computer that connects once is remembered. */
  g_free (view->conn->username);
  g_free (view->conn->domain);
  view->conn->username = g_strdup (dr_session_get_username (session));
  view->conn->domain = g_strdup (dr_session_get_domain (session));
  view->conn->last_used = g_get_real_time () / G_USEC_PER_SEC;
  if (saved)
    {
      g_free (saved->username);
      g_free (saved->domain);
      saved->username = g_strdup (view->conn->username);
      saved->domain = g_strdup (view->conn->domain);
      saved->last_used = view->conn->last_used;
    }
  else
    {
      g_ptr_array_add (self->connections, dr_connection_copy (view->conn));
    }
  dr_connections_sort (self->connections);
  dr_connections_save (self->connections);
  refresh_saved (self);

  if (view->remember && view->password && *view->password)
    dr_password_store (view->conn, view->password);
  view->remember = FALSE;
}

static void
on_disconnected (DrSession *session, GError *error, View *view)
{
  gboolean auth = g_error_matches (error, DR_SESSION_ERROR, DR_SESSION_ERROR_AUTH);

  if (view->prompt)
    adw_dialog_force_close (view->prompt);
  if (view->closed || error == NULL)
    return;

  if (gtk_window_is_fullscreen (GTK_WINDOW (view->window)))
    set_fullscreen (view, FALSE);

  if (g_error_matches (error, DR_SESSION_ERROR, DR_SESSION_ERROR_CANCELLED) && !view->was_connected)
    {
      adw_navigation_view_pop (view->window->nav);
      return;
    }

  if (auth)
    {
      /* A saved password that no longer works is forgotten; the next try asks. */
      dr_password_clear (view->conn);
      g_clear_pointer (&view->password, g_free);
    }

  adw_status_page_set_title (view->error_page,
                             auth ? "Couldn't Sign In"
                                  : view->was_connected ? "Disconnected" : "Couldn't Connect");
  adw_status_page_set_description (view->error_page, error->message);
  gtk_widget_set_visible (view->wake_button,
                          !view->was_connected && !auth && view->conn->mac && *view->conn->mac);
  gtk_stack_set_visible_child_name (view->stack, "error");
}

static void
view_start_with_password (View *view, const char *password)
{
  DrWindow *self = view->window;
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
  GdkSurface *surface = native ? gtk_native_get_surface (native) : NULL;
  double scale = surface ? gdk_surface_get_scale (surface) : 1.0;
  int width = gtk_widget_get_width (GTK_WIDGET (view->stack));
  int height = gtk_widget_get_height (GTK_WIDGET (view->stack));
  g_autofree char *status = NULL;

  view_drop_session (view);
  if (password != view->password)
    {
      g_free (view->password);
      view->password = g_strdup (password);
    }

  if (width <= 0 || height <= 0)
    {
      width = gtk_widget_get_width (GTK_WIDGET (self));
      height = gtk_widget_get_height (GTK_WIDGET (self)) - 47;
    }

  if (view->conn->protocol == DR_PROTOCOL_VNC)
    {
      view->session = dr_vnc_session_new (view->conn->host, dr_connection_get_port (view->conn));
      dr_vnc_session_set_adaptive (DR_VNC_SESSION (view->session), view->conn->adaptive);
    }
  else
    view->session = dr_rdp_session_new (view->conn->host, dr_connection_get_port (view->conn));
  dr_session_set_credentials (view->session, view->conn->username, view->password,
                              view->conn->domain);

  g_signal_connect (view->session, "connected", G_CALLBACK (on_connected), view);
  g_signal_connect (view->session, "disconnected", G_CALLBACK (on_disconnected), view);
  g_signal_connect (view->session, "credentials-requested",
                    G_CALLBACK (on_credentials_requested), view);
  g_signal_connect (view->session, "certificate-requested",
                    G_CALLBACK (on_certificate_requested), view);

  view->display = dr_display_new (view->session);
  gtk_stack_add_named (view->stack, view->display, "display");

  status = g_strdup_printf ("Connecting to %s…", view->conn->host);
  gtk_label_set_text (view->status_label, status);
  gtk_stack_set_visible_child_name (view->stack, "connecting");
  view->was_connected = FALSE;

  dr_session_start (view->session, (int) (width * scale), (int) (height * scale),
                    (int) (scale * 100));
}

static void
on_password_looked_up (GObject *source, GAsyncResult *result, gpointer data)
{
  View *view = data;
  g_autofree char *password = dr_password_lookup_finish (result);

  view->lookup_pending = FALSE;
  if (view->closed)
    view_free (view);
  else
    view_start_with_password (view, password);
}

static void
view_start (View *view)
{
  view->lookup_pending = TRUE;
  gtk_label_set_text (view->status_label, "Connecting…");
  gtk_stack_set_visible_child_name (view->stack, "connecting");
  dr_password_lookup (view->conn, NULL, on_password_looked_up, view);
}

/* Session page */

static void
on_retry (GtkButton *button, View *view)
{
  if (view->password)
    view_start_with_password (view, view->password);
  else
    view_start (view);
}

static void
on_wake_clicked (GtkButton *button, View *view)
{
  g_autoptr (GError) error = NULL;

  if (dr_wol_send (view->conn->mac, &error))
    toast (view->window, "Wake-up sent. Try again in a minute.");
  else
    toast (view->window, error->message);
}

static void
send_keys_action (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  View *view = data;
  const char *which = g_variant_get_string (parameter, NULL);
  /* XKB keycodes and keysyms */
  static const guint ctrl_alt_del_codes[] = { 37, 64, 119 };
  static const guint ctrl_alt_del_syms[] = { GDK_KEY_Control_L, GDK_KEY_Alt_L, GDK_KEY_Delete };
  static const guint super_codes[] = { 133 };
  static const guint super_syms[] = { GDK_KEY_Super_L };
  static const guint alt_tab_codes[] = { 64, 23 };
  static const guint alt_tab_syms[] = { GDK_KEY_Alt_L, GDK_KEY_Tab };
  static const guint alt_f4_codes[] = { 64, 70 };
  static const guint alt_f4_syms[] = { GDK_KEY_Alt_L, GDK_KEY_F4 };
  static const guint print_codes[] = { 107 };
  static const guint print_syms[] = { GDK_KEY_Print };

  if (view->session == NULL)
    return;

  if (g_str_equal (which, "ctrl-alt-del"))
    dr_session_send_keys (view->session, ctrl_alt_del_codes, ctrl_alt_del_syms, 3);
  else if (g_str_equal (which, "super"))
    dr_session_send_keys (view->session, super_codes, super_syms, 1);
  else if (g_str_equal (which, "alt-tab"))
    dr_session_send_keys (view->session, alt_tab_codes, alt_tab_syms, 2);
  else if (g_str_equal (which, "alt-f4"))
    dr_session_send_keys (view->session, alt_f4_codes, alt_f4_syms, 2);
  else if (g_str_equal (which, "print"))
    dr_session_send_keys (view->session, print_codes, print_syms, 1);

  if (view->display)
    gtk_widget_grab_focus (view->display);
}

/* A Mac's picture: Full quality is lossless, Adaptive sends a fraction of the
 * data for slow connections. Kept with the saved computer. */
static void
quality_changed (GSimpleAction *action, GVariant *value, gpointer data)
{
  View *view = data;
  gboolean adaptive = g_str_equal (g_variant_get_string (value, NULL), "adaptive");
  DrConnection *saved = find_connection (view->window, view->conn->id);

  g_simple_action_set_state (action, value);
  view->conn->adaptive = adaptive;
  if (DR_IS_VNC_SESSION (view->session))
    dr_vnc_session_set_adaptive (DR_VNC_SESSION (view->session), adaptive);
  if (saved && saved->adaptive != adaptive)
    {
      saved->adaptive = adaptive;
      dr_connections_save (view->window->connections);
    }
}

static void
toggle_fullscreen_action (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  View *view = data;

  set_fullscreen (view, !gtk_window_is_fullscreen (GTK_WINDOW (view->window)));
}

static void
on_fullscreen_changed (GtkWindow *window, GParamSpec *pspec, View *view)
{
  gboolean fullscreen = gtk_window_is_fullscreen (window);
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (window));

  if (view->closed)
    return;

  /* In full screen the header slides over the picture, and system shortcuts
   * such as Super and Alt+Tab go to the remote computer. */
  adw_toolbar_view_set_extend_content_to_top_edge (view->toolbar, fullscreen);
  adw_toolbar_view_set_reveal_top_bars (view->toolbar, !fullscreen);
  if (GDK_IS_TOPLEVEL (surface))
    {
      if (fullscreen)
        gdk_toplevel_inhibit_system_shortcuts (GDK_TOPLEVEL (surface), NULL);
      else
        gdk_toplevel_restore_system_shortcuts (GDK_TOPLEVEL (surface));
    }
}

static void
on_toolbar_motion (GtkEventControllerMotion *controller, double x, double y, View *view)
{
  if (!gtk_window_is_fullscreen (GTK_WINDOW (view->window)))
    return;
  if (y < 4)
    adw_toolbar_view_set_reveal_top_bars (view->toolbar, TRUE);
  else if (y > adw_toolbar_view_get_top_bar_height (view->toolbar) + 24)
    adw_toolbar_view_set_reveal_top_bars (view->toolbar, FALSE);
}

static GtkWidget *
build_connecting_page (View *view)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  GtkWidget *spinner = gtk_spinner_new ();
  GtkWidget *label = gtk_label_new (NULL);

  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_spinner_set_spinning (GTK_SPINNER (spinner), TRUE);
  gtk_widget_set_size_request (spinner, 32, 32);
  gtk_widget_add_css_class (label, "title-4");
  gtk_box_append (GTK_BOX (box), spinner);
  gtk_box_append (GTK_BOX (box), label);
  view->status_label = GTK_LABEL (label);
  return box;
}

static GtkWidget *
build_error_page (View *view)
{
  GtkWidget *status = adw_status_page_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *retry = gtk_button_new_with_mnemonic ("_Try Again");
  GtkWidget *wake = gtk_button_new_with_mnemonic ("_Wake Up Computer");

  adw_status_page_set_icon_name (ADW_STATUS_PAGE (status), "network-offline-symbolic");
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (retry, "pill");
  gtk_widget_add_css_class (retry, "suggested-action");
  gtk_widget_add_css_class (wake, "pill");
  g_signal_connect (retry, "clicked", G_CALLBACK (on_retry), view);
  g_signal_connect (wake, "clicked", G_CALLBACK (on_wake_clicked), view);
  gtk_box_append (GTK_BOX (box), retry);
  gtk_box_append (GTK_BOX (box), wake);
  adw_status_page_set_child (ADW_STATUS_PAGE (status), box);

  view->error_page = ADW_STATUS_PAGE (status);
  view->wake_button = wake;
  return status;
}

static void
open_connection (DrWindow *self, const DrConnection *conn)
{
  View *view = g_new0 (View, 1);
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *keys = gtk_menu_button_new ();
  GtkWidget *fullscreen = gtk_button_new_from_icon_name ("view-fullscreen-symbolic");
  GtkWidget *stack = gtk_stack_new ();
  g_autofree char *address = NULL;
  g_autofree char *subtitle = NULL;
  g_autoptr (GMenu) menu = g_menu_new ();
  g_autoptr (GSimpleActionGroup) actions = g_simple_action_group_new ();
  GtkEventController *motion;
  const GActionEntry entries[] = {
    { "send-keys", send_keys_action, "s", NULL, NULL },
    { "fullscreen", toggle_fullscreen_action, NULL, NULL, NULL },
    { "quality", NULL, "s", conn->adaptive ? "'adaptive'" : "'full'", quality_changed },
  };

  view->window = self;
  view->conn = dr_connection_copy (conn);

  address = dr_connection_format_address (conn);
  subtitle = g_strdup_printf ("%s · %s", dr_protocol_to_string (conn->protocol), address);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   adw_window_title_new (dr_connection_get_title (conn), subtitle));

  g_menu_append (menu, "Ctrl+Alt+Del", "session.send-keys::ctrl-alt-del");
  g_menu_append (menu, conn->protocol == DR_PROTOCOL_VNC ? "Super / Command" : "Windows Key",
                 "session.send-keys::super");
  g_menu_append (menu, "Alt+Tab", "session.send-keys::alt-tab");
  g_menu_append (menu, "Alt+F4", "session.send-keys::alt-f4");
  g_menu_append (menu, "Print Screen", "session.send-keys::print");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (keys), G_MENU_MODEL (menu));
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (keys), "input-keyboard-symbolic");
  gtk_widget_set_tooltip_text (keys, "Send Keys");
  gtk_widget_set_tooltip_text (fullscreen, "Full Screen");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (fullscreen), "session.fullscreen");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), fullscreen);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), keys);

  if (conn->protocol == DR_PROTOCOL_VNC)
    {
      GtkWidget *quality = gtk_menu_button_new ();
      g_autoptr (GMenu) quality_menu = g_menu_new ();
      g_autoptr (GMenu) section = g_menu_new ();

      g_menu_append (section, "Full Quality", "session.quality::full");
      g_menu_append (section, "Adaptive (Less Data)", "session.quality::adaptive");
      g_menu_append_section (quality_menu, "Mac Picture", G_MENU_MODEL (section));
      gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (quality), G_MENU_MODEL (quality_menu));
      gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (quality), "video-display-symbolic");
      gtk_widget_set_tooltip_text (quality, "Picture Quality");
      adw_header_bar_pack_end (ADW_HEADER_BAR (header), quality);
    }

  view->stack = GTK_STACK (stack);
  gtk_stack_add_named (view->stack, build_connecting_page (view), "connecting");
  gtk_stack_add_named (view->stack, build_error_page (view), "error");

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), stack);
  view->toolbar = ADW_TOOLBAR_VIEW (toolbar);

  motion = gtk_event_controller_motion_new ();
  gtk_event_controller_set_propagation_phase (motion, GTK_PHASE_CAPTURE);
  g_signal_connect (motion, "motion", G_CALLBACK (on_toolbar_motion), view);
  gtk_widget_add_controller (toolbar, motion);

  view->page = adw_navigation_page_new (toolbar, dr_connection_get_title (conn));
  g_object_set_data_full (G_OBJECT (view->page), "view", view, NULL);
  g_action_map_add_action_entries (G_ACTION_MAP (actions), entries, G_N_ELEMENTS (entries), view);
  gtk_widget_insert_action_group (GTK_WIDGET (view->page), "session", G_ACTION_GROUP (actions));
  g_signal_connect (self, "notify::fullscreened", G_CALLBACK (on_fullscreen_changed), view);

  adw_navigation_view_push (self->nav, view->page);
  view_start (view);
}

static void
on_page_popped (AdwNavigationView *nav, AdwNavigationPage *page, DrWindow *self)
{
  View *view = g_object_get_data (G_OBJECT (page), "view");

  if (view == NULL)
    return;

  view->closed = TRUE;
  g_signal_handlers_disconnect_by_data (self, view);
  if (gtk_window_is_fullscreen (GTK_WINDOW (self)))
    gtk_window_unfullscreen (GTK_WINDOW (self));
  {
    GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (self));
    if (GDK_IS_TOPLEVEL (surface))
      gdk_toplevel_restore_system_shortcuts (GDK_TOPLEVEL (surface));
  }
  view_drop_session (view);

  g_object_set_data (G_OBJECT (page), "view", NULL);
  /* A keyring lookup still running owns the view and frees it when it ends. */
  if (!view->lookup_pending)
    view_free (view);
}

/* Home page */

static void
on_address_changed (GtkEditable *editable, DrWindow *self)
{
  DrProtocol protocol = gtk_toggle_button_get_active (self->vnc_toggle) ? DR_PROTOCOL_VNC
                                                                        : DR_PROTOCOL_RDP;
  g_autofree char *host = NULL;
  guint port;

  if (dr_parse_address (gtk_editable_get_text (editable), &protocol, &host, &port))
    gtk_toggle_button_set_active (protocol == DR_PROTOCOL_VNC ? self->vnc_toggle
                                                              : self->rdp_toggle, TRUE);
}

static void
connect_to_address (DrWindow *self)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->address_row));
  DrProtocol protocol = gtk_toggle_button_get_active (self->vnc_toggle) ? DR_PROTOCOL_VNC
                                                                        : DR_PROTOCOL_RDP;
  g_autoptr (DrConnection) conn = NULL;
  g_autofree char *host = NULL;
  guint port;

  if (!dr_parse_address (text, &protocol, &host, &port))
    {
      toast (self, "Enter an address like 192.168.1.20 or my-pc");
      gtk_widget_grab_focus (GTK_WIDGET (self->address_row));
      return;
    }

  for (guint i = 0; i < self->connections->len; i++)
    {
      DrConnection *saved = g_ptr_array_index (self->connections, i);

      if (saved->protocol == protocol && saved->port == port &&
          g_ascii_strcasecmp (saved->host, host) == 0)
        {
          open_connection (self, saved);
          gtk_editable_set_text (GTK_EDITABLE (self->address_row), "");
          return;
        }
    }

  conn = dr_connection_new ();
  conn->protocol = protocol;
  conn->host = g_steal_pointer (&host);
  conn->port = port;
  open_connection (self, conn);
  gtk_editable_set_text (GTK_EDITABLE (self->address_row), "");
}

static void
on_saved_activated (AdwActionRow *row, DrWindow *self)
{
  DrConnection *conn = find_connection (self, g_object_get_data (G_OBJECT (row), "id"));

  if (conn)
    open_connection (self, conn);
}

static void
refresh_saved (DrWindow *self)
{
  gtk_list_box_remove_all (self->saved_list);
  gtk_widget_set_visible (GTK_WIDGET (self->saved_group), self->connections->len > 0);

  for (guint i = 0; i < self->connections->len; i++)
    {
      DrConnection *conn = g_ptr_array_index (self->connections, i);
      GtkWidget *row = adw_action_row_new ();
      GtkWidget *more = gtk_menu_button_new ();
      g_autoptr (GMenu) menu = g_menu_new ();
      g_autofree char *address = dr_connection_format_address (conn);
      g_autofree char *subtitle = NULL;
      g_autofree char *action = NULL;

      if (conn->username)
        subtitle = g_strdup_printf ("%s · %s@%s", dr_protocol_to_string (conn->protocol),
                                    conn->username, address);
      else
        subtitle = g_strdup_printf ("%s · %s", dr_protocol_to_string (conn->protocol), address);

      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), dr_connection_get_title (conn));
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row),
                                 gtk_image_new_from_icon_name (conn->protocol == DR_PROTOCOL_VNC
                                                               ? "video-display-symbolic"
                                                               : "computer-symbolic"));
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
      g_object_set_data_full (G_OBJECT (row), "id", g_strdup (conn->id), g_free);
      g_signal_connect (row, "activated", G_CALLBACK (on_saved_activated), self);

      action = g_strdup_printf ("win.edit::%s", conn->id);
      g_menu_append (menu, "_Edit…", action);
      if (conn->mac && *conn->mac)
        {
          g_free (action);
          action = g_strdup_printf ("win.wake::%s", conn->id);
          g_menu_append (menu, "_Wake Up", action);
        }
      g_free (action);
      action = g_strdup_printf ("win.remove::%s", conn->id);
      g_menu_append (menu, "_Remove", action);

      gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (more), "view-more-symbolic");
      gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (more), G_MENU_MODEL (menu));
      gtk_widget_set_valign (more, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (more, "flat");
      gtk_widget_set_tooltip_text (more, "More");
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), more);

      gtk_list_box_append (self->saved_list, row);
    }
}

static void
on_edit_response (AdwAlertDialog *dialog, const char *response, DrWindow *self)
{
  DrConnection *conn = find_connection (self, g_object_get_data (G_OBJECT (dialog), "id"));
  AdwEntryRow *name = g_object_get_data (G_OBJECT (dialog), "name");
  AdwEntryRow *address = g_object_get_data (G_OBJECT (dialog), "address");
  AdwComboRow *protocol_row = g_object_get_data (G_OBJECT (dialog), "protocol");
  AdwEntryRow *user = g_object_get_data (G_OBJECT (dialog), "user");
  AdwEntryRow *mac = g_object_get_data (G_OBJECT (dialog), "mac");
  DrProtocol protocol;
  g_autofree char *host = NULL;
  guint port;
  guint8 mac_bytes[6];
  const char *mac_text;

  if (conn == NULL || g_strcmp0 (response, "save") != 0)
    return;

  protocol = adw_combo_row_get_selected (protocol_row) == 1 ? DR_PROTOCOL_VNC : DR_PROTOCOL_RDP;
  if (!dr_parse_address (gtk_editable_get_text (GTK_EDITABLE (address)), &protocol, &host, &port))
    {
      toast (self, "That address isn't valid, so the change wasn't saved");
      return;
    }
  mac_text = gtk_editable_get_text (GTK_EDITABLE (mac));
  if (*mac_text && !dr_wol_parse_mac (mac_text, mac_bytes))
    {
      toast (self, "That MAC address isn't valid, so the change wasn't saved");
      return;
    }

  g_free (conn->name);
  conn->name = g_strdup (gtk_editable_get_text (GTK_EDITABLE (name)));
  g_free (conn->host);
  conn->host = g_steal_pointer (&host);
  conn->port = port;
  conn->protocol = protocol;
  g_free (conn->username);
  conn->username = g_strdup (gtk_editable_get_text (GTK_EDITABLE (user)));
  if (conn->username && *conn->username == '\0')
    g_clear_pointer (&conn->username, g_free);
  g_free (conn->mac);
  conn->mac = g_strdup (mac_text);

  dr_connections_save (self->connections);
  refresh_saved (self);
}

static GtkWidget *
add_entry (GtkWidget *list, const char *title, const char *text)
{
  GtkWidget *row = adw_entry_row_new ();

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  gtk_editable_set_text (GTK_EDITABLE (row), text ? text : "");
  adw_entry_row_set_activates_default (ADW_ENTRY_ROW (row), TRUE);
  gtk_list_box_append (GTK_LIST_BOX (list), row);
  return row;
}

static void
edit_action (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  DrWindow *self = data;
  DrConnection *conn = find_connection (self, g_variant_get_string (parameter, NULL));
  AdwDialog *dialog;
  GtkWidget *list;
  GtkWidget *protocol;
  g_autofree char *address = NULL;
  const char *protocols[] = { "RDP", "VNC", NULL };

  if (conn == NULL)
    return;

  dialog = adw_alert_dialog_new ("Edit Computer", NULL);
  list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");

  address = dr_connection_format_address (conn);
  g_object_set_data (G_OBJECT (dialog), "name", add_entry (list, "Name", conn->name));
  g_object_set_data (G_OBJECT (dialog), "address", add_entry (list, "Address", address));

  protocol = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (protocol), "Protocol");
  adw_combo_row_set_model (ADW_COMBO_ROW (protocol),
                           G_LIST_MODEL (gtk_string_list_new (protocols)));
  adw_combo_row_set_selected (ADW_COMBO_ROW (protocol), conn->protocol == DR_PROTOCOL_VNC);
  gtk_list_box_append (GTK_LIST_BOX (list), protocol);
  g_object_set_data (G_OBJECT (dialog), "protocol", protocol);

  g_object_set_data (G_OBJECT (dialog), "user", add_entry (list, "User name", conn->username));
  g_object_set_data (G_OBJECT (dialog), "mac",
                     add_entry (list, "MAC address (for Wake on LAN)", conn->mac));
  g_object_set_data_full (G_OBJECT (dialog), "id", g_strdup (conn->id), g_free);

  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), list);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                  "cancel", "_Cancel", "save", "_Save", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "save",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "save");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect (dialog, "response", G_CALLBACK (on_edit_response), self);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
wake_action (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  DrWindow *self = data;
  DrConnection *conn = find_connection (self, g_variant_get_string (parameter, NULL));
  g_autoptr (GError) error = NULL;

  if (conn == NULL)
    return;
  if (dr_wol_send (conn->mac, &error))
    toast (self, "Wake-up sent");
  else
    toast (self, error->message);
}

static void
remove_action (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  DrWindow *self = data;
  DrConnection *conn = find_connection (self, g_variant_get_string (parameter, NULL));

  if (conn == NULL)
    return;
  dr_password_clear (conn);
  g_ptr_array_remove (self->connections, conn);
  dr_connections_save (self->connections);
  refresh_saved (self);
}

static void
about_action (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  AdwDialog *about = adw_about_dialog_new ();
  const char *developers[] = { "Lidan", NULL };

  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (about), "Direct RDP & VNC");
  adw_about_dialog_set_application_icon (ADW_ABOUT_DIALOG (about), APP_ID);
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (about), APP_VERSION);
  adw_about_dialog_set_developers (ADW_ABOUT_DIALOG (about), developers);
  adw_about_dialog_set_website (ADW_ABOUT_DIALOG (about), "https://github.com/Lidanha/direct-rdp");
  adw_about_dialog_set_issue_url (ADW_ABOUT_DIALOG (about),
                                  "https://github.com/Lidanha/direct-rdp/issues");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (about), GTK_LICENSE_GPL_3_0);
  adw_about_dialog_add_legal_section (ADW_ABOUT_DIALOG (about), "FreeRDP",
                                      "Copyright © FreeRDP contributors", GTK_LICENSE_APACHE_2_0,
                                      NULL);
  adw_about_dialog_add_legal_section (ADW_ABOUT_DIALOG (about), "LibVNCServer",
                                      "Copyright © LibVNCServer contributors",
                                      GTK_LICENSE_GPL_2_0, NULL);
  adw_dialog_present (about, GTK_WIDGET (data));
}

static GtkWidget *
build_home_page (DrWindow *self)
{
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *menu_button = gtk_menu_button_new ();
  GtkWidget *prefs = adw_preferences_page_new ();
  GtkWidget *connect_group = adw_preferences_group_new ();
  GtkWidget *address = adw_entry_row_new ();
  GtkWidget *toggles = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *rdp = gtk_toggle_button_new_with_label ("RDP");
  GtkWidget *vnc = gtk_toggle_button_new_with_label ("VNC");
  GtkWidget *go = gtk_button_new_from_icon_name ("go-next-symbolic");
  GtkWidget *saved = adw_preferences_group_new ();
  GtkWidget *list = gtk_list_box_new ();
  g_autoptr (GMenu) menu = g_menu_new ();

  g_menu_append (menu, "_About Direct RDP & VNC", "win.about");
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_button), "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_button), G_MENU_MODEL (menu));
  gtk_menu_button_set_primary (GTK_MENU_BUTTON (menu_button), TRUE);
  gtk_widget_set_tooltip_text (menu_button, "Main Menu");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_button);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (address), "Computer address");
  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (vnc), GTK_TOGGLE_BUTTON (rdp));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (rdp), TRUE);
  gtk_widget_add_css_class (toggles, "linked");
  gtk_widget_set_valign (toggles, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (toggles), rdp);
  gtk_box_append (GTK_BOX (toggles), vnc);
  gtk_widget_set_valign (go, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (go, "suggested-action");
  gtk_widget_add_css_class (go, "circular");
  gtk_widget_set_tooltip_text (go, "Connect");
  adw_entry_row_add_suffix (ADW_ENTRY_ROW (address), toggles);
  adw_entry_row_add_suffix (ADW_ENTRY_ROW (address), go);
  g_signal_connect_swapped (address, "entry-activated", G_CALLBACK (connect_to_address), self);
  g_signal_connect_swapped (go, "clicked", G_CALLBACK (connect_to_address), self);
  g_signal_connect (address, "changed", G_CALLBACK (on_address_changed), self);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (connect_group), "Connect");
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (connect_group),
                                         "An IP address or computer name, like 192.168.1.20, "
                                         "my-pc or my-pc.tailnet.ts.net. RDP for Windows and "
                                         "Linux desktops, VNC for Macs and VNC servers.");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (connect_group), address);

  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (saved), "Computers");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (saved), list);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (prefs), ADW_PREFERENCES_GROUP (connect_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (prefs), ADW_PREFERENCES_GROUP (saved));

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), prefs);

  self->address_row = ADW_ENTRY_ROW (address);
  self->rdp_toggle = GTK_TOGGLE_BUTTON (rdp);
  self->vnc_toggle = GTK_TOGGLE_BUTTON (vnc);
  self->saved_group = ADW_PREFERENCES_GROUP (saved);
  self->saved_list = GTK_LIST_BOX (list);

  return GTK_WIDGET (adw_navigation_page_new (toolbar, "Direct RDP & VNC"));
}

static void
dr_window_dispose (GObject *object)
{
  DrWindow *self = DR_WINDOW (object);

  g_clear_pointer (&self->connections, g_ptr_array_unref);
  G_OBJECT_CLASS (dr_window_parent_class)->dispose (object);
}

static void
dr_window_class_init (DrWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = dr_window_dispose;
}

static void
dr_window_init (DrWindow *self)
{
  const GActionEntry entries[] = {
    { "edit", edit_action, "s", NULL, NULL },
    { "wake", wake_action, "s", NULL, NULL },
    { "remove", remove_action, "s", NULL, NULL },
    { "about", about_action, NULL, NULL, NULL },
  };
  GtkWidget *nav = adw_navigation_view_new ();
  GtkWidget *toasts = adw_toast_overlay_new ();

  self->connections = dr_connections_load ();
  g_action_map_add_action_entries (G_ACTION_MAP (self), entries, G_N_ELEMENTS (entries), self);

  self->nav = ADW_NAVIGATION_VIEW (nav);
  self->toasts = ADW_TOAST_OVERLAY (toasts);
  adw_navigation_view_add (self->nav, ADW_NAVIGATION_PAGE (build_home_page (self)));
  g_signal_connect (nav, "popped", G_CALLBACK (on_page_popped), self);
  adw_toast_overlay_set_child (self->toasts, nav);

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), toasts);
  gtk_window_set_title (GTK_WINDOW (self), "Direct RDP & VNC");
  gtk_window_set_default_size (GTK_WINDOW (self), 1280, 800);
  gtk_widget_set_size_request (GTK_WIDGET (self), 360, 300);

  refresh_saved (self);
}

DrWindow *
dr_window_new (GtkApplication *app)
{
  return g_object_new (DR_TYPE_WINDOW, "application", app, NULL);
}

void
dr_window_open_address (DrWindow *self, const char *address)
{
  gtk_editable_set_text (GTK_EDITABLE (self->address_row), address);
  connect_to_address (self);
}
