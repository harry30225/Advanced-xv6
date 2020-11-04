#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct
{
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void pinit(void)
{
  initlock(&ptable.lock, "ptable");
#if SCHEDULER == SCHED_MLFQ  
  acquire(&ptable.lock);
  for (int i = 0; i < NPROC; i++) 
  {
    surplus_node[i].use = 0;
  }

  for(int i=0; i<NQUEUE; i++)
  {
    queues[i] = 0;
  }
  release(&ptable.lock);
#endif  
}

// Must be called with interrupts disabled
int cpuid()
{
  return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *
mycpu(void)
{
  int apicid, i;

  if (readeflags() & FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i)
  {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc *
myproc(void)
{
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->ctime = ticks;       // set creation time to current time
  p->rtime = 0;           // set run time to zero
  p->etime = 0;           // set end time to zero
  p->priority = 60;       // set the priority of processs to 60
  p->timeslices = 0;      // set the timeslices to zero

  p->agetime = ticks;     
  p->current_timeslices = 0;
  p->queue = 0;
  p->punish = 0;
  p->num_run = 0;

  for(int i=0;i<NQUEUE;i++)
  {
    p->ticks[i] = 0;
  }

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0)
  {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint *)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

// This function loops through process table and increments the rtime if the process is running

void inc_rtime()
{
  acquire(&ptable.lock);

  for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == RUNNING)
    {
      p->rtime++;
    }
  }

  release(&ptable.lock);
}
void punisher() 
{
    acquire(&ptable.lock);
    myproc()->punish = 1;
    release(&ptable.lock);
}

void inc_timeslice() 
{
    acquire(&ptable.lock);
    myproc()->current_timeslices++;
    release(&ptable.lock);
}

void inc_ticks()
{
    acquire(&ptable.lock);
    myproc()->ticks[myproc()->queue]++;
    release(&ptable.lock);
}
//PAGEBREAK: 32
// Set up first user process.
void userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if ((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0; // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
#if SCHEDULER == SCHED_MLFQ  
  queues[0] = push(queues[0],p);
#endif
  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if (n > 0)
  {
    if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  else if (n < 0)
  {
    if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy process state from proc.
  if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0)
  {
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
#if SCHEDULER == SCHED_MLFQ  
  queues[0] = push(queues[0],np);
#endif
  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");
  curproc->etime = ticks;

  // Close all open files.
  for (fd = 0; fd < NOFILE; fd++)
  {
    if (curproc->ofile[fd])
    {
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->parent == curproc)
    {
      p->parent = initproc;
      if (p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE)
      {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); //DOC: wait-sleep
  }
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// Also tell about wait time and runtime of a process
int waitx(int *wtime, int *rtime)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE)
      {
        // Found one.
        *rtime = p->rtime;
        *wtime = p->etime - p->rtime - p->ctime;
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); //DOC: wait-sleep
  }
}

int set_priority(int new_priority, int pid)
{
  struct proc *p;
  if(new_priority < 0 || new_priority > 100)
  {
    return -1;
  }
  acquire(&ptable.lock);
  int old = 60;
  int flag_found = 0;
  for(p = ptable.proc ; p < &ptable.proc[NPROC] ; p++)
  {
      if(p->pid == pid)
      {
        old = p->priority;
        p->priority = new_priority;
        if(old != new_priority)
        {
          p->timeslices = 0;
        }
        flag_found = 1;
        break;
      }
  }
  release(&ptable.lock);
  if(flag_found == 1)
  {
    if(new_priority != old)
    {
      yield();
    }
    return old;
  }
  return -1;
}

void get_ps()
{
  struct proc *p;
  acquire(&ptable.lock);
  cprintf("PID  Priority  State    r_time  w_time  n_run  q_curr    q0    q1    q2    q3    q4\n");
  for(p = ptable.proc ; p < &ptable.proc[NPROC]; p++)
  {
    
     // cprintf("hi\n");
    if(p->state != UNUSED)
    {
    int wait_time = p->etime - p->ctime - p->rtime;
#if SCHEDULER == SCHED_MLFQ
    p->current_queue = p->queue;
#else
    p->current_queue = -1;
    for(int i=0;i<NQUEUE;i++)
    {
      p->ticks[i] = -1;
    }      
#endif
    char state[10] = "";   
    if(p->state == EMBRYO)
    {
      strncpy(state,"embryo",6);
    }
    else if(p->state == SLEEPING)
    {
      strncpy(state,"sleeping",8);
    }
    else if(p->state == RUNNABLE)
    {
      strncpy(state,"runnable",8);
    }  
    else if(p->state == RUNNING)
    {
      strncpy(state,"running",7);
    }
    else if(p->state == ZOMBIE)
    {
      strncpy(state,"zombie",6);
    }
    cprintf("%d     %d      %s     %d      %d     %d     %d     %d     %d     %d     %d     %d\n",p->pid,p->priority,state,p->rtime,wait_time,p->num_run,p->current_queue,p->ticks[0],p->ticks[1],p->ticks[2],p->ticks[3],p->ticks[4]);
    }  
  }

  release(&ptable.lock);
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.

void scheduler(void)
{

#if SCHEDULER == SCHED_RR
  sched_rr();

#elif SCHEDULER == SCHED_FCFS
  sched_fcfs();

#elif SCHEDULER == SCHED_PBS
  sched_pbs();

#elif SCHEDULER == SCHED_MLFQ
  sched_mlfq();

#endif
}

// round robin scheduler
void sched_rr(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  for (;;)
  {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state != RUNNABLE)
      {
        continue;
      }

      #ifdef DEBUG
            cprintf("On core: %d, scheduling %d %s\n", c->apicid, p->pid, p->name);
      #endif

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      p->num_run++;
    }
    release(&ptable.lock);
  }
}

// first come first serve scheduler
void sched_fcfs(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  for (;;)
  {
    // Enable interrupts on this processor - yielding disabled for FCFS
    sti();
    // We will loop over the entire ptable to find the proceess with leat creation time as FCFS is First Come First Serve.
    int min_time = ticks + 100;
    struct proc *selected_proc = 0;

    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      //cprintf("Entered");
      if (p->state != RUNNABLE)
      {
        continue;
      }

      if (p->ctime < min_time)
      {
        min_time = p->ctime;
        selected_proc = p;
      }
    }
    if (selected_proc == 0)
    {
      release(&ptable.lock);
      continue;
    }
    #ifdef DEBUG
        cprintf("On core: %d, scheduling %d %s\n", c->apicid, selected_proc->pid, selected_proc->name);
    #endif

    c->proc = selected_proc;
    selected_proc->num_run++;
    switchuvm(selected_proc);
    selected_proc->state = RUNNING;
    swtch(&(c->scheduler), selected_proc->context);
    switchkvm();

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;

    release(&ptable.lock);
  }

}

// priority based scheduler
void sched_pbs(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  for(;;)
  {
    // Enable interrupts on this processor
    sti();
    // loop over entire ptable for the process with high priority (less int priority) and minimum timeslices
    
    int mintimeslices = ticks + 100;
    int maxpriority = 101;  // As the priority cant be greater than 100
    struct proc *selected_proc = 0;
    
    acquire(&ptable.lock);

    for(p = ptable.proc; p < &ptable.proc[NPROC] ; p++)
    {
      if(p->state != RUNNABLE)
      {
        continue;
      }

      if(p->priority < maxpriority)
      {
        maxpriority = p->priority;
        mintimeslices = p->timeslices;
        selected_proc = p;
      }
      else if(p->priority == maxpriority && p->timeslices < mintimeslices)
      {
        mintimeslices = p->timeslices;
        selected_proc = p;
      }
    }
    if(selected_proc == 0)
    {
      release(&ptable.lock);
      continue;
    }
    #ifdef DEBUG
        cprintf("PBS: On core: %d, scheduling %d %s\n", c->apicid, selected_proc->pid, selected_proc->name);
    #endif
    selected_proc->timeslices++;
    selected_proc->num_run++;
    // Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.
    c->proc = selected_proc;
    switchuvm(selected_proc);
    selected_proc->state = RUNNING;

    swtch(&(c->scheduler), selected_proc->context);
    switchkvm();

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;

    release(&ptable.lock);
  }

}

//multi level feedback queueing
void sched_mlfq(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  for(;;)
  {
    // Enable interrupts for the process
    sti();
    acquire(&ptable.lock);
    // age of process
    for(int i=1;i<NQUEUE;i++)
    {
      
        #ifdef DEBUG
              int temp = split(&queues[i],&queues[i-1],AGELIMIT);
              if (temp > 0) {
                cprintf("Aged %d processes from %d to %d\n", temp, i, i - 1);
              }
        #else
              split(&queues[i],&queues[i-1],AGELIMIT);      
        #endif
    }
    
    // Loop through all the queues to find the appropriate process to run
    p = 0;
    for(int i=0; i<NQUEUE; i++)
    {
      if(length(queues[i]) == 0)
      {
        continue;
      }
      p = queues[i]->p;
      queues[i] = pop(queues[i]);
      break;
    } 
  
    if(p == 0 || p->state != RUNNABLE)
    {
      release(&ptable.lock);
      continue;
    } 
    p->current_timeslices++;
    p->num_run++;

    #ifdef DEBUG
        cprintf("MLFQ: On core %d scheduling %d %s from queue %d with %d timeslices and %d age time %d\n", c->apicid, p->pid, p->name, p->queue, p->current_timeslices, p->agetime,ticks);
    #endif
    // Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.
    c->proc = p;
    switchuvm(p);
    p->state = RUNNING;

    swtch(&(c->scheduler), p->context);
    switchkvm();

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;

    if(p != 0 && p-> state == RUNNABLE)
    {
      if( p->punish == 0 )
      {
        p->current_timeslices = 0;
        p->agetime = ticks;
        queues[p->queue] = push(queues[p->queue] , p);
      }
      else
      {
        p->current_timeslices = 0;
        p->agetime = ticks;
        p->punish = 0;

        if(p->queue != NQUEUE - 1)
        {
          p->queue++;
        } 
        queues[p->queue] = push(queues[p->queue] , p);
      }
      
    }

    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (readeflags() & FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  acquire(&ptable.lock); //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first)
  {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock)
  {                        //DOC: sleeplock0
    acquire(&ptable.lock); //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock)
  { //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == SLEEPING && p->chan == chan)
    {
      p->state = RUNNABLE;
#if SCHEDULER == SCHED_MLFQ      
      queues[p->queue] = push(queues[p->queue] , p);
      p->current_timeslices = 0;
      p->agetime = ticks;
#endif      
    }  
  }    
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
      {
        p->state = RUNNABLE;
#if SCHEDULER == SCHED_MLFQ
        queues[p->queue] = push(queues[p->queue], p);
        p->agetime = ticks;
        p->current_timeslices = 0;
#endif
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [EMBRYO] "embryo",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
#if SCHEDULER == SCHED_MLFQ      
    cprintf("%d %s %s QUEUE: %d TIMESLICES %d", p->pid, state, p->name, p->queue, p->timeslices);
#else
    cprintf("%d %s %s ", p->pid, state,p->name);  
#endif    
    if (p->state == SLEEPING)
    {
      getcallerpcs((uint *)p->context->ebp + 2, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
