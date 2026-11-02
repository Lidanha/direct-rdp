/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

gboolean dr_wol_parse_mac (const char *text, guint8 mac[6]);
gboolean dr_wol_send (const char *mac_text, GError **error);

G_END_DECLS
