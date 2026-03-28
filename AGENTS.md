# AGENTS.md

## Purpose

Keep short, high-signal notes here when work uncovers architectural decisions or repo-specific operational details that future agents should not have to rediscover.

## Current Monitoring Direction

- Goal: make FSearch behave more like Everything by moving toward a persistent daemon-backed index.
- Chosen direction: `fsearchd` system/user daemon + IPC + monitoring backends.
- Do not model Linux as if it had a direct NTFS `MFT + USN Journal` equivalent.
- Preferred architecture:
  - persistent daemon keeps monitors alive and index warm
  - GUI acts as a client
  - multiple monitoring backends
  - dirty-subtree / repair rescan is a first-class mechanism, not a failure-only path

## Reverse Engineering Findings About Everything

- `everything.exe` clearly contains separate paths for:
  - NTFS monitoring
  - ReFS monitoring
  - folder-based monitoring using `ReadDirectoryChangesW`
- Strings/imports observed locally indicate:
  - `USN_RECORD`
  - `FSCTL_QUERY_USN_JOURNAL`
  - `FSCTL_GET_NTFS_FILE_RECORD`
  - `ReadDirectoryChangesW FAIL`
  - `folder_rescan_if_full_list`
  - service/admin privilege model
- Conclusion: Everything already uses multi-backend monitoring plus rescan/repair logic. FSearch should copy that shape, not force a `fanotify-only` design.

## Current Code Added

- New daemon target: `fsearchd`
- New files:
  - `src/fsearchd.c`
  - `src/fsearchd.h`
  - `src/fsearchd_main.c`
  - `src/fsearch_ipc.c`
  - `src/fsearch_ipc.h`
  - `src/fsearch_monitor_backend.c`
  - `src/fsearch_monitor_backend.h`
- `src/meson.build` now builds `fsearchd` and a small shared static library for IPC/backend metadata.

## Current Daemon Status

- `fsearchd` now has a working first monitor backend: `inotify`.
- Current daemon capabilities:
  - UNIX socket listener
  - `PING`
  - `STATUS`
  - `BACKENDS`
  - `ROOTS`
  - `DIRTY`
  - `TAKE_DIRTY`
  - startup watch roots via repeated `--watch PATH`
  - optional auto-refresh via `--refresh-command`
- Current monitor behavior:
  - recursive watch registration for configured roots
  - new subdirectories are watched automatically
  - removed directories release their watches
  - counters are tracked for create/delete/modify/attrib/move/overflow/root_gone
- Dirty queue behavior:
  - monitor events are converted into pending dirty subtree paths
  - queue coalesces parent/child paths, so deeper dirty entries are dropped if a parent is already queued
  - for ordinary file changes, the queue currently stores the parent directory path
  - for root loss / overflow, the queue stores the monitored root path
- Refresh behavior:
  - if `--refresh-command` is set, dirty queue can trigger a spawned refresh command after `--refresh-delay`
  - if the refresh command succeeds, the inflight dirty batch is dropped
  - if the refresh command fails, the inflight dirty batch is re-queued
  - retries are throttled by `--refresh-retry-delay` to avoid tight loops
- It still does **not** own the FSearch database or push changes into the GUI.

## Build / Smoke Test

- After adding new Meson targets, run:
  - `meson setup --reconfigure build`
- Build daemon:
  - `meson compile -C build fsearchd`
- Example smoke test:
  - start: `./build/src/fsearchd --socket-path /tmp/fsearchd.sock --watch /tmp/some-root`
  - ping: `./build/src/fsearchd --socket-path /tmp/fsearchd.sock --ping`
  - status: `./build/src/fsearchd --socket-path /tmp/fsearchd.sock --status`
  - roots: `./build/src/fsearchd --socket-path /tmp/fsearchd.sock --roots`
  - dirty: `./build/src/fsearchd --socket-path /tmp/fsearchd.sock --dirty`
  - take dirty: `./build/src/fsearchd --socket-path /tmp/fsearchd.sock --take-dirty`
  - auto refresh (safe test): `./build/src/fsearchd --socket-path /tmp/fsearchd.sock --watch /tmp/some-root --refresh-command "sh -c 'echo refresh >> /tmp/fsearchd.log'" --refresh-delay 1`
- Verified locally:
  - daemon started with a root containing a subdirectory
  - initial watch count matched recursive directories
  - creating a new directory increased watch count
  - deleting that directory reduced watch count again
  - event counters increased as expected for create/delete/modify/move activity
  - multiple nested changes under the same subtree coalesced to one dirty path
  - two file changes in existing sibling subtrees produced two dirty paths
  - successful refresh command drained dirty queue
  - failed refresh command re-queued dirty paths and retried using the configured retry delay

## Next Recommended Steps

- Add dirty subtree queue
- Integrate targeted subtree rescan with FSearch snapshot replacement model
- Add `fanotify` backend after the path-based flow is correct
- Connect daemon-side roots to real FSearch config instead of CLI-only `--watch`

