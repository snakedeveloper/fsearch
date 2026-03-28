#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "fsearch_monitor_manager.h"

#include "fsearch_monitor_inotify.h"
#include "fsearch_monitor_dirty_queue.h"

#include <stdbool.h>

typedef struct FsearchMonitorCounters {
    guint64 total;
    guint64 created;
    guint64 deleted;
    guint64 modified;
    guint64 attrib;
    guint64 moved_from;
    guint64 moved_to;
    guint64 overflow;
    guint64 root_gone;
} FsearchMonitorCounters;

struct FsearchMonitorManager {
    gchar **roots;
    gchar *refresh_command;
    guint refresh_delay_seconds;
    guint refresh_retry_delay_seconds;
    FsearchMonitorBackendType active_backend;
    gboolean running;
    gboolean refresh_running;
    guint refresh_timeout_id;
    guint refresh_watch_id;
    GPid refresh_pid;
    gint last_refresh_wait_status;
    FsearchMonitorCounters counters;
    FsearchMonitorDirtyQueue *dirty_queue;
    GPtrArray *refresh_inflight_paths;
    FsearchMonitorInotify *inotify_backend;
};

static char *
fsearch_monitor_manager_get_parent_dir(const char *path) {
    g_return_val_if_fail(path, NULL);

    if (strcmp(path, G_DIR_SEPARATOR_S) == 0) {
        return g_strdup(path);
    }
    return g_path_get_dirname(path);
}

static char *
fsearch_monitor_manager_get_dirty_target(const FsearchMonitorEvent *event) {
    g_return_val_if_fail(event, NULL);

    switch (event->type) {
    case FSEARCH_MONITOR_EVENT_TYPE_OVERFLOW:
    case FSEARCH_MONITOR_EVENT_TYPE_ROOT_GONE:
        return event->root_path ? g_strdup(event->root_path) : NULL;
    case FSEARCH_MONITOR_EVENT_TYPE_CREATE:
    case FSEARCH_MONITOR_EVENT_TYPE_DELETE:
    case FSEARCH_MONITOR_EVENT_TYPE_MODIFY:
    case FSEARCH_MONITOR_EVENT_TYPE_ATTRIB:
    case FSEARCH_MONITOR_EVENT_TYPE_MOVE_FROM:
    case FSEARCH_MONITOR_EVENT_TYPE_MOVE_TO:
        if (!event->path) {
            return event->root_path ? g_strdup(event->root_path) : NULL;
        }
        return fsearch_monitor_manager_get_parent_dir(event->path);
    default:
        return NULL;
    }
}

static gchar **
fsearch_monitor_manager_normalize_roots(char **roots) {
    if (!roots || !roots[0]) {
        return NULL;
    }

    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func(g_free);

    for (gchar **root = roots; *root; ++root) {
        if (!root[0] || !(*root)[0]) {
            continue;
        }

        g_autofree gchar *normalized = g_canonicalize_filename(*root, NULL);
        if (g_hash_table_contains(seen, normalized)) {
            continue;
        }

        g_hash_table_add(seen, g_strdup(normalized));
        g_ptr_array_add(result, g_strdup(normalized));
    }

    g_ptr_array_add(result, NULL);
    return (gchar **)g_ptr_array_free(g_steal_pointer(&result), false);
}

static void
fsearch_monitor_manager_schedule_refresh(FsearchMonitorManager *manager);

static gboolean
fsearch_monitor_manager_dispatch_refresh(gpointer user_data);

static void
fsearch_monitor_manager_schedule_refresh_in(FsearchMonitorManager *manager, guint delay_seconds) {
    g_return_if_fail(manager);

    if (!manager->refresh_command || manager->refresh_running || manager->refresh_timeout_id != 0) {
        return;
    }
    if (fsearch_monitor_dirty_queue_get_count(manager->dirty_queue) == 0) {
        return;
    }

    manager->refresh_timeout_id = g_timeout_add_seconds(MAX(delay_seconds, 1), fsearch_monitor_manager_dispatch_refresh, manager);
}

