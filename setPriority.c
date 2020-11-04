#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char** argv)
{
    if(argc != 3)
    {
        printf(2, "Incomplete Command line ....\n");
        exit();
    }
    if(atoi(argv[1]) <= 100 && atoi(argv[1]) >= 0)
    {
       int old_priority = set_priority(atoi(argv[1]),atoi(argv[2]));
       if(old_priority != -1)
       {
       printf(2,"\nProcess with id %s : The priority is changed from %d to %s \n",argv[2],old_priority,argv[1]);
       }
       else
       {
        printf(2, "Pid does not exist\n");
       }
    }
    else
    {
        printf(2,"Priority must lie between 0 to 100\n");
    }
    exit();
}