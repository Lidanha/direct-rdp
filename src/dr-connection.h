/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum
{
  DR_PROTOCOL_RDP,
  DR_PROTOCOL_VNC,
} DrProtocol;

/* A Mac's picture: Full quality is lossless, Adaptive sends a fraction of
 * the data, and Automatic uses Adaptive while much of the screen moves. */
typedef enum
{
  DR_PICTURE_AUTOMATIC,
  DR_PICTURE_FULL,
  DR_PICTURE_ADAPTIVE,
} DrPicture;

const char *dr_picture_to_string (DrPicture picture);
DrPicture dr_picture_from_string (const char *string);

typedef struct
{
  char *id;
  char *name;     /* optional; the address is shown when empty */
  DrProtocol protocol;
  char *host;
  guint port;     /* 0 = the protocol's default */
  char *username;
  char *domain;
  char *mac;      /* for Wake on LAN */
  DrPicture picture; /* Macs */
  gint64 last_used;
} DrConnection;

DrConnection *dr_connection_new (void);
DrConnection *dr_connection_copy (const DrConnection *conn);
void dr_connection_free (DrConnection *conn);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (DrConnection, dr_connection_free)

guint dr_connection_get_port (const DrConnection *conn);
const char *dr_connection_get_title (const DrConnection *conn);
char *dr_connection_format_address (const DrConnection *conn);
const char *dr_protocol_to_string (DrProtocol protocol);

gboolean dr_parse_address (const char *text, DrProtocol *protocol, char **host, guint *port);

GPtrArray *dr_connections_load (void);
void dr_connections_save (GPtrArray *connections);
void dr_connections_sort (GPtrArray *connections);

void dr_password_lookup (const DrConnection *conn, GCancellable *cancellable,
                         GAsyncReadyCallback callback, gpointer user_data);
char *dr_password_lookup_finish (GAsyncResult *result);
void dr_password_store (const DrConnection *conn, const char *password);
void dr_password_clear (const DrConnection *conn);

G_END_DECLS
