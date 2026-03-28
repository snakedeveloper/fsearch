#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "fsearch_monitor_inotify.h"

#include <errno.h>
#include <glib-unix.h>
#include <stdbool.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct FsearchInotifyWatch {
    gchar *path;
    gchar *root_path;
} FsearchInotifyWatch;

struct FsearchMonitorInotify {
    gchar **roots;
    int fd;
    guint fd_source_id;
    GHashTable *wd_to_watch;
    GHashTable *path_to_wd;
    FsearchMonitorEventCallback event_cb;
    gpointer event_cb_data;
};

static void
fsearch_inotify_watch_free(FsearchInotifyWatch *watch) {
    if (!watch) {
        return;
    }

    g_clear_pointer(&watch->path, g_free);
    g_clear_pointer(&watch->root_path, g_free);
    g_free(watch);
}

static gboolean
fsearch_monitor_inotify_emit_event(FsearchMonitorInotify *monitor,
                                   FsearchMonitorEventType type,
                                   const char *root_path,
                                   const char *path,
                                   gboolean is_directory,
                                   guint32 raw_mask) {
    if (!monitor->event_cb) {
        return true;
    }

    FsearchMonitorEvent event = {
        .type = type,
        .root_path = root_path,
        .path = path,
        .is_directory = is_directory,
        .raw_mask = raw_mask,
    };
    monitor->event_cb(&event, monitor->event_cb_data);
    return true;
}

static void
fsearch_monitor_inotify_remove_watch(FsearchMonitorInotify *monitor, int wd, gboolean remove_from_kernel) {
    FsearchInotifyWatch *watch = g_hash_table_lookup(monitor->wd_to_watch, GINT_TO_POINTER(wd));
    if (!watch) {
        return;
    }

    if (remove_from_kernel && monitor->fd >= 0) {
        if (inotify_rm_watch(monitor->fd, wd) != 0 && errno != EINVAL) {
            g_debug("[fsearchd] inotify_rm_watch failed for %s: %s", watch->path, g_strerror(errno));
        }
    }

    g_hash_table_remove(monitor->path_to_wd, watch->path);
    g_hash_table_remove(monitor->wd_to_watch, GINT_TO_POINTER(wd));
}

static gboolean
fsearch_monitor_inotify_add_watch_recursive(FsearchMonitorInotify *monitor,
                                            const char *path,
                                            const char *root_path,
                                            GError **error) {
    g_return_val_if_fail(monitor, false);
    g_return_val_if_fail(path, false);
    g_return_val_if_fail(root_path, false);

    if (g_hash_table_contains(monitor->path_to_wd, path)) {
        return true;
    }

    struct stat st = {0};
    if (g_lstat(path, &st) != 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "failed to stat monitored path %s: %s",
                    path,
                    g_strerror(errno));
        return false;
    }

    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
        return true;
    }

    const guint32 mask = IN_CREATE | IN_DELETE | IN_ATTRIB | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
                         IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
    const int wd = inotify_add_watch(monitor->fd, path, mask);
    if (wd < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "failed to add inotify watch for %s: %s",
                    path,
                    g_strerror(errno));
        return false;
    }

    FsearchInotifyWatch *watch = g_new0(FsearchInotifyWatch, 1);
    watch->path = g_strdup(path);
    watch->root_path = g_strdup(root_path);
    g_hash_table_insert(monitor->wd_to_watch, GINT_TO_POINTER(wd), watch);
    g_hash_table_insert(monitor->path_to_wd, g_strdup(path), GINT_TO_POINTER(wd));

    g_autoptr(GDir) dir = g_dir_open(path, 0, error);
    if (!dir) {
        fsearch_monitor_inotify_remove_watch(monitor, wd, true);
        return false;
    }

    const gchar *entry_name = NULL;
    while ((entry_name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *child_path = g_build_filename(path, entry_name, NULL);
        if (!fsearch_monitor_inotify_add_watch_recursive(monitor, child_path, root_path, error)) {
            return false;
        }
    }

    return true;
}

