/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "dr-session.h"

G_BEGIN_DECLS

#define DR_TYPE_RDP_SESSION (dr_rdp_session_get_type ())
G_DECLARE_FINAL_TYPE (DrRdpSession, dr_rdp_session, DR, RDP_SESSION, DrSession)

DrSession *dr_rdp_session_new (const char *host, guint port);

G_END_DECLS
