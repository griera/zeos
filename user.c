#include <libc.h>

char buff[24];
int pid;

int __attribute__ ((__section__(".text.main"))) main(void)
{
    /* Run a test suite provided by lab course */
    runjp_rank(5, 9);

    while (1);

    return 0;
}

