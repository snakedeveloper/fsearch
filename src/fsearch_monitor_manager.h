#pragma once

#include "fsearch_monitor_backend.h"

#include <glib.h>

G_BEGIN_DECLS

typedef struct FsearchMonitorManager FsearchMonitorManager;

FsearchMonitorManager *
fsearch_monitor_manager_new(char **roots,
                            const char *refresh_command,
                            guint refresh_delay_seconds,
                            guint refresh_retry_delay_seconds);

void
fsearch_monitor_manager_free(FsearchMonitorManager *manager);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FsearchMonitorManager, fsearch_monitor_manager_free)

gboolean
fsearch_monitor_manager_start(FsearchMonitorManager *manager, GError **error);

void
fsearch_monitor_manager_stop(FsearchMonitorManager *manager);

gchar *
fsearch_monitor_manager_format_status(FsearchMonitorManager *manager);

gchar *
fsearch_monitor_manager_format_roots(FsearchMonitorManager *manager);

gchar *
fsearch_monitor_manager_format_dirty_paths(FsearchMonitorManager *manager);

char *
fsearch_monitor_manager_take_dirty_path(FsearchMonitorManager *manager);

G_END_DECLS
