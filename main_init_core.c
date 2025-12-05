#include "usrl_core.h"
#include <stdio.h>

int main(void)
{
    if (usrl_core_init("/usrl_core", 1024 * 1024) == 0)
        printf("USRL core initialized.\n");
    else
        printf("Init failed.\n");
    return 0;
}