static gboolean
fsearch_monitor_inotify_handle_event(FsearchMonitorInotify *monitor, const struct inotify_event *event) {
    g_return_val_if_fail(monitor, true);
    g_return_val_if_fail(event, true);

    if ((event->mask & IN_Q_OVERFLOW) != 0) {
        fsearch_monitor_inotify_emit_event(monitor, FSEARCH_MONITOR_EVENT_TYPE_OVERFLOW, NULL, NULL, false, event->mask);
        g_warning("[fsearchd] inotify queue overflow detected");
        return true;
    }

    FsearchInotifyWatch *watch = g_hash_table_lookup(monitor->wd_to_watch, GINT_TO_POINTER(event->wd));
    if (!watch) {
        return true;
    }

    const gboolean is_directory = (event->mask & IN_ISDIR) != 0;
    g_autofree gchar *full_path = event->len > 0 && event->name[0] != '\0' ? g_build_filename(watch->path, event->name, NULL)
                                                                            : g_strdup(watch->path);

    if ((event->mask & IN_CREATE) != 0) {
        fsearch_monitor_inotify_emit_event(monitor,
                                           FSEARCH_MONITOR_EVENT_TYPE_CREATE,
                                           watch->root_path,
                                           full_path,
                                           is_directory,
                                           event->mask);
    }
    if ((event->mask & IN_DELETE) != 0) {
        fsearch_monitor_inotify_emit_event(monitor,
                                           FSEARCH_MONITOR_EVENT_TYPE_DELETE,
                                           watch->root_path,
                                           full_path,
                                           is_directory,
                                           event->mask);
    }
    if ((event->mask & IN_ATTRIB) != 0) {
        fsearch_monitor_inotify_emit_event(monitor,
                                           FSEARCH_MONITOR_EVENT_TYPE_ATTRIB,
                                           watch->root_path,
                                           full_path,
                                           is_directory,
                                           event->mask);
    }
    if ((event->mask & IN_CLOSE_WRITE) != 0) {
        fsearch_monitor_inotify_emit_event(monitor,
                                           FSEARCH_MONITOR_EVENT_TYPE_MODIFY,
                                           watch->root_path,
                                           full_path,
                                           is_directory,
                                           event->mask);
    }
    if ((event->mask & IN_MOVED_FROM) != 0) {
        fsearch_monitor_inotify_emit_event(monitor,
                                           FSEARCH_MONITOR_EVENT_TYPE_MOVE_FROM,
                                           watch->root_path,
                                           full_path,
                                           is_directory,
                                           event->mask);
    }
    if ((event->mask & IN_MOVED_TO) != 0) {
        fsearch_monitor_inotify_emit_event(monitor,
                                           FSEARCH_MONITOR_EVENT_TYPE_MOVE_TO,
                                           watch->root_path,
                                           full_path,
                                           is_directory,
                                           event->mask);
    }
    if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
        fsearch_monitor_inotify_emit_event(monitor,
                                           FSEARCH_MONITOR_EVENT_TYPE_ROOT_GONE,
                                           watch->root_path,
                                           watch->path,
                                           true,
                                           event->mask);
    }

    if (is_directory && ((event->mask & IN_CREATE) != 0 || (event->mask & IN_MOVED_TO) != 0)) {
        g_autoptr(GError) add_error = NULL;
        if (!fsearch_monitor_inotify_add_watch_recursive(monitor, full_path, watch->root_path, &add_error)) {
            g_warning("[fsearchd] failed to recursively watch %s: %s", full_path, add_error->message);
        }
    }

    if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) != 0) {
        fsearch_monitor_inotify_remove_watch(monitor, event->wd, false);
    }

    return true;
}

