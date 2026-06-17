/* colors.c -- an ANSI color showcase, in the spirit of the classic Unix
 * "colortest" demos. Compile + run it ON the machine to exercise the text-
 * property / ansi-color pipeline:
 *
 *   (cc "/colors.c" "/colors")     ; on-device tcc, links against musl
 *   (run-file "/colors")           ; output streams into the frame buffer,
 *                                  ; SGR escapes translated to `face` props
 *
 * MyOSv2's ansi-color-apply handles the 16 SGR foreground colors (30-37 normal,
 * 90-97 bright) plus bold(1)/underline(4)/inverse(7) and reset(0). 256-color
 * (38;5;n) and background codes are recognized-and-ignored, so this demo sticks
 * to what actually renders.
 */
#include <stdio.h>

int main(void)
{
    static const char *name[8] = {
        "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"
    };

    printf("\033[1mMyOSv2 ANSI color showcase\033[0m\n");
    printf("(SGR escapes -> face text properties)\n\n");

    /* The 8 standard foreground colors (SGR 30-37), each in its own color. */
    printf("standard (30-37):\n  ");
    for (int i = 0; i < 8; i++) {
        printf("\033[3%dm%-9s\033[0m", i, name[i]);
    }
    printf("\n\n");

    /* The 8 bright foreground colors (SGR 90-97). */
    printf("bright   (90-97):\n  ");
    for (int i = 0; i < 8; i++) {
        printf("\033[9%dm%-9s\033[0m", i, name[i]);
    }
    printf("\n\n");

    /* Bold + color (the busybox-ls directory style is bold blue: 1;34). */
    printf("bold     (1;30-1;37):\n  ");
    for (int i = 0; i < 8; i++) {
        printf("\033[1;3%dm%-9s\033[0m", i, name[i]);
    }
    printf("\n\n");

    /* Attributes on their own. */
    printf("attributes:\n  ");
    printf("\033[1mbold\033[0m  ");
    printf("\033[4munderline\033[0m  ");
    printf("\033[7minverse\033[0m  ");
    printf("\033[1;4;31mall-at-once\033[0m\n");

    return 0;
}
