#include "usrl_ring.h"
#include "usrl_core.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    void *core = usrl_core_map("/usrl_core", 1024 * 1024);
    printf("[PUB] core=%p\n", core);
    if (!core) return 1;

    UsrlPublisher pub;
    memset(&pub, 0, sizeof(pub));
    usrl_pub_init(&pub, core, "demo");

    char msg[100];
    for (int i = 0; i < 100000; i++) {
        sprintf(msg, "hello %d", i);
        int r = usrl_pub_publish(&pub, msg, (uint32_t)strlen(msg) + 1);
        if (r != 0)
            printf("[PUB] publish error=%d\n", r);
        usleep(200000); /* 200 ms for demo */
    }

    return 0;
}
