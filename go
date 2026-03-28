#!/usr/bin/env sh
set -e
cd "$(dirname "$0")" || exit 1
meson compile -C build
exec build/src/fsearch "$@"
