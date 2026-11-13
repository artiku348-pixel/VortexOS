/* hello: the smallest console program. Usage: hello [name...] */
#include <vortex.h>

static const char *const tips[] = {
    "run 'desktop' to start the GUI",
    "scripts in /bin are plain text: try 'cat /bin/greet'",
    "add '&' to run a program in the background",
};

int main(int argc, char **argv)
{
    char user[32];

    vx_color(VX_LIGHT_CYAN, VX_BLACK);
    printf("Hello from a VortexOS application!\n");
    vx_color(VX_LIGHT_GREY, VX_BLACK);

    if (argc > 1) {
        printf("Arguments:");
        for (int i = 1; i < argc; i++) printf(" [%s]", argv[i]);
        printf("\n");
    } else if (vx_getenv("USER", user, sizeof(user)) >= 0) {
        printf("Nice to meet you, %s.\n", user);
    }
    printf("Tip: %s.\n", tips[vx_ticks() % 3]);
    return 0;
}
