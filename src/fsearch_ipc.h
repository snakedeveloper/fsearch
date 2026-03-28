#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define FSEARCH_DAEMON_PROTOCOL_VERSION 1
#define FSEARCH_DAEMON_SYSTEM_SOCKET_PATH "/run/fsearchd/socket"

char *
fsearch_ipc_get_default_socket_path(gboolean system_scope);

gboolean
fsearch_ipc_client_request(const char *socket_path, const char *request, gchar **response, GError **error);

G_END_DECLS