static void
fsearch_monitor_manager_on_event(const FsearchMonitorEvent *event, gpointer user_data) {
    FsearchMonitorManager *manager = user_data;
    g_return_if_fail(manager);
    g_return_if_fail(event);

    manager->counters.total++;
    switch (event->type) {
    case FSEARCH_MONITOR_EVENT_TYPE_CREATE:
        manager->counters.created++;
        break;
    case FSEARCH_MONITOR_EVENT_TYPE_DELETE:
        manager->counters.deleted++;
        break;
    case FSEARCH_MONITOR_EVENT_TYPE_MODIFY:
        manager->counters.modified++;
        break;
    case FSEARCH_MONITOR_EVENT_TYPE_ATTRIB:
        manager->counters.attrib++;
        break;
    case FSEARCH_MONITOR_EVENT_TYPE_MOVE_FROM:
        manager->counters.moved_from++;
        break;
    case FSEARCH_MONITOR_EVENT_TYPE_MOVE_TO:
        manager->counters.moved_to++;
        break;
    case FSEARCH_MONITOR_EVENT_TYPE_OVERFLOW:
        manager->counters.overflow++;
        break;
    case FSEARCH_MONITOR_EVENT_TYPE_ROOT_GONE:
        manager->counters.root_gone++;
        break;
    default:
        break;
    }

    g_autofree char *dirty_target = fsearch_monitor_manager_get_dirty_target(event);
    if (dirty_target) {
        const gboolean inserted = fsearch_monitor_dirty_queue_add(manager->dirty_queue, dirty_target);
        if (inserted) {
            g_debug("[fsearchd] queued dirty subtree: %s", dirty_target);
            fsearch_monitor_manager_schedule_refresh(manager);
        }
    }

    g_debug("[fsearchd] monitor event type=%s path=%s root=%s dir=%d mask=0x%x",
            fsearch_monitor_event_type_to_string(event->type),
            event->path ? event->path : "-",
            event->root_path ? event->root_path : "-",
            event->is_directory,
            event->raw_mask);
}

static void
fsearch_monitor_manager_requeue_inflight_paths(FsearchMonitorManager *manager) {
    if (!manager->refresh_inflight_paths) {
        return;
    }

    fsearch_monitor_dirty_queue_requeue_all(manager->dirty_queue, manager->refresh_inflight_paths);
    g_clear_pointer(&manager->refresh_inflight_paths, g_ptr_array_unref);
}

static void
fsearch_monitor_manager_on_refresh_exit(GPid pid, gint wait_status, gpointer user_data) {
    FsearchMonitorManager *manager = user_data;
    g_return_if_fail(manager);

    manager->refresh_running = false;
    manager->refresh_watch_id = 0;
    manager->refresh_pid = 0;
    manager->last_refresh_wait_status = wait_status;

    g_autoptr(GError) error = NULL;
    if (!g_spawn_check_wait_status(wait_status, &error)) {
        g_warning("[fsearchd] refresh command failed: %s", error->message);
        fsearch_monitor_manager_requeue_inflight_paths(manager);
    }
    else {
        g_message("[fsearchd] refresh command finished successfully");
        g_clear_pointer(&manager->refresh_inflight_paths, g_ptr_array_unref);
    }

    g_spawn_close_pid(pid);
    if (fsearch_monitor_dirty_queue_get_count(manager->dirty_queue) > 0) {
        fsearch_monitor_manager_schedule_refresh_in(manager, manager->refresh_retry_delay_seconds);
    }
}

