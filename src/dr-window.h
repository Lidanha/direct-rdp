/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define DR_TYPE_WINDOW (dr_window_get_type ())
G_DECLARE_FINAL_TYPE (DrWindow, dr_window, DR, WINDOW, AdwApplicationWindow)

DrWindow *dr_window_new (GtkApplication *app);
void dr_window_open_address (DrWindow *self, const char *address);

G_END_DECLS
