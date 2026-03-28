/*
   FSearch - A fast file search utility
   Copyright © 2020 Christian Boxdörfer

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
   */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "fsearch_ipc.h"
#include "fsearchd.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>

typedef struct FsearchDaemonOptions {
    gboolean system_socket;
    gboolean print_socket_path;
    gboolean ping;
    gboolean status;
    gboolean roots;
    gboolean backends;
    gboolean dirty;
    gboolean take_dirty;
    gboolean version;
    gint refresh_delay;
    gint refresh_retry_delay;
    gchar *socket_path;
    gchar *refresh_command;
    gchar **watch_paths;
} FsearchDaemonOptions;

static gboolean
fsearchd_on_shutdown_signal(gpointer user_data) {
    GMainLoop *loop = user_data;
    g_main_loop_quit(loop);
    return G_SOURCE_CONTINUE;
}

static int
fsearchd_handle_client_request(const FsearchDaemonOptions *options) {
    const char *request = "PING";
    if (options->status) {
        request = "STATUS";
    }
    else if (options->roots) {
        request = "ROOTS";
    }
    else if (options->backends) {
        request = "BACKENDS";
    }
    else if (options->dirty) {
        request = "DIRTY";
    }
    else if (options->take_dirty) {
        request = "TAKE_DIRTY";
    }

    g_autofree gchar *socket_path =
        options->socket_path ? g_strdup(options->socket_path) : fsearch_ipc_get_default_socket_path(options->system_socket);
    g_autofree gchar *response = NULL;
    g_autoptr(GError) error = NULL;
    if (!fsearch_ipc_client_request(socket_path, request, &response, &error)) {
        g_printerr("fsearchd: %s\n", error ? error->message : "IPC request failed");
        return 1;
    }

    g_print("%s\n", response);
    return 0;
}

int
main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    FsearchDaemonOptions options = {0};
    static GOptionEntry entries[] = {
        {"system-socket", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Listen on the system socket path"), NULL},
        {"socket-path", 0, 0, G_OPTION_ARG_FILENAME, NULL, N_("Override the daemon socket path"), N_("PATH")},
        {"watch", 'w', 0, G_OPTION_ARG_FILENAME_ARRAY, NULL, N_("Add a watched root directory"), N_("PATH")},
        {"refresh-command", 0, 0, G_OPTION_ARG_STRING, NULL, N_("Run this command when dirty paths require a refresh"), N_("COMMAND")},
        {"refresh-delay", 0, 0, G_OPTION_ARG_INT, NULL, N_("Delay in seconds before dispatching the refresh command"), N_("SECONDS")},
        {"refresh-retry-delay", 0, 0, G_OPTION_ARG_INT, NULL, N_("Retry delay in seconds after a failed refresh command"), N_("SECONDS")},
        {"print-socket-path", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Print the resolved socket path and exit"), NULL},
        {"ping", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Send PING to a running daemon and exit"), NULL},
        {"status", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Query STATUS from a running daemon and exit"), NULL},
        {"roots", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Query watched roots from a running daemon and exit"), NULL},
        {"backends", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Query available monitoring backends and exit"), NULL},
        {"dirty", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Query pending dirty rescan paths from a running daemon and exit"), NULL},
        {"take-dirty", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Pop the next pending dirty rescan path from a running daemon and exit"), NULL},
        {"version", 'v', 0, G_OPTION_ARG_NONE, NULL, N_("Print version information and exit"), NULL},
        {NULL},
    };

    entries[0].arg_data = &options.system_socket;
    entries[1].arg_data = &options.socket_path;
    entries[2].arg_data = &options.watch_paths;
    entries[3].arg_data = &options.refresh_command;
    entries[4].arg_data = &options.refresh_delay;
    entries[5].arg_data = &options.refresh_retry_delay;
    entries[6].arg_data = &options.print_socket_path;
    entries[7].arg_data = &options.ping;
    entries[8].arg_data = &options.status;
    entries[9].arg_data = &options.roots;
    entries[10].arg_data = &options.backends;
    entries[11].arg_data = &options.dirty;
    entries[12].arg_data = &options.take_dirty;
    entries[13].arg_data = &options.version;

    g_autoptr(GOptionContext) context = g_option_context_new("- FSearch daemon");
    g_option_context_add_main_entries(context, entries, GETTEXT_PACKAGE);
    if (!g_option_context_parse(context, &argc, &argv, NULL)) {
        return 1;
    }

    if (options.version) {
        g_print("fsearchd %s\n", PACKAGE_VERSION);
        return 0;
    }

    g_autofree gchar *socket_path =
        options.socket_path ? g_strdup(options.socket_path) : fsearch_ipc_get_default_socket_path(options.system_socket);

    if (options.print_socket_path) {
        g_print("%s\n", socket_path);
        return 0;
    }

    if (options.ping || options.status || options.roots || options.backends || options.dirty || options.take_dirty) {
        return fsearchd_handle_client_request(&options);
    }

    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, false);
    g_unix_signal_add(SIGINT, fsearchd_on_shutdown_signal, loop);
    g_unix_signal_add(SIGTERM, fsearchd_on_shutdown_signal, loop);

    g_autoptr(GError) error = NULL;
    g_autoptr(FsearchDaemon) daemon = fsearch_daemon_new(socket_path,
                                                         options.system_socket,
                                                         options.watch_paths,
                                                         options.refresh_command,
                                                         options.refresh_delay > 0 ? (guint)options.refresh_delay : 1,
                                                         options.refresh_retry_delay > 0 ? (guint)options.refresh_retry_delay : 30);
    if (!fsearch_daemon_start(daemon, &error)) {
        g_printerr("fsearchd: %s\n", error ? error->message : "failed to start daemon");
        return 1;
    }

    g_main_loop_run(loop);
    g_clear_pointer(&options.refresh_command, g_free);
    g_strfreev(options.watch_paths);
    return 0;
}
