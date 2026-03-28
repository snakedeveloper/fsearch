#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "fsearch_monitor_backend.h"

#include <stdbool.h>

const FsearchMonitorBackendInfo *
fsearch_monitor_backends_get_all(gsize *length) {
    static const FsearchMonitorBackendInfo backends[] = {
        {
            .type = FSEARCH_MONITOR_BACKEND_TYPE_INOTIFY,
            .name = "inotify",
            .available = true,
            .requires_privileges = false,
            .description = "recursive per-directory watcher with path-friendly semantics",
        },
        {
            .type = FSEARCH_MONITOR_BACKEND_TYPE_FANOTIFY,
            .name = "fanotify",
            .available = true,
            .requires_privileges = true,
            .description = "mount/filesystem monitor for privileged daemon use",
        },
    };

    if (length) {
        *length = G_N_ELEMENTS(backends);
    }
    return backends;
}

const char *
fsearch_monitor_backend_type_to_string(FsearchMonitorBackendType type) {
    switch (type) {
    case FSEARCH_MONITOR_BACKEND_TYPE_INOTIFY:
        return "inotify";
    case FSEARCH_MONITOR_BACKEND_TYPE_FANOTIFY:
        return "fanotify";
    default:
        return "unknown";
    }
}

const char *
fsearch_monitor_event_type_to_string(FsearchMonitorEventType type) {
    switch (type) {
    case FSEARCH_MONITOR_EVENT_TYPE_CREATE:
        return "create";
    case FSEARCH_MONITOR_EVENT_TYPE_DELETE:
        return "delete";
    case FSEARCH_MONITOR_EVENT_TYPE_MODIFY:
        return "modify";
    case FSEARCH_MONITOR_EVENT_TYPE_ATTRIB:
        return "attrib";
    case FSEARCH_MONITOR_EVENT_TYPE_MOVE_FROM:
        return "move_from";
    case FSEARCH_MONITOR_EVENT_TYPE_MOVE_TO:
        return "move_to";
    case FSEARCH_MONITOR_EVENT_TYPE_OVERFLOW:
        return "overflow";
    case FSEARCH_MONITOR_EVENT_TYPE_ROOT_GONE:
        return "root_gone";
    default:
        return "unknown";
    }
}

gchar *
fsearch_monitor_backends_format_summary(void) {
    gsize num_backends = 0;
    const FsearchMonitorBackendInfo *backends = fsearch_monitor_backends_get_all(&num_backends);

    g_autoptr(GString) summary = g_string_new(NULL);
    for (gsize i = 0; i < num_backends; ++i) {
        const FsearchMonitorBackendInfo *backend = &backends[i];

        if (i > 0) {
            g_string_append_c(summary, ',');
        }

        g_string_append(summary, backend->name);
        g_string_append_c(summary, '[');
        g_string_append(summary, backend->available ? "available" : "unavailable");
        if (backend->requires_privileges) {
            g_string_append(summary, "+privileged");
        }
        g_string_append_c(summary, ']');
    }

    return g_string_free(g_steal_pointer(&summary), false);
}