static gboolean
fsearch_monitor_manager_dispatch_refresh(gpointer user_data) {
    FsearchMonitorManager *manager = user_data;
    g_return_val_if_fail(manager, G_SOURCE_REMOVE);

    manager->refresh_timeout_id = 0;

    if (!manager->refresh_command || manager->refresh_running) {
        return G_SOURCE_REMOVE;
    }

    manager->refresh_inflight_paths = fsearch_monitor_dirty_queue_take_all(manager->dirty_queue);
    if (!manager->refresh_inflight_paths || manager->refresh_inflight_paths->len == 0) {
        g_clear_pointer(&manager->refresh_inflight_paths, g_ptr_array_unref);
        return G_SOURCE_REMOVE;
    }

    g_autoptr(GError) error = NULL;
    gint argc = 0;
    g_auto(GStrv) argv = NULL;
    if (!g_shell_parse_argv(manager->refresh_command, &argc, &argv, &error)) {
        g_warning("[fsearchd] failed to parse refresh command: %s", error->message);
        fsearch_monitor_manager_requeue_inflight_paths(manager);
        return G_SOURCE_REMOVE;
    }

    if (!g_spawn_async(NULL,
                       argv,
                       NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL,
                       NULL,
                       &manager->refresh_pid,
                       &error)) {
        g_warning("[fsearchd] failed to spawn refresh command: %s", error->message);
        fsearch_monitor_manager_requeue_inflight_paths(manager);
        return G_SOURCE_REMOVE;
    }

    manager->refresh_running = true;
    manager->refresh_watch_id = g_child_watch_add(manager->refresh_pid, fsearch_monitor_manager_on_refresh_exit, manager);
    g_message("[fsearchd] started refresh command for %u dirty paths", manager->refresh_inflight_paths->len);
    return G_SOURCE_REMOVE;
}

static void
fsearch_monitor_manager_schedule_refresh(FsearchMonitorManager *manager) {
    fsearch_monitor_manager_schedule_refresh_in(manager, manager->refresh_delay_seconds);
}

FsearchMonitorManager *
fsearch_monitor_manager_new(char **roots,
                            const char *refresh_command,
                            guint refresh_delay_seconds,
                            guint refresh_retry_delay_seconds) {
    FsearchMonitorManager *manager = g_new0(FsearchMonitorManager, 1);
    manager->roots = fsearch_monitor_manager_normalize_roots(roots);
    manager->refresh_command = refresh_command && refresh_command[0] != '\0' ? g_strdup(refresh_command) : NULL;
    manager->refresh_delay_seconds = refresh_delay_seconds;
    manager->refresh_retry_delay_seconds = refresh_retry_delay_seconds;
    manager->active_backend = FSEARCH_MONITOR_BACKEND_TYPE_UNKNOWN;
    manager->dirty_queue = fsearch_monitor_dirty_queue_new();
    return manager;
}

void
fsearch_monitor_manager_free(FsearchMonitorManager *manager) {
    if (!manager) {
        return;
    }

    fsearch_monitor_manager_stop(manager);
    g_clear_pointer(&manager->dirty_queue, fsearch_monitor_dirty_queue_free);
    g_clear_pointer(&manager->refresh_inflight_paths, g_ptr_array_unref);
    g_clear_pointer(&manager->refresh_command, g_free);
    g_clear_pointer(&manager->roots, g_strfreev);
    g_free(manager);
}

gboolean
fsearch_monitor_manager_start(FsearchMonitorManager *manager, GError **error) {
    g_return_val_if_fail(manager, false);

    if (manager->running) {
        return true;
    }

    if (!manager->roots || !manager->roots[0]) {
        manager->active_backend = FSEARCH_MONITOR_BACKEND_TYPE_UNKNOWN;
        manager->running = false;
        return true;
    }

    manager->inotify_backend =
        fsearch_monitor_inotify_new(manager->roots, fsearch_monitor_manager_on_event, manager);
    if (!fsearch_monitor_inotify_start(manager->inotify_backend, error)) {
        g_clear_pointer(&manager->inotify_backend, fsearch_monitor_inotify_free);
        return false;
    }

    manager->active_backend = FSEARCH_MONITOR_BACKEND_TYPE_INOTIFY;
    manager->running = true;
    return true;
}

