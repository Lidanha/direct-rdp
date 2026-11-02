/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wake on LAN: a "magic packet" (6 x 0xFF, then the MAC address 16 times),
 * broadcast over UDP to ports 9 and 7.
 */
#include "dr-wol.h"

#include <gio/gio.h>

gboolean
dr_wol_parse_mac (const char *text, guint8 mac[6])
{
  int n = 0;

  for (const char *p = text; p && *p && n < 12; p++)
    {
      int digit = g_ascii_xdigit_value (*p);

      if (digit < 0)
        {
          if (*p == ':' || *p == '-' || *p == '.' || *p == ' ')
            continue;
          return FALSE;
        }
      if (n % 2 == 0)
        mac[n / 2] = digit << 4;
      else
        mac[n / 2] |= digit;
      n++;
    }
  return n == 12;
}

gboolean
dr_wol_send (const char *mac_text, GError **error)
{
  g_autoptr (GSocket) socket = NULL;
  g_autoptr (GInetAddress) broadcast = NULL;
  guint8 mac[6];
  guint8 packet[102];
  const guint16 ports[] = { 9, 7 };

  if (!dr_wol_parse_mac (mac_text, mac))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "\"%s\" isn't a MAC address. It looks like 1a:2b:3c:4d:5e:6f.", mac_text);
      return FALSE;
    }

  memset (packet, 0xff, 6);
  for (int i = 0; i < 16; i++)
    memcpy (packet + 6 + i * 6, mac, 6);

  socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, error);
  if (socket == NULL)
    return FALSE;
  g_socket_set_broadcast (socket, TRUE);
  broadcast = g_inet_address_new_from_string ("255.255.255.255");

  for (guint i = 0; i < G_N_ELEMENTS (ports); i++)
    {
      g_autoptr (GSocketAddress) address = g_inet_socket_address_new (broadcast, ports[i]);

      if (g_socket_send_to (socket, address, (const char *) packet, sizeof packet, NULL, error) < 0)
        return FALSE;
    }
  return TRUE;
}
