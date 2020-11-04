#include "types.h"
#include "fcntl.h"
#include "stat.h"
#include "user.h"

int main(int argc, char **argv)
{
    if(argc != 1)
    {
        printf(2,"Fatal Error : Inappropraite Command line\n");
        exit();
    }
    get_ps();
    exit();
}