void
fsearch_monitor_manager_stop(FsearchMonitorManager *manager) {
    if (!manager) {
        return;
    }

    manager->running = false;
    manager->active_backend = FSEARCH_MONITOR_BACKEND_TYPE_UNKNOWN;
    if (manager->refresh_timeout_id != 0) {
        g_source_remove(manager->refresh_timeout_id);
        manager->refresh_timeout_id = 0;
    }
    if (manager->refresh_watch_id != 0) {
        g_source_remove(manager->refresh_watch_id);
        manager->refresh_watch_id = 0;
    }
    manager->refresh_running = false;
    if (manager->refresh_pid != 0) {
        g_spawn_close_pid(manager->refresh_pid);
        manager->refresh_pid = 0;
    }
    g_clear_pointer(&manager->refresh_inflight_paths, g_ptr_array_unref);
    g_clear_pointer(&manager->inotify_backend, fsearch_monitor_inotify_free);
}

gchar *
fsearch_monitor_manager_format_status(FsearchMonitorManager *manager) {
    g_return_val_if_fail(manager, NULL);

    const guint32 root_count = manager->roots ? g_strv_length(manager->roots) : 0;
    const guint32 watch_count =
        manager->inotify_backend ? fsearch_monitor_inotify_get_watch_count(manager->inotify_backend) : 0;
    const guint32 dirty_count = manager->dirty_queue ? fsearch_monitor_dirty_queue_get_count(manager->dirty_queue) : 0;

    return g_strdup_printf("monitor_state=%s backend=%s roots=%u watches=%u events_total=%" G_GUINT64_FORMAT
                           " create=%" G_GUINT64_FORMAT " delete=%" G_GUINT64_FORMAT " modify=%" G_GUINT64_FORMAT
                           " attrib=%" G_GUINT64_FORMAT " move_from=%" G_GUINT64_FORMAT
                           " move_to=%" G_GUINT64_FORMAT " overflow=%" G_GUINT64_FORMAT
                           " dirty=%u"
                           " refresh_enabled=%d refresh_running=%d"
                           " root_gone=%" G_GUINT64_FORMAT,
                           manager->running ? "running" : "idle",
                           fsearch_monitor_backend_type_to_string(manager->active_backend),
                           root_count,
                           watch_count,
                           manager->counters.total,
                           manager->counters.created,
                           manager->counters.deleted,
                           manager->counters.modified,
                           manager->counters.attrib,
                           manager->counters.moved_from,
                           manager->counters.moved_to,
                           manager->counters.overflow,
                           dirty_count,
                           manager->refresh_command != NULL,
                           manager->refresh_running,
                           manager->counters.root_gone);
}

gchar *
fsearch_monitor_manager_format_roots(FsearchMonitorManager *manager) {
    g_return_val_if_fail(manager, NULL);

    const guint32 root_count = manager->roots ? g_strv_length(manager->roots) : 0;
    if (root_count == 0) {
        return g_strdup("count=0 paths=");
    }

    g_autoptr(GString) escaped_paths = g_string_new(NULL);
    for (guint32 i = 0; i < root_count; ++i) {
        g_autofree gchar *escaped = g_strescape(manager->roots[i], NULL);
        if (i > 0) {
            g_string_append_c(escaped_paths, ';');
        }
        g_string_append(escaped_paths, escaped);
    }

    return g_strdup_printf("count=%u paths=%s", root_count, escaped_paths->str);
}

gchar *
fsearch_monitor_manager_format_dirty_paths(FsearchMonitorManager *manager) {
    g_return_val_if_fail(manager, NULL);
    return fsearch_monitor_dirty_queue_format(manager->dirty_queue);
}

char *
fsearch_monitor_manager_take_dirty_path(FsearchMonitorManager *manager) {
    g_return_val_if_fail(manager, NULL);
    return fsearch_monitor_dirty_queue_take_next(manager->dirty_queue);
}
