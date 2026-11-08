# No display manager: logging in on tty1 starts niri. Other TTYs stay plain
# shells (a recovery console if the session ever breaks).
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ "$(tty)" = /dev/tty1 ]; then
    ~/start_niri.sh
fi
