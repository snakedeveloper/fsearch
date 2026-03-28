#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    FSEARCH_MONITOR_BACKEND_TYPE_UNKNOWN = 0,
    FSEARCH_MONITOR_BACKEND_TYPE_INOTIFY,
    FSEARCH_MONITOR_BACKEND_TYPE_FANOTIFY,
} FsearchMonitorBackendType;

typedef enum {
    FSEARCH_MONITOR_EVENT_TYPE_UNKNOWN = 0,
    FSEARCH_MONITOR_EVENT_TYPE_CREATE,
    FSEARCH_MONITOR_EVENT_TYPE_DELETE,
    FSEARCH_MONITOR_EVENT_TYPE_MODIFY,
    FSEARCH_MONITOR_EVENT_TYPE_ATTRIB,
    FSEARCH_MONITOR_EVENT_TYPE_MOVE_FROM,
    FSEARCH_MONITOR_EVENT_TYPE_MOVE_TO,
    FSEARCH_MONITOR_EVENT_TYPE_OVERFLOW,
    FSEARCH_MONITOR_EVENT_TYPE_ROOT_GONE,
} FsearchMonitorEventType;

typedef struct FsearchMonitorEvent {
    FsearchMonitorEventType type;
    const char *root_path;
    const char *path;
    gboolean is_directory;
    guint32 raw_mask;
} FsearchMonitorEvent;

typedef struct FsearchMonitorBackendInfo {
    FsearchMonitorBackendType type;
    const char *name;
    gboolean available;
    gboolean requires_privileges;
    const char *description;
} FsearchMonitorBackendInfo;

const FsearchMonitorBackendInfo *
fsearch_monitor_backends_get_all(gsize *length);

const char *
fsearch_monitor_backend_type_to_string(FsearchMonitorBackendType type);

const char *
fsearch_monitor_event_type_to_string(FsearchMonitorEventType type);

gchar *
fsearch_monitor_backends_format_summary(void);

G_END_DECLS
