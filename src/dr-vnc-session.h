/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "dr-session.h"

G_BEGIN_DECLS

#define DR_TYPE_VNC_SESSION (dr_vnc_session_get_type ())
G_DECLARE_FINAL_TYPE (DrVncSession, dr_vnc_session, DR, VNC_SESSION, DrSession)

DrSession *dr_vnc_session_new (const char *host, guint port);
void dr_vnc_session_set_adaptive (DrVncSession *self, gboolean adaptive);

G_END_DECLS
