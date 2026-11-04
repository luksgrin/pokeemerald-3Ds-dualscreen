/*
 * The bridge's state machine as a program of its own, for a development
 * machine: exactly what the board runs (main/bridge.c), without the
 * board's Wi-Fi. tests/local_esp32bridge_test.py runs it beside the game's
 * host test; the board itself runs main.c instead.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/bridge.h"

int main(int argc, char **argv)
{
    uint8_t kind = 2; /* the Direct Corner, as the board's main.c starts it */
    bool loopback = false;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--kind") && i + 1 < argc)
            kind = (uint8_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--loopback"))
            loopback = true;
        else
        {
            fprintf(stderr, "usage: %s [--kind N] [--loopback]\n", argv[0]);
            return 2;
        }
    }

    bridge_init(kind, true, loopback);
    if (bridge_bind() != 0)
    {
        fprintf(stderr, "bridge: cannot bind its port\n");
        return 1;
    }
    bridge_run_loop(); /* until the runner stops us */
    return 0;
}
