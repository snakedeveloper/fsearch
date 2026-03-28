resources = gnome.compile_resources('ui_resources',
    'gresource.xml',
    source_dir: '.',
    c_name: 'fsearch_ui',
)

resources += gnome.compile_resources('icon_resources',
    join_paths(meson.source_root(), 'data', 'gresource.xml'),
    source_dir: join_paths(meson.source_root(), 'data'),
    c_name: 'fsearch_icons',
)

monitor_common_deps = [
    dependency('gio-unix-2.0', version: '>= 2.50'),
]

monitor_common_sources = [
    'fsearch_ipc.c',
    'fsearch_monitor_backend.c',
    'fsearch_monitor_dirty_queue.c',
]

libfsearch_monitor_common = static_library(
    'fsearch-monitor-common',
    monitor_common_sources,
    dependencies: monitor_common_deps,
    include_directories: fsearch_include_dirs,
)

libfsearch_monitor_common_dep = declare_dependency(
    link_with: libfsearch_monitor_common,
    include_directories: [
        fsearch_include_dirs,
    ],
    dependencies: monitor_common_deps,
)

libfsearch_sources = [
    resources,
    'fsearch.c',
    'fsearch_array.c',
    'fsearch_clipboard.c',
    'fsearch_config.c',
    'fsearch_database.c',
    'fsearch_database_entry.c',
    'fsearch_database_index.c',
    'fsearch_database_search.c',
    'fsearch_database_view.c',
    'fsearch_exclude_path.c',
    'fsearch_file_utils.c',
    'fsearch_filter.c',
    'fsearch_filter_editor.c',
    'fsearch_filter_manager.c',
    'fsearch_index.c',
    'fsearch_list_view.c',
    'fsearch_listview_popup.c',
    'fsearch_memory_pool.c',
    'fsearch_preferences_ui.c',
    'fsearch_preferences_widgets.c',
    'fsearch_query.c',
    'fsearch_query_match_data.c',
    'fsearch_query_matchers.c',
    'fsearch_query_node.c',
    'fsearch_query_lexer.c',
    'fsearch_query_parser.c',
    'fsearch_query_tree.c',
    'fsearch_result_view.c',
    'fsearch_selection.c',
    'fsearch_size_utils.c',
    'fsearch_statusbar.c',
    'fsearch_string_utils.c',
    'fsearch_task.c',
    'fsearch_thread_pool.c',
    'fsearch_time_utils.c',
    'fsearch_ui_utils.c',
    'fsearch_utf.c',
    'fsearch_window.c',
    'fsearch_window_actions.c',
    'fsearch_preview.c',
]

if build_machine.system()=='darwin'
    libfsearch_sources+=['strverscmp.c']
endif

fsearch_deps = [
    cc.find_library('m', required: true),
    dependency('gio-unix-2.0', version: '>= 2.50'),
    dependency('gtk+-3.0', version: '>= 3.18'),
    dependency('libpcre2-8', version: '>= 10.21'),
    dependency('icu-uc', version: '>= 3.8'),
]

libfsearch = static_library(
    'fsearch',
    libfsearch_sources,
    dependencies: fsearch_deps,
    include_directories: fsearch_include_dirs,
)

libfsearch_include_dirs = include_directories('.')

libfsearch_dep = declare_dependency(
    link_with: libfsearch,
    include_directories: [
        fsearch_include_dirs,
        libfsearch_include_dirs
    ],
    dependencies: fsearch_deps,
    sources: resources
)

fsearch = executable('fsearch', 'main.c',
    include_directories: fsearch_include_dirs,
    dependencies: libfsearch_dep,
    install: true,
)

fsearchd = executable(
    'fsearchd',
    [
        'fsearch_monitor_inotify.c',
        'fsearch_monitor_manager.c',
        'fsearchd.c',
        'fsearchd_main.c',
    ],
    include_directories: fsearch_include_dirs,
    dependencies: libfsearch_monitor_common_dep,
    install: true,
)

subdir('tests')