static gboolean
fsearch_monitor_inotify_on_fd_ready(gint fd, GIOCondition condition, gpointer user_data) {
    (void)fd;

    FsearchMonitorInotify *monitor = user_data;
    g_return_val_if_fail(monitor, G_SOURCE_REMOVE);

    if ((condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0) {
        g_warning("[fsearchd] inotify fd reported error condition: %u", condition);
        return G_SOURCE_CONTINUE;
    }

    char buffer[16384] = {0};
    const ssize_t bytes_read = read(monitor->fd, buffer, sizeof(buffer));
    if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EINTR) {
            return G_SOURCE_CONTINUE;
        }
        g_warning("[fsearchd] failed to read from inotify fd: %s", g_strerror(errno));
        return G_SOURCE_CONTINUE;
    }

    for (char *cursor = buffer; cursor < buffer + bytes_read;) {
        const struct inotify_event *event = (const struct inotify_event *)cursor;
        fsearch_monitor_inotify_handle_event(monitor, event);
        cursor += sizeof(struct inotify_event) + event->len;
    }

    return G_SOURCE_CONTINUE;
}

FsearchMonitorInotify *
fsearch_monitor_inotify_new(char **roots, FsearchMonitorEventCallback event_cb, gpointer user_data) {
    FsearchMonitorInotify *monitor = g_new0(FsearchMonitorInotify, 1);
    monitor->roots = g_strdupv(roots);
    monitor->fd = -1;
    monitor->wd_to_watch = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)fsearch_inotify_watch_free);
    monitor->path_to_wd = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    monitor->event_cb = event_cb;
    monitor->event_cb_data = user_data;
    return monitor;
}

void
fsearch_monitor_inotify_free(FsearchMonitorInotify *monitor) {
    if (!monitor) {
        return;
    }

    fsearch_monitor_inotify_stop(monitor);
    g_clear_pointer(&monitor->roots, g_strfreev);
    g_clear_pointer(&monitor->wd_to_watch, g_hash_table_unref);
    g_clear_pointer(&monitor->path_to_wd, g_hash_table_unref);
    g_free(monitor);
}

gboolean
fsearch_monitor_inotify_start(FsearchMonitorInotify *monitor, GError **error) {
    g_return_val_if_fail(monitor, false);

    if (monitor->fd >= 0) {
        return true;
    }

    monitor->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (monitor->fd < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "failed to initialize inotify: %s",
                    g_strerror(errno));
        return false;
    }

    for (gchar **root = monitor->roots; root && *root; ++root) {
        if (!fsearch_monitor_inotify_add_watch_recursive(monitor, *root, *root, error)) {
            fsearch_monitor_inotify_stop(monitor);
            return false;
        }
    }

    monitor->fd_source_id = g_unix_fd_add(monitor->fd, G_IO_IN | G_IO_ERR | G_IO_HUP, fsearch_monitor_inotify_on_fd_ready, monitor);
    g_message("[fsearchd] inotify backend started with %u watches", fsearch_monitor_inotify_get_watch_count(monitor));
    return true;
}

void
fsearch_monitor_inotify_stop(FsearchMonitorInotify *monitor) {
    if (!monitor) {
        return;
    }

    if (monitor->fd_source_id != 0) {
        g_source_remove(monitor->fd_source_id);
        monitor->fd_source_id = 0;
    }

    g_hash_table_remove_all(monitor->path_to_wd);
    g_hash_table_remove_all(monitor->wd_to_watch);

    if (monitor->fd >= 0) {
        close(monitor->fd);
        monitor->fd = -1;
    }
}

gboolean
fsearch_monitor_inotify_is_running(FsearchMonitorInotify *monitor) {
    g_return_val_if_fail(monitor, false);
    return monitor->fd >= 0;
}

guint32
fsearch_monitor_inotify_get_watch_count(FsearchMonitorInotify *monitor) {
    g_return_val_if_fail(monitor, 0);
    return (guint32)g_hash_table_size(monitor->wd_to_watch);
}
