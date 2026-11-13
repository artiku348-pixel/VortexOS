/* guess: a number guessing game (keyboard input demo). */
#include <vortex.h>

int main(void)
{
    char line[16];
    srand(vx_ticks());
    int secret = rand() % 100 + 1;

    printf("I picked a number from 1 to 100. Empty line to give up.\n");
    for (int tries = 1; ; tries++) {
        printf("Your guess: ");
        gets_s(line, sizeof(line));
        if (!line[0]) { printf("It was %d.\n", secret); return 0; }
        int n = atoi(line);
        if (n == secret) { printf("Correct! Attempts: %d\n", tries); return 0; }
        printf(n < secret ? "Higher.\n" : "Lower.\n");
    }
}
