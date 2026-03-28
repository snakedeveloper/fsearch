#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "fsearch_monitor_dirty_queue.h"

#include <stdbool.h>
#include <string.h>

struct FsearchMonitorDirtyQueue {
    GQueue *paths;
    GHashTable *known_paths;
};

static gboolean
path_is_same_or_parent(const char *parent_path, const char *child_path) {
    g_return_val_if_fail(parent_path, false);
    g_return_val_if_fail(child_path, false);

    if (g_strcmp0(parent_path, child_path) == 0) {
        return true;
    }

    const size_t parent_len = strlen(parent_path);
    if (parent_len == 0) {
        return false;
    }

    if (!g_str_has_prefix(child_path, parent_path)) {
        return false;
    }

    if (strcmp(parent_path, G_DIR_SEPARATOR_S) == 0) {
        return true;
    }

    return child_path[parent_len] == G_DIR_SEPARATOR;
}

FsearchMonitorDirtyQueue *
fsearch_monitor_dirty_queue_new(void) {
    FsearchMonitorDirtyQueue *queue = g_new0(FsearchMonitorDirtyQueue, 1);
    queue->paths = g_queue_new();
    queue->known_paths = g_hash_table_new(g_str_hash, g_str_equal);
    return queue;
}

void
fsearch_monitor_dirty_queue_free(FsearchMonitorDirtyQueue *queue) {
    if (!queue) {
        return;
    }

    g_clear_pointer(&queue->known_paths, g_hash_table_unref);
    if (queue->paths) {
        g_queue_free_full(queue->paths, g_free);
    }
    g_free(queue);
}

gboolean
fsearch_monitor_dirty_queue_add(FsearchMonitorDirtyQueue *queue, const char *path) {
    g_return_val_if_fail(queue, false);
    g_return_val_if_fail(path, false);

    g_autofree char *normalized_path = g_canonicalize_filename(path, NULL);
    if (!normalized_path || normalized_path[0] == '\0') {
        return false;
    }

    if (g_hash_table_contains(queue->known_paths, normalized_path)) {
        return false;
    }

    for (GList *link = queue->paths->head; link; link = link->next) {
        const char *existing_path = link->data;
        if (path_is_same_or_parent(existing_path, normalized_path)) {
            return false;
        }
    }

    for (GList *link = queue->paths->head; link;) {
        GList *next = link->next;
        const char *existing_path = link->data;
        if (path_is_same_or_parent(normalized_path, existing_path)) {
            g_hash_table_remove(queue->known_paths, existing_path);
            g_free(link->data);
            g_queue_delete_link(queue->paths, link);
        }
        link = next;
    }

    char *owned_path = g_steal_pointer(&normalized_path);
    g_queue_push_tail(queue->paths, owned_path);
    g_hash_table_add(queue->known_paths, owned_path);
    return true;
}

guint32
fsearch_monitor_dirty_queue_get_count(FsearchMonitorDirtyQueue *queue) {
    g_return_val_if_fail(queue, 0);
    return (guint32)g_queue_get_length(queue->paths);
}

GPtrArray *
fsearch_monitor_dirty_queue_take_all(FsearchMonitorDirtyQueue *queue) {
    g_return_val_if_fail(queue, NULL);

    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
    for (;;) {
        char *path = fsearch_monitor_dirty_queue_take_next(queue);
        if (!path) {
            break;
        }
        g_ptr_array_add(paths, path);
    }
    return g_steal_pointer(&paths);
}

void
fsearch_monitor_dirty_queue_requeue_all(FsearchMonitorDirtyQueue *queue, GPtrArray *paths) {
    g_return_if_fail(queue);

    if (!paths) {
        return;
    }

    for (guint i = 0; i < paths->len; ++i) {
        const char *path = g_ptr_array_index(paths, i);
        if (path) {
            fsearch_monitor_dirty_queue_add(queue, path);
        }
    }
}

char *
fsearch_monitor_dirty_queue_take_next(FsearchMonitorDirtyQueue *queue) {
    g_return_val_if_fail(queue, NULL);

    char *path = g_queue_pop_head(queue->paths);
    if (!path) {
        return NULL;
    }

    g_hash_table_remove(queue->known_paths, path);
    return path;
}

char *
fsearch_monitor_dirty_queue_format(FsearchMonitorDirtyQueue *queue) {
    g_return_val_if_fail(queue, NULL);

    const guint32 count = fsearch_monitor_dirty_queue_get_count(queue);
    if (count == 0) {
        return g_strdup("count=0 paths=");
    }

    g_autoptr(GString) escaped_paths = g_string_new(NULL);
    guint32 index = 0;
    for (GList *link = queue->paths->head; link; link = link->next, ++index) {
        g_autofree char *escaped = g_strescape(link->data, NULL);
        if (index > 0) {
            g_string_append_c(escaped_paths, ';');
        }
        g_string_append(escaped_paths, escaped);
    }

    return g_strdup_printf("count=%u paths=%s", count, escaped_paths->str);
}
