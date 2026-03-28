#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "fsearch_ipc.h"

#include <errno.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

static char *
fsearch_ipc_normalize_request(const char *request) {
    g_return_val_if_fail(request, NULL);

    if (g_str_has_suffix(request, "\n")) {
        return g_strdup(request);
    }
    return g_strdup_printf("%s\n", request);
}

char *
fsearch_ipc_get_default_socket_path(gboolean system_scope) {
    const char *socket_path_env = g_getenv("FSEARCHD_SOCKET_PATH");
    if (socket_path_env && socket_path_env[0] != '\0') {
        return g_strdup(socket_path_env);
    }

    if (system_scope) {
        return g_strdup(FSEARCH_DAEMON_SYSTEM_SOCKET_PATH);
    }

    const char *runtime_dir = g_get_user_runtime_dir();
    if (runtime_dir && runtime_dir[0] != '\0') {
        return g_build_filename(runtime_dir, "fsearch", "fsearchd.sock", NULL);
    }

    return g_strdup_printf("%s/fsearch-%u/fsearchd.sock", g_get_tmp_dir(), (unsigned)getuid());
}

gboolean
fsearch_ipc_client_request(const char *socket_path, const char *request, gchar **response, GError **error) {
    g_return_val_if_fail(socket_path, false);
    g_return_val_if_fail(request, false);
    g_return_val_if_fail(response, false);

    *response = NULL;

    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(socket_path);
    g_autoptr(GSocket) socket = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, error);
    if (!socket) {
        return false;
    }
    if (!g_socket_connect(socket, address, NULL, error)) {
        return false;
    }

    g_autoptr(GSocketConnection) connection = g_socket_connection_factory_create_connection(socket);
    if (!connection) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "failed to create IPC connection");
        return false;
    }

    g_autofree char *request_line = fsearch_ipc_normalize_request(request);
    if (!request_line) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "failed to normalize IPC request");
        return false;
    }

    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    if (!g_output_stream_write_all(output, request_line, strlen(request_line), NULL, NULL, error)) {
        return false;
    }
    if (!g_output_stream_flush(output, NULL, error)) {
        return false;
    }

    g_autoptr(GDataInputStream) input =
        g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    *response = g_data_input_stream_read_line_utf8(input, NULL, NULL, error);
    if (!*response) {
        if (error && *error == NULL) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE, "daemon closed IPC connection");
        }
        return false;
    }

    return true;
}
