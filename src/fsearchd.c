#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "fsearchd.h"

#include "fsearch_ipc.h"
#include "fsearch_monitor_backend.h"
#include "fsearch_monitor_manager.h"

#include <errno.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct FsearchDaemon {
    gchar *socket_path;
    gboolean system_scope;
    guint64 clients_served;
    gint64 started_at_monotonic_usec;
    GSocketService *service;
    FsearchMonitorManager *monitor_manager;
};

static gboolean
fsearch_daemon_write_response(GSocketConnection *connection, const char *response, GError **error) {
    g_return_val_if_fail(connection, false);
    g_return_val_if_fail(response, false);

    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    if (!g_output_stream_write_all(output, response, strlen(response), NULL, NULL, error)) {
        return false;
    }
    return g_output_stream_flush(output, NULL, error);
}

static gchar *
fsearch_daemon_build_response(FsearchDaemon *daemon, const char *request_line) {
    g_return_val_if_fail(daemon, NULL);

    g_autofree gchar *request = g_strstrip(g_strdup(request_line ? request_line : ""));

    if (g_ascii_strcasecmp(request, "PING") == 0) {
        return g_strdup_printf("PONG protocol=%u\n", FSEARCH_DAEMON_PROTOCOL_VERSION);
    }

    if (g_ascii_strcasecmp(request, "BACKENDS") == 0) {
        g_autofree gchar *backends = fsearch_monitor_backends_format_summary();
        return g_strdup_printf("BACKENDS %s\n", backends);
    }

    if (g_ascii_strcasecmp(request, "STATUS") == 0) {
        const gint64 uptime_usec = g_get_monotonic_time() - daemon->started_at_monotonic_usec;
        g_autofree gchar *backends = fsearch_monitor_backends_format_summary();
        g_autofree gchar *monitor_status = fsearch_monitor_manager_format_status(daemon->monitor_manager);
        return g_strdup_printf("STATUS protocol=%u scope=%s privileged=%d clients=%" G_GUINT64_FORMAT
                               " uptime_sec=%" G_GINT64_FORMAT " backends=%s %s\n",
                               FSEARCH_DAEMON_PROTOCOL_VERSION,
                               daemon->system_scope ? "system" : "user",
                               geteuid() == 0,
                               daemon->clients_served,
                               uptime_usec / G_USEC_PER_SEC,
                               backends,
                               monitor_status);
    }

    if (g_ascii_strcasecmp(request, "ROOTS") == 0) {
        g_autofree gchar *roots = fsearch_monitor_manager_format_roots(daemon->monitor_manager);
        return g_strdup_printf("ROOTS %s\n", roots);
    }

    if (g_ascii_strcasecmp(request, "DIRTY") == 0) {
        g_autofree gchar *dirty_paths = fsearch_monitor_manager_format_dirty_paths(daemon->monitor_manager);
        return g_strdup_printf("DIRTY %s\n", dirty_paths);
    }

    if (g_ascii_strcasecmp(request, "TAKE_DIRTY") == 0) {
        g_autofree char *path = fsearch_monitor_manager_take_dirty_path(daemon->monitor_manager);
        if (!path) {
            return g_strdup("TAKE_DIRTY path=\n");
        }
        g_autofree char *escaped_path = g_strescape(path, NULL);
        return g_strdup_printf("TAKE_DIRTY path=%s\n", escaped_path);
    }

    if (g_ascii_strcasecmp(request, "HELP") == 0 || request[0] == '\0') {
        return g_strdup("OK commands=PING,STATUS,BACKENDS,ROOTS,DIRTY,TAKE_DIRTY,HELP\n");
    }

    return g_strdup_printf("ERROR unsupported-command=%s\n", request);
}

static gboolean
fsearch_daemon_on_incoming(GSocketService *service,
                           GSocketConnection *connection,
                           GObject *source_object,
                           gpointer user_data) {
    (void)service;
    (void)source_object;

    FsearchDaemon *daemon = user_data;
    g_return_val_if_fail(daemon, false);

    g_autoptr(GDataInputStream) input =
        g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));

    g_autoptr(GError) error = NULL;
    g_autofree gchar *request = g_data_input_stream_read_line_utf8(input, NULL, NULL, &error);
    if (!request) {
        g_warning("[fsearchd] failed to read IPC request: %s", error ? error->message : "unknown error");
        return false;
    }

    daemon->clients_served++;

    g_autofree gchar *response = fsearch_daemon_build_response(daemon, request);
    if (!response) {
        g_warning("[fsearchd] failed to build IPC response");
        return false;
    }

    g_clear_error(&error);
    if (!fsearch_daemon_write_response(connection, response, &error)) {
        g_warning("[fsearchd] failed to write IPC response: %s", error ? error->message : "unknown error");
        return false;
    }

    return true;
}

