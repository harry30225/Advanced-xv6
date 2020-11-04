#include "types.h"
#include "fcntl.h"
#include "stat.h"
#include "user.h"

int main(int argc, char** argv) {
    if(argc < 2)
    {
        printf(2,"time Fatal error : Incomplete Command Line\n");
        exit();
    }
    int pid;
    pid = fork();
    if(pid < 0)
    {
        printf(2,"time Fatal error : Unable To Run Command\n");
        exit();
    }
    else if(pid == 0)
    {
      printf(1,"Timmings : %s\n",argv[1]);
      if(exec(argv[1], argv + 1) < 0)
      {
        printf(2,"time Fatal error : Unable To Run Command\n");
        exit();
      }
    }
    else
    {
        int wtime, rtime;
        int wait_id = waitx(&wtime, &rtime);
        printf(1, "Time report for %s\nProcess ID: %d\nWaiting time: %d\nRunning time: %d\n", argv[1], wait_id, wtime, rtime);
        exit();
    }
    
} 
