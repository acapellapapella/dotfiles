# niri

`~/.config/niri/*.kdl` are symlinks to the files here (run `install.sh` from
the repo root), so editing them in the repo applies live: niri reloads on save.

`config.kdl` includes the modules:

```kdl
include "layout.kdl"       // layout, decorations, overview
include "windowrules.kdl"  // per-app rules
include "keybindings.kdl"
include "layerrules.kdl"   // rofi, notifications
include "animations.kdl"
include "cursor.kdl"
```

`config.kdl` itself (outputs, environment, autostart) is machine-specific and
not tracked by git.

The session is started from a TTY with `start_niri.sh`; its log goes to
`~/.cache/niri/niri.log` (the previous session's to `niri.log.old`).
