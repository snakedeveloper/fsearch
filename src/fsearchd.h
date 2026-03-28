#pragma once

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct FsearchDaemon FsearchDaemon;

FsearchDaemon *
fsearch_daemon_new(const char *socket_path,
                   gboolean system_scope,
                   char **monitor_roots,
                   const char *refresh_command,
                   guint refresh_delay_seconds,
                   guint refresh_retry_delay_seconds);

void
fsearch_daemon_free(FsearchDaemon *daemon);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FsearchDaemon, fsearch_daemon_free)

gboolean
fsearch_daemon_start(FsearchDaemon *daemon, GError **error);

void
fsearch_daemon_stop(FsearchDaemon *daemon);

const char *
fsearch_daemon_get_socket_path(FsearchDaemon *daemon);

G_END_DECLS
