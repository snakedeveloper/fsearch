#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct FsearchMonitorDirtyQueue FsearchMonitorDirtyQueue;

FsearchMonitorDirtyQueue *
fsearch_monitor_dirty_queue_new(void);

void
fsearch_monitor_dirty_queue_free(FsearchMonitorDirtyQueue *queue);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FsearchMonitorDirtyQueue, fsearch_monitor_dirty_queue_free)

gboolean
fsearch_monitor_dirty_queue_add(FsearchMonitorDirtyQueue *queue, const char *path);

guint32
fsearch_monitor_dirty_queue_get_count(FsearchMonitorDirtyQueue *queue);

GPtrArray *
fsearch_monitor_dirty_queue_take_all(FsearchMonitorDirtyQueue *queue);

void
fsearch_monitor_dirty_queue_requeue_all(FsearchMonitorDirtyQueue *queue, GPtrArray *paths);

char *
fsearch_monitor_dirty_queue_take_next(FsearchMonitorDirtyQueue *queue);

char *
fsearch_monitor_dirty_queue_format(FsearchMonitorDirtyQueue *queue);

G_END_DECLS
