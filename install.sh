#!/bin/sh
# Link the live configs to this repository, so the repo is the only copy.
# Safe to re-run: correct links are left alone, anything else in the way is
# moved to <path>.bak-<date> first.

set -eu

repo=$(cd "$(dirname "$0")" && pwd)
cfg="${XDG_CONFIG_HOME:-$HOME/.config}"
stamp=$(date +%Y%m%d-%H%M%S)

link() {
    src="$repo/$1"
    dst="$2"

    if [ ! -e "$src" ]; then
        echo "missing in repo: $1" >&2
        return 1
    fi
    if [ -L "$dst" ] && [ "$(readlink "$dst")" = "$src" ]; then
        return 0
    fi

    mkdir -p "$(dirname "$dst")"
    if [ -e "$dst" ] || [ -L "$dst" ]; then
        mv "$dst" "$dst.bak-$stamp"
        echo "backed up $dst -> $dst.bak-$stamp"
    fi
    ln -s "$src" "$dst"
    echo "linked $dst"
}

niri=desktop/compositor/niri
for f in config layout windowrules keybindings layerrules animations cursor; do
    link "$niri/config/$f.kdl" "$cfg/niri/$f.kdl"
done
link "$niri/niri-sidebar/config.toml"        "$cfg/niri-sidebar/config.toml"
link "$niri/waybar-niri/config.jsonc"        "$cfg/waybar/config.jsonc"
link "$niri/waybar-niri/style.css"           "$cfg/waybar/style.css"
link "$niri/portals/niri-portals.conf"       "$cfg/xdg-desktop-portal/niri-portals.conf"
link "$niri/scripts/niri-session"            "$HOME/.local/bin/niri-session"
link "$niri/start_niri.sh"                   "$HOME/start_niri.sh"

link desktop/config/mako/config              "$cfg/mako/config"
link desktop/config/rofi/config.rasi         "$cfg/rofi/config.rasi"
link desktop/config/rofi/themes/midve.rasi   "$cfg/rofi/themes/midve.rasi"
link desktop/config/swaylock/config          "$cfg/swaylock/config"
link desktop/config/gtk/gtk-3.0/settings.ini "$cfg/gtk-3.0/settings.ini"
link desktop/config/gtk/gtk-4.0/settings.ini "$cfg/gtk-4.0/settings.ini"
link desktop/config/gtk/gtk-4.0/gtk.css      "$cfg/gtk-4.0/gtk.css"
link desktop/config/gtk/gtk-3.0/gtk.css      "$cfg/gtk-3.0/gtk.css"
link desktop/config/gtk/plain-widgets.css    "$cfg/gtk-3.0/plain-widgets.css"
link desktop/config/gtk/plain-widgets.css    "$cfg/gtk-4.0/plain-widgets.css"
link kitty/kitty.conf                        "$cfg/kitty/kitty.conf"
link shells/zsh/.zprofile                    "$HOME/.zprofile"

# Icon theme: Papirus-Dark with violet folders (built in ~/.local/share/icons).
python3 -I "$repo/desktop/config/gtk/papirus-plum.py"

# Cursor: the midve theme, Bibata Modern recoloured by the script (~/.local/share/icons/midve).
python3 -I "$repo/desktop/config/cursor/midve-cursor.py"

# UI font: Onest (OFL, Google Fonts), installed per user.
if ! fc-list : family | grep -qx 'Onest'; then
    mkdir -p "$HOME/.local/share/fonts/Onest"
    curl -fsSL -o "$HOME/.local/share/fonts/Onest/Onest-Variable.ttf" \
        "https://github.com/google/fonts/raw/main/ofl/onest/Onest%5Bwght%5D.ttf" &&
    fc-cache -f "$HOME/.local/share/fonts" >/dev/null && echo "installed font Onest"
fi