static gboolean
fsearch_daemon_remove_stale_socket(const char *socket_path, GError **error) {
    struct stat st = {0};
    if (g_lstat(socket_path, &st) != 0) {
        return true;
    }

    if (!S_ISSOCK(st.st_mode)) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(EEXIST),
                    "refusing to remove non-socket path: %s",
                    socket_path);
        return false;
    }

    if (g_unlink(socket_path) != 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "failed to remove stale socket %s: %s",
                    socket_path,
                    g_strerror(errno));
        return false;
    }

    return true;
}

FsearchDaemon *
fsearch_daemon_new(const char *socket_path,
                   gboolean system_scope,
                   char **monitor_roots,
                   const char *refresh_command,
                   guint refresh_delay_seconds,
                   guint refresh_retry_delay_seconds) {
    g_return_val_if_fail(socket_path, NULL);

    FsearchDaemon *daemon = g_new0(FsearchDaemon, 1);
    daemon->socket_path = g_strdup(socket_path);
    daemon->system_scope = system_scope;
    daemon->monitor_manager =
        fsearch_monitor_manager_new(monitor_roots, refresh_command, refresh_delay_seconds, refresh_retry_delay_seconds);

    return daemon;
}

void
fsearch_daemon_free(FsearchDaemon *daemon) {
    if (!daemon) {
        return;
    }

    fsearch_daemon_stop(daemon);
    g_clear_pointer(&daemon->monitor_manager, fsearch_monitor_manager_free);
    g_clear_pointer(&daemon->socket_path, g_free);
    g_free(daemon);
}

gboolean
fsearch_daemon_start(FsearchDaemon *daemon, GError **error) {
    g_return_val_if_fail(daemon, false);

    if (daemon->service) {
        return true;
    }

    g_autofree gchar *socket_dir = g_path_get_dirname(daemon->socket_path);
    const int socket_dir_mode = daemon->system_scope ? 0775 : 0700;
    if (g_mkdir_with_parents(socket_dir, socket_dir_mode) != 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "failed to create socket directory %s: %s",
                    socket_dir,
                    g_strerror(errno));
        return false;
    }

    if (!fsearch_daemon_remove_stale_socket(daemon->socket_path, error)) {
        return false;
    }

    if (!fsearch_monitor_manager_start(daemon->monitor_manager, error)) {
        return false;
    }

    daemon->service = g_socket_service_new();
    g_signal_connect(daemon->service, "incoming", G_CALLBACK(fsearch_daemon_on_incoming), daemon);

    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(daemon->socket_path);
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(daemon->service),
                                       address,
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL,
                                       NULL,
                                       error)) {
        g_clear_object(&daemon->service);
        return false;
    }

    g_socket_service_start(daemon->service);
    daemon->started_at_monotonic_usec = g_get_monotonic_time();

    const int socket_mode = daemon->system_scope ? 0660 : 0600;
    if (g_chmod(daemon->socket_path, socket_mode) != 0) {
        g_warning("[fsearchd] failed to chmod socket %s: %s", daemon->socket_path, g_strerror(errno));
    }

    g_message("[fsearchd] listening on %s", daemon->socket_path);
    return true;
}

void
fsearch_daemon_stop(FsearchDaemon *daemon) {
    if (!daemon || !daemon->service) {
        if (daemon) {
            fsearch_monitor_manager_stop(daemon->monitor_manager);
        }
        return;
    }

    g_socket_service_stop(daemon->service);
    g_clear_object(&daemon->service);

    if (daemon->socket_path && g_file_test(daemon->socket_path, G_FILE_TEST_EXISTS)) {
        if (g_unlink(daemon->socket_path) != 0) {
            g_warning("[fsearchd] failed to remove socket %s: %s", daemon->socket_path, g_strerror(errno));
        }
    }

    fsearch_monitor_manager_stop(daemon->monitor_manager);
}

const char *
fsearch_daemon_get_socket_path(FsearchDaemon *daemon) {
    g_return_val_if_fail(daemon, NULL);
    return daemon->socket_path;
}
