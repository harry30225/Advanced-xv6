#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"

int main(void)
{
     int j;
  for (j = 0; j < 5; j++)
  {
    int pid = fork();
    if (pid < 0)
    {
      printf(1, "Fork failed\n");
      continue;
    }
    if (pid == 0)
    {
      volatile int i;
      volatile int check = 0;
      for (volatile int k = 0; k < 5; k++)
      {
        if (k <= j)
        {
          sleep(200); //io time
        }
        else
        {
          for (i = 0; i < 100000000; i++)
          {
            check += 1; //cpu time
            check *= 2;
            check *= 4;
            check -= 3;
            check += 7;
            check /= 3;
            check *= 8;
            check -= 5;
          }
        }
      }
    //   printf(1, "Process: %d Finished\n", j);
      exit();
    }
    else{
        ;
#if SCHEDULER == SCHED_PBS        
       set_priority(100-(20+j),pid);
#endif        // will only matter for PBS, comment it out if not implemented yet (better priorty for more IO intensive jobs)
    }
  }
  int avg_wait_time = 0,avg_run_time = 0;
  for (j = 0; j < 5+5; j++)
  {
    int wtime , rtime;
    waitx(&wtime , &rtime);
    avg_wait_time += wtime;
    avg_run_time += rtime;
  }
  avg_wait_time = avg_wait_time / 10;
  avg_run_time = avg_run_time / 10;
  printf(2,"Average wait time is %d   and    Average run time is %d\n",avg_wait_time,avg_run_time);
  exit();  
}
