#pragma once

#include "fsearch_monitor_backend.h"

#include <glib.h>

G_BEGIN_DECLS

typedef struct FsearchMonitorInotify FsearchMonitorInotify;
typedef void (*FsearchMonitorEventCallback)(const FsearchMonitorEvent *event, gpointer user_data);

FsearchMonitorInotify *
fsearch_monitor_inotify_new(char **roots, FsearchMonitorEventCallback event_cb, gpointer user_data);

void
fsearch_monitor_inotify_free(FsearchMonitorInotify *monitor);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FsearchMonitorInotify, fsearch_monitor_inotify_free)

gboolean
fsearch_monitor_inotify_start(FsearchMonitorInotify *monitor, GError **error);

void
fsearch_monitor_inotify_stop(FsearchMonitorInotify *monitor);

gboolean
fsearch_monitor_inotify_is_running(FsearchMonitorInotify *monitor);

guint32
fsearch_monitor_inotify_get_watch_count(FsearchMonitorInotify *monitor);

G_END_DECLS
