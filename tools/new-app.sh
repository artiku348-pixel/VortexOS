#!/bin/sh
# Create a new VortexOS application from a template.
#   tools/new-app.sh NAME        console program -> /bin/NAME
#   tools/new-app.sh NAME gui    window program  -> /apps/NAME (shown in the launcher)
set -e
cd "$(dirname "$0")/.."

name="$1"
kind="${2:-bin}"
case "$name" in
    ""|*[!a-z0-9_]*) echo "usage: $0 NAME [gui]   (NAME: a-z, 0-9, _)" >&2; exit 1 ;;
esac
case "$kind" in bin|gui) ;; *) echo "kind must be 'gui' or omitted" >&2; exit 1 ;; esac

file="apps/$kind/$name.c"
[ -e "$file" ] && { echo "$file already exists" >&2; exit 1; }
mkdir -p "apps/$kind"

if [ "$kind" = gui ]; then
cat > "$file" <<TEMPLATE
/* $name: a VortexOS window application. */
#include <vortex.h>

#define W 320
#define H 200

int main(void)
{
    struct vx_event ev;
    int win = vx_win_create("$name", W, H);
    if (win < 0) {
        printf("$name: needs the desktop (run 'desktop' first)\n");
        return 1;
    }
    vx_win_fill(win, 0, 0, W, H, VX_RGB(0xEC, 0xEF, 0xF1));
    vx_win_text(win, 16, 16, "Hello from $name! Click or press Esc.", VX_RGB(0x1B, 0x25, 0x33));

    for (;;) {
        vx_win_event(win, &ev, -1);                 /* wait for input */
        if (ev.type == VX_EV_KEY && ev.key == 27) break;
        if (ev.type == VX_EV_MOUSE_DOWN)
            vx_win_fill(win, ev.x - 4, ev.y - 4, 8, 8, VX_RGB(0x3D, 0x8B, 0xFD));
    }
    return 0;
}
TEMPLATE
else
cat > "$file" <<TEMPLATE
/* $name: a VortexOS console program. */
#include <vortex.h>

int main(int argc, char **argv)
{
    printf("Hello from $name!\n");
    for (int i = 1; i < argc; i++) printf("  argument %d: %s\n", i, argv[i]);
    return 0;
}
TEMPLATE
fi

echo "Created $file"
echo "Build and boot with: make run   (then type '$name')"
