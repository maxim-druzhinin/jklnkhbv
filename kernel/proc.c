#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#include "process_info.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;
struct spinlock ns_lock;

//namespaces
struct namespace namespaces[NUMNS];
struct namespace* initnamespace;
int nextnamespace = 1;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
      p->init_ticks = 0;
      p->run_time = 0;               
      //p->last_run_start = 0;
      p->context_switches = 0;
      //p->kernel_time = 0;
      //p->user_time = 0;
      p->waiting_time = 0;
      p->last_runnable = 0;

      //namespaces
      p->ns = 0;
      for (int i = 0; i < MAXDEPTH; ++i) {
          p->pids[i] = 0;
      }
  }
}

// initialize the namespaces table.
void
namespaceinit(void) 
{
  initlock(&ns_lock, "nextlocks");
  initlock(&wait_lock, "wait_lock");
  struct namespace* ns;
  for (ns = namespaces; ns < &namespaces[NUMNS]; ns++) {
      initlock(&ns->lock, "namespace");
      ns->parent = 0;
      ns->head = 0;

      ns->ns_id = 0;
      ns->depth = 0;

      ns->used = 0;
      ns->next_ns_pid = 1;
      ns->num_proc = 0;
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

int
allocnamespaceid() 
{
  int namespace;

  acquire(&ns_lock);
  namespace = nextnamespace;
  nextnamespace = nextnamespace + 1;  //  as in allocpid
  release(&ns_lock);

  return namespace;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(struct namespace* ns)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->read_b = 0;
  p->write_b = 0;
  p->heap_pages = 0;
  p->last_run_start = sys_uptime();
  p->is_kernel = 0;
  p->kernel_time = 0;
  p->last_kernel_time = sys_uptime();


  // namespaces
  // assigns process id within the current namespace hierarchy
  p->ns = ns;
  struct namespace* curr_ns = ns;
  for (int i = ns->depth; i >= 0; --i) {
    acquire(&curr_ns->lock);

    p->pids[i] = curr_ns->next_ns_pid;
    curr_ns->next_ns_pid++;
    curr_ns->num_proc++;

    release(&curr_ns->lock);


    // move to the parent namespace to continue assigning pids up the hierarchy
    curr_ns = curr_ns->parent;
  }

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// function searches for an available namespace, initializes it, and returns a pointer.
// if all namespaces are occupied than 0.
struct namespace*
allocnamespace(void) {
    struct namespace* ns;
    // iterate through the global namespaces
    for (ns = namespaces; ns < &namespaces[NUMNS]; ns++) {
        acquire(&ns->lock);

        // if this namespace is currently unused
        if (ns->used == 0) {
            // Initialize the namespace's properties
            ns->ns_id = allocnamespaceid(); // a new unique namespace id
            ns->head = 0;                   
            ns->parent = 0;                 
            ns->used = 1;                  
            ns->num_proc = 0;               
            ns->depth = 0;                  
            ns->next_ns_pid = 1;            

            release(&ns->lock);
            // return the initialized namespace
            return ns;
        } else {
            // if this namespace is already used
            release(&ns->lock);
        }
    }

    // if all in use
    return 0;
}


// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  p->init_ticks = 0;
  p->kernel_time = 0;
  //p->user_time = 0;
  p->waiting_time = 0;
  p->last_runnable = 0;

  p->run_time += sys_uptime() - p->last_run_start;
  if(p->is_kernel){
    p->kernel_time += sys_uptime() - p->last_kernel_time;
  }

  // namespace
  p->ns = 0;
  for (int i = 0; i < MAXDEPTH; ++i) {
    p->pids[i] = 0;
  }
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct namespace* ns;
  ns = allocnamespace();
  initnamespace = ns;

  struct proc* p;
  p = allocproc(initnamespace); 
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  p->init_ticks = sys_uptime();
  p->last_runnable = sys_uptime();


  // namespace
  acquire(&ns->lock);
  ns->head = initproc;
  ns->num_proc = 1;
  release(&ns->lock);

  p->ns = initnamespace;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on copy_res, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc(p->ns)) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pids[p->ns->depth];

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  np->last_runnable = sys_uptime();
  np->init_ticks = sys_uptime();
  np->run_time = 0;
  //np->last_run_start = 0;
  np->context_switches = 0;
  np->kernel_time = 0;
  //np->user_time = 0;
  np->waiting_time = 0;

  release(&np->lock);

  return pid;
}


// our clone (copy-paste from fork + new features)
int
clone(void) {
    int i, pid;
    struct proc* np;
    struct proc* p = myproc();


    // Allocate namespace
    struct namespace* ns = allocnamespace();
    if (ns == 0) { return -1; }
    ns->parent = p->ns;
    if (p->ns->depth + 1 >= MAXDEPTH) { return -1; }
    ns->depth = p->ns->depth + 1;

    // Allocate process.
    if ((np = allocproc(ns)) == 0) {
        return -1;
    }
    
    // Assign np as ns head
    ns->head = np;

    // Copy user memory from parent to child.
    if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
        freeproc(np);
        release(&np->lock);
        return -1;
    }
    np->sz = p->sz;

    // copy saved user registers.
    *(np->trapframe) = *(p->trapframe);

    // Cause fork to return 0 in the child.
    np->trapframe->a0 = 0;

    // increment reference counts on open file descriptors.
    for (i = 0; i < NOFILE; i++)
        if (p->ofile[i])
            np->ofile[i] = filedup(p->ofile[i]);
    np->cwd = idup(p->cwd);

    safestrcpy(np->name, p->name, sizeof(p->name));

    pid = np->pids[p->ns->depth];

    release(&np->lock);

    acquire(&wait_lock);
    np->parent = p;
    release(&wait_lock);

    acquire(&np->lock);
    np->state = RUNNABLE;
    np->last_runnable = sys_uptime();
    np->init_ticks = sys_uptime();
    np->run_time = 0;
    //np->last_run_start = 0;
    np->context_switches = 0;
    np->kernel_time = 0;
    //np->user_time = 0;
    np->waiting_time = 0;
    release(&np->lock);

    return pid;
}

