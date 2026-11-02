/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "dr-session.h"

G_BEGIN_DECLS

#define DR_TYPE_DISPLAY (dr_display_get_type ())
G_DECLARE_FINAL_TYPE (DrDisplay, dr_display, DR, DISPLAY, GtkWidget)

GtkWidget *dr_display_new (DrSession *session);

G_END_DECLS
