#!/bin/sh
# Start niri from a TTY (OpenRC: no systemd user session).
# Log of the current session: ~/.cache/niri/niri.log (previous: niri.log.old).

log_dir="${XDG_CACHE_HOME:-$HOME/.cache}/niri"
log="$log_dir/niri.log"

mkdir -p "$log_dir"
[ -f "$log" ] && mv -f "$log" "$log.old"

dbus-run-session niri --session >"$log" 2>&1

less +G "$log"