// comparing two namespaces and going to parent of ns
int
compare_ns(struct namespace* ns1, struct namespace* ns2) {
  struct namespace* ns = ns1;
  while (ns != 0) {
      if (ns == ns2) { return 1; }
      ns = ns->parent;
  }
  return 0;
}

struct proc*
get_ns_head(struct namespace* ns) {
    struct namespace* curr_ns = ns;
    struct proc* p = ns->head;
    while (curr_ns->depth > 0 && p == 0) {
        curr_ns = curr_ns->parent;
        p = curr_ns->head;
    }
    return p;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      struct proc* new_parent = get_ns_head(pp->ns);
      pp->parent = new_parent;
      wakeup(new_parent);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);
  

  p->xstate = status;
  p->run_time += sys_uptime() - p->last_run_start;
  
  if(p->is_kernel){
    p->kernel_time += sys_uptime() - p->last_kernel_time;
  }

  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        p->waiting_time += sys_uptime() - p->last_runnable;
        p->last_run_start = sys_uptime();
        if(p->is_kernel){
          p->last_kernel_time = sys_uptime();
        }
        c->proc = p;
        swtch(&c->context, &p->context);
        p->context_switches++;

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");
  
  intena = mycpu()->intena;
  
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  p->run_time += sys_uptime() - p->last_run_start;
  if(p->is_kernel){
    p->kernel_time += sys_uptime() - p->last_kernel_time;
  }
  p->last_runnable = sys_uptime();
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;

  p->run_time += sys_uptime() - p->last_run_start;

  if(p->is_kernel){
    p->kernel_time += sys_uptime() - p->last_kernel_time;
  }
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        p->last_runnable = sys_uptime();
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
        p->last_runnable = sys_uptime();
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on copy_res, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on copy_res, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

// update ps pids and count //
int
ps_list(int limit, uint64 pids, int global) {
 

  int count_ps = 0;
  for (struct proc* p = proc; p < &proc[NPROC]; p++) {
    if (p->state != UNUSED && compare_ns(p->ns, myproc()->ns))
      count_ps++;
  }
    
  if (limit > count_ps)
    limit = count_ps;

  int index = 0;
  for (struct proc* p = proc; p < &proc[NPROC]; p++) {

    if (p->state != UNUSED && compare_ns(p->ns, myproc()->ns) && index < limit) {

        acquire(&p->lock);

        int curr_pid = p->pids[myproc()->ns->depth];
        if (global == 1)
          curr_pid = p->pid;

        int copy_res = copyout(myproc()->pagetable, pids + index * sizeof(int), (char*) &(curr_pid), sizeof(int));
        release(&p->lock);
        if (copy_res != 0)
          return -1;

        index++;
      }
    }
  return count_ps;
}

