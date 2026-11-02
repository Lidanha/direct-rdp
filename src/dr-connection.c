/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Saved computers live in ~/.config/direct-rdp/connections.ini; their
 * passwords live in the GNOME keyring (libsecret), never in that file.
 */
#include "dr-connection.h"

#include <libsecret/secret.h>

static const SecretSchema password_schema = {
  "io.github.lidanha.DirectRDP.Password", SECRET_SCHEMA_NONE,
  {
    { "id", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 },
  }
};

DrConnection *
dr_connection_new (void)
{
  DrConnection *conn = g_new0 (DrConnection, 1);

  conn->id = g_uuid_string_random ();
  return conn;
}

DrConnection *
dr_connection_copy (const DrConnection *conn)
{
  DrConnection *copy = g_new0 (DrConnection, 1);

  copy->id = g_strdup (conn->id);
  copy->name = g_strdup (conn->name);
  copy->protocol = conn->protocol;
  copy->host = g_strdup (conn->host);
  copy->port = conn->port;
  copy->username = g_strdup (conn->username);
  copy->domain = g_strdup (conn->domain);
  copy->mac = g_strdup (conn->mac);
  copy->last_used = conn->last_used;
  return copy;
}

void
dr_connection_free (DrConnection *conn)
{
  if (conn == NULL)
    return;
  g_free (conn->id);
  g_free (conn->name);
  g_free (conn->host);
  g_free (conn->username);
  g_free (conn->domain);
  g_free (conn->mac);
  g_free (conn);
}

guint
dr_connection_get_port (const DrConnection *conn)
{
  if (conn->port)
    return conn->port;
  return conn->protocol == DR_PROTOCOL_VNC ? 5900 : 3389;
}

const char *
dr_connection_get_title (const DrConnection *conn)
{
  return conn->name && *conn->name ? conn->name : conn->host;
}

char *
dr_connection_format_address (const DrConnection *conn)
{
  gboolean ipv6 = strchr (conn->host, ':') != NULL;

  if (conn->port == 0)
    return g_strdup (conn->host);
  return g_strdup_printf (ipv6 ? "[%s]:%u" : "%s:%u", conn->host, conn->port);
}

const char *
dr_protocol_to_string (DrProtocol protocol)
{
  return protocol == DR_PROTOCOL_VNC ? "VNC" : "RDP";
}

/* Accepts "host", "host:port", "[ipv6]:port", "rdp://host" and "vnc://host:port".
 * A vnc:// scheme or a port in 5900-5999 switches *protocol to VNC; rdp:// or
 * port 3389 switch it to RDP. A port equal to the protocol's default becomes 0. */
gboolean
dr_parse_address (const char *text, DrProtocol *protocol, char **host, guint *port)
{
  g_autofree char *s = g_strstrip (g_strdup (text ? text : ""));
  char *h = s;
  char *colon;
  guint64 p = 0;

  if (g_ascii_strncasecmp (h, "vnc://", 6) == 0)
    {
      *protocol = DR_PROTOCOL_VNC;
      h += 6;
    }
  else if (g_ascii_strncasecmp (h, "rdp://", 6) == 0)
    {
      *protocol = DR_PROTOCOL_RDP;
      h += 6;
    }

  /* drop a trailing path and a user@ prefix */
  if ((colon = strchr (h, '/')) != NULL)
    *colon = '\0';
  if ((colon = strrchr (h, '@')) != NULL)
    h = colon + 1;

  if (*h == '[')
    {
      char *end = strchr (h, ']');
      if (end == NULL)
        return FALSE;
      *end = '\0';
      if (end[1] == ':' && !g_ascii_string_to_unsigned (end + 2, 10, 1, 65535, &p, NULL))
        return FALSE;
      h++;
    }
  else if ((colon = strchr (h, ':')) != NULL && strchr (colon + 1, ':') == NULL)
    {
      *colon = '\0';
      if (!g_ascii_string_to_unsigned (colon + 1, 10, 1, 65535, &p, NULL))
        return FALSE;
    }

  if (*h == '\0' || strchr (h, ' ') != NULL)
    return FALSE;

  if (p >= 5900 && p <= 5999)
    *protocol = DR_PROTOCOL_VNC;
  else if (p == 3389)
    *protocol = DR_PROTOCOL_RDP;
  if ((*protocol == DR_PROTOCOL_VNC && p == 5900) || (*protocol == DR_PROTOCOL_RDP && p == 3389))
    p = 0;

  *host = g_strdup (h);
  *port = p;
  return TRUE;
}

