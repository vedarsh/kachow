#include "usrl_ring.h"
#include "usrl_core.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    void *core = usrl_core_map("/usrl_core", 1024 * 1024);
    if (!core) {
        printf("Failed to map core. Run ./init_core first!\n");
        return 1;
    }

    UsrlSubscriber sub;
    memset(&sub, 0, sizeof(sub));
    usrl_sub_init(&sub, core, "demo");

    printf("[SUB] Connected. Starting at seq=%lu\n", sub.last_seq);

    uint8_t buf[512];
    while (1) {
        int n = usrl_sub_next(&sub, buf, sizeof(buf));

        if (n > 0) {
            buf[n] = 0;
            printf("[SUB] MESSAGE seq=%lu -> %s\n", sub.last_seq, buf);
            fflush(stdout); // Force print immediately
            continue;
        }

        if (n == 0) {
            usleep(1000); // Sleep 1ms to reduce CPU usage
            continue;
        }
    }
    return 0;
}