// update ps info //
int 
ps_info(int pid, uint64 psinfo) {

  struct proc* curr_proc = 0;
  for (struct proc* p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      curr_proc = p;
    }
    release(&p->lock);
  }

  acquire(&curr_proc->lock);

  uint64 user_ptr = psinfo;

  // state //
  enum procstate state = curr_proc->state;
  //state -> str_state
  static char *states[] = {
      [UNUSED]    "unused",
      [USED]      "used",
      [SLEEPING]  "sleep ",
      [RUNNABLE]  "runble",
      [RUNNING]   "run   ",
      [ZOMBIE]    "zombie"
  };
  char* str_state = states[state];
  int copy_res = copyout(myproc()->pagetable, user_ptr, str_state, sizeof(char) * STATE_SIZE);
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }
  //increase user_ptr by size of str_state
  user_ptr += sizeof(char) * STATE_SIZE;



  // parent_id //
  acquire(&wait_lock);//as written in man

  struct proc* parent = curr_proc->parent;
  int parent_pid = 0;
  if (parent != 0) {
    acquire(&parent->lock);//lock parent
    parent_pid = 0;
    if (curr_proc->ns == curr_proc->parent->ns)
        parent_pid =  curr_proc->parent->pids[curr_proc->ns->depth];
    release(&parent->lock);
  }

  release(&wait_lock);


  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &parent_pid, sizeof(int));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }
  //increase user_ptr by size of PPID field as int
  user_ptr += sizeof(int);



  // mem_size //
  int mem_size = curr_proc->sz;

  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &mem_size, sizeof(int));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }
  //increase user_ptr by size of mem field as int
  user_ptr += sizeof(int);



  // files_count //
  int files_count = 0;
  for (int i = 0; i < NOFILE; i++) {
    if (curr_proc->ofile[i]) {
      files_count++;
    }
  }
  if (files_count == -1) {
    release(&curr_proc->lock);
    return -1;
  }

  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &files_count, sizeof(int));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }
  //increase user_ptr by size of mem field as int
  user_ptr += sizeof(int);


  // proc_name //
  char* proc_name = curr_proc->name;

  copy_res = copyout(myproc()->pagetable, user_ptr, proc_name, sizeof(char) * NAME_SIZE);
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }

  // time from start //
  user_ptr += sizeof(char) * NAME_SIZE;

  uint proc_ticks = sys_uptime() - curr_proc->init_ticks;

  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &proc_ticks, sizeof(uint));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }


  // real time //
  user_ptr += sizeof(uint);

  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &(curr_proc->run_time), sizeof(uint));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }

  // context_switches //
  user_ptr += sizeof(uint);

  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &(curr_proc->context_switches), sizeof(uint));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }


  // user time //
  user_ptr += sizeof(uint);

  uint user_time = curr_proc->run_time - curr_proc->kernel_time;

  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &(user_time), sizeof(uint));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }

  // kernel time //
  user_ptr += sizeof(uint);

  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &(curr_proc->kernel_time), sizeof(uint));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }

  // waiting time //
  user_ptr += sizeof(uint);

  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &(curr_proc->waiting_time), sizeof(uint));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }

    // bytes read //
  user_ptr += sizeof(uint);

  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &(curr_proc->read_b), sizeof(uint));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }

  // bytes write //
  user_ptr += sizeof(uint);

  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &(curr_proc->write_b), sizeof(uint));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }

  // pages //
  user_ptr += sizeof(uint);

  copy_res = copyout(myproc()->pagetable, user_ptr, (char*) &(curr_proc->heap_pages), sizeof(uint));
  if (copy_res != 0) {
    release(&curr_proc->lock);
    return -1;
  }

  //release last
  release(&curr_proc->lock);
  return 0;
}