static char *
store_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "direct-rdp", "connections.ini", NULL);
}

static char *
get_string (GKeyFile *file, const char *group, const char *key)
{
  char *value = g_key_file_get_string (file, group, key, NULL);

  if (value && *value == '\0')
    g_clear_pointer (&value, g_free);
  return value;
}

static int
compare_last_used (gconstpointer a, gconstpointer b)
{
  const DrConnection *ca = *(DrConnection **) a;
  const DrConnection *cb = *(DrConnection **) b;

  return (cb->last_used > ca->last_used) - (cb->last_used < ca->last_used);
}

void
dr_connections_sort (GPtrArray *connections)
{
  g_ptr_array_sort (connections, compare_last_used);
}

GPtrArray *
dr_connections_load (void)
{
  GPtrArray *connections = g_ptr_array_new_with_free_func ((GDestroyNotify) dr_connection_free);
  g_autoptr (GKeyFile) file = g_key_file_new ();
  g_autofree char *path = store_path ();
  g_auto (GStrv) groups = NULL;

  if (!g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, NULL))
    return connections;

  groups = g_key_file_get_groups (file, NULL);
  for (int i = 0; groups[i]; i++)
    {
      DrConnection *conn = g_new0 (DrConnection, 1);
      g_autofree char *protocol = get_string (file, groups[i], "protocol");

      conn->id = g_strdup (groups[i]);
      conn->name = get_string (file, groups[i], "name");
      conn->protocol = g_strcmp0 (protocol, "vnc") == 0 ? DR_PROTOCOL_VNC : DR_PROTOCOL_RDP;
      conn->host = get_string (file, groups[i], "host");
      conn->port = g_key_file_get_integer (file, groups[i], "port", NULL);
      conn->username = get_string (file, groups[i], "username");
      conn->domain = get_string (file, groups[i], "domain");
      conn->mac = get_string (file, groups[i], "mac");
      conn->last_used = g_key_file_get_int64 (file, groups[i], "last-used", NULL);

      if (conn->host == NULL)
        dr_connection_free (conn);
      else
        g_ptr_array_add (connections, conn);
    }

  dr_connections_sort (connections);
  return connections;
}

static void
set_string (GKeyFile *file, const char *group, const char *key, const char *value)
{
  if (value && *value)
    g_key_file_set_string (file, group, key, value);
}

void
dr_connections_save (GPtrArray *connections)
{
  g_autoptr (GKeyFile) file = g_key_file_new ();
  g_autofree char *path = store_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr (GError) error = NULL;

  for (guint i = 0; i < connections->len; i++)
    {
      DrConnection *conn = g_ptr_array_index (connections, i);

      set_string (file, conn->id, "name", conn->name);
      g_key_file_set_string (file, conn->id, "protocol",
                             conn->protocol == DR_PROTOCOL_VNC ? "vnc" : "rdp");
      set_string (file, conn->id, "host", conn->host);
      if (conn->port)
        g_key_file_set_integer (file, conn->id, "port", conn->port);
      set_string (file, conn->id, "username", conn->username);
      set_string (file, conn->id, "domain", conn->domain);
      set_string (file, conn->id, "mac", conn->mac);
      g_key_file_set_int64 (file, conn->id, "last-used", conn->last_used);
    }

  g_mkdir_with_parents (dir, 0700);
  if (!g_key_file_save_to_file (file, path, &error))
    g_warning ("Couldn't save %s: %s", path, error->message);
}

void
dr_password_lookup (const DrConnection *conn, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
  secret_password_lookup (&password_schema, cancellable, callback, user_data,
                          "id", conn->id, NULL);
}

char *
dr_password_lookup_finish (GAsyncResult *result)
{
  g_autoptr (GError) error = NULL;
  char *password = secret_password_lookup_finish (result, &error);

  if (error)
    g_debug ("Keyring lookup failed: %s", error->message);
  return password;
}

void
dr_password_store (const DrConnection *conn, const char *password)
{
  g_autofree char *label = g_strdup_printf ("Direct RDP & VNC: %s", conn->host);

  secret_password_store (&password_schema, SECRET_COLLECTION_DEFAULT, label, password,
                         NULL, NULL, NULL, "id", conn->id, NULL);
}

void
dr_password_clear (const DrConnection *conn)
{
  secret_password_clear (&password_schema, NULL, NULL, NULL, "id", conn->id, NULL);
}
