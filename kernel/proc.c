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
struct proc* initproc;
int nextpid = 1;

struct namespace namespaces[NNAMESPACES];
struct namespace* initnamespace;
int nextnamespace = 1;

struct spinlock pid_lock;
struct spinlock namespace_lock;

extern void forkret(void);
static void freeproc(struct proc* p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock
        wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++) {
        char* pa = kalloc();
        if (pa == 0)
            panic("kalloc");
        uint64 va = KSTACK((int) (p - proc));
        kvmmap(kpgtbl, va, (uint64) pa, PGSIZE, PTE_R | PTE_W);
    }
}


// initialize the namespaces table
void
namespaceinit(void) {
    initlock(&namespace_lock, "nextlocks");
    initlock(&wait_lock, "wait_lock");
    struct namespace* ns;
    for (ns = namespaces; ns < &namespaces[NNAMESPACES]; ns++) {
        initlock(&ns->lock, "namespace");
        ns->parent = 0;
        ns->head = 0;
        ns->namespace_id = 0;
        ns->level = 0;
        ns->used = 0;
        ns->next_namespace_pid = 1;
        ns->proc_cnt = 0;
    }
}


// initialize the proc table.
void
procinit(void) {
    struct proc* p;

    initlock(&pid_lock, "nextpid");
    initlock(&wait_lock, "wait_lock");
    for (p = proc; p < &proc[NPROC]; p++) {
        initlock(&p->lock, "proc");
        p->state = UNUSED;
        p->kstack = KSTACK((int) (p - proc));
        p->init_ticks = 0;
        p->run_time = 0;
        p->last_run_start = 0;
        p->context_switches = 0;
        p->ns = 0;
        for (int i = 0; i < MAXLEVEL; ++i) {
            p->pids[i] = 0;
        }
    }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid() {
    int id = r_tp();
    return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
    int id = cpuid();
    struct cpu* c = &cpus[id];
    return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
    push_off();
    struct cpu* c = mycpu();
    struct proc* p = c->proc;
    pop_off();
    return p;
}

int
allocnamespaceid() {
    int namespace;

    acquire(&namespace_lock);
    namespace = nextnamespace;
    nextnamespace++;

    release(&namespace_lock);
    return namespace;
}

int
allocpid() {
    int pid;

    acquire(&pid_lock);
    pid = nextpid;
    nextpid = nextpid + 1;
    release(&pid_lock);

    return pid;
}

// look for unused namespace, initialize it, return without locks held
// if all namespaces are used, return 0
struct namespace*
allocnamespace(void) {
    struct namespace* ns;
    for (ns = namespaces; ns < &namespaces[NNAMESPACES]; ns++) {
        acquire(&ns->lock);
        if (ns->used == 0) {
            ns->namespace_id = allocnamespaceid();
            ns->head = 0;
            ns->parent = 0;
            ns->used = 1;
            ns->proc_cnt = 0;
            ns->level = 0;
            ns->next_namespace_pid = 1;
            release(&ns->lock);
            return ns;
        } else {
            release(&ns->lock);
        }
    }
    return 0;
}


// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(struct namespace* ns) {
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->state == UNUSED) {
            goto found;
        } else {
            release(&p->lock);
        }
    }
    return 0;

    found:
    p->pid = allocpid();
    p->state = USED;

    // pids in namespaces
    p->ns = ns;
    struct namespace* curr_ns = ns;
    for (int i = ns->level; i >= 0; --i) {

        acquire(&curr_ns->lock);

        p->pids[i] = curr_ns->next_namespace_pid;
        curr_ns->next_namespace_pid++;
        curr_ns->proc_cnt++;

        release(&curr_ns->lock);

        curr_ns = curr_ns->parent;

    }

    // Allocate a trapframe page.
    if ((p->trapframe = (struct trapframe*) kalloc()) == 0) {
        freeproc(p);
        release(&p->lock);
        return 0;
    }

    // An empty user page table.
    p->pagetable = proc_pagetable(p);
    if (p->pagetable == 0) {
        freeproc(p);
        release(&p->lock);
        return 0;
    }

    // Set up new context to start executing at forkret,
    // which returns to user space.
    memset(&p->context, 0, sizeof(p->context));
    p->context.ra = (uint64) forkret;
    p->context.sp = p->kstack + PGSIZE;
    p->context_switches += 1;

    return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc* p) {
    if (p->trapframe)
        kfree((void*) p->trapframe);
    p->trapframe = 0;
    if (p->pagetable)
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
    // namespace fields
    p->ns = 0;
    for (int i = 0; i < MAXLEVEL; ++i) {
        p->pids[i] = 0;
    }
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc* p) {
    pagetable_t pagetable;

    // An empty page table.
    pagetable = uvmcreate();
    if (pagetable == 0)
        return 0;

    // map the trampoline code (for system call return)
    // at the highest user virtual address.
    // only the supervisor uses it, on the way
    // to/from user space, so not PTE_U.
    if (mappages(pagetable, TRAMPOLINE, PGSIZE,
                 (uint64) trampoline, PTE_R | PTE_X) < 0) {
        uvmfree(pagetable, 0);
        return 0;
    }

    // map the trapframe page just below the trampoline page, for
    // trampoline.S.
    if (mappages(pagetable, TRAPFRAME, PGSIZE,
                 (uint64)(p->trapframe), PTE_R | PTE_W) < 0) {
        uvmunmap(pagetable, TRAMPOLINE, 1, 0);
        uvmfree(pagetable, 0);
        return 0;
    }

    return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz) {
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
userinit(void) {
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

    // add head to the  initnamespace, set proc_cnt to 1
    acquire(&ns->lock);
    ns->head = initproc;
    ns->proc_cnt = 1;
    release(&ns->lock);

    // add namespace for initproc
    p->ns = initnamespace;

    release(&p->lock);

}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n) {
    uint64 sz;
    struct proc* p = myproc();

    sz = p->sz;
    if (n > 0) {
        if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
            return -1;
        }
    } else if (n < 0) {
        sz = uvmdealloc(p->pagetable, sz, sz + n);
    }
    p->sz = sz;
    return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void) {
    int i, pid;
    struct proc* np;
    struct proc* p = myproc();

    // Allocate process.
    if ((np = allocproc(p->ns)) == 0) {
        return -1;
    }

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

    pid = np->pids[p->ns->level];    // pid in the lowest namespace (return value)

    release(&np->lock);

    acquire(&wait_lock);
    np->parent = p;
    release(&wait_lock);

    acquire(&np->lock);
    np->state = RUNNABLE;
    np->init_ticks = sys_uptime();
    np->run_time = 0;
    np->last_run_start = 0;
    np->context_switches = 0;
    release(&np->lock);

    return pid;
}


int
clone(void) {
    int i, pid;
    struct proc* np;
    struct proc* p = myproc();


    // Allocate namespace
    struct namespace* ns = allocnamespace();
    if (ns == 0) { return -1; }
    ns->parent = p->ns;
    if (p->ns->level + 1 >= MAXLEVEL) { return -1; }
    ns->level = p->ns->level + 1;

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

    pid = np->pids[p->ns->level];    // pid in the lowest namespace (return value)

    release(&np->lock);

    acquire(&wait_lock);
    np->parent = p;
    release(&wait_lock);

    acquire(&np->lock);
    np->state = RUNNABLE;
    np->init_ticks = sys_uptime();
    np->run_time = 0;
    np->last_run_start = 0;
    np->context_switches = 0;
    release(&np->lock);

    return pid;
}


// Check if ns1 is ns2 or one of it child namespaces
int
ischildnamespaceof(struct namespace* ns1, struct namespace* ns2) {
    struct namespace* ns = ns1;
    // comparing two namespaces and going to parent of ns
    while (ns != 0) {
        if (ns == ns2) { return 1; }
        ns = ns->parent;
    }
    return 0;
}

// get the head of the current namespace, if it doesn't exist - first existent head in parent namespaces
struct proc*
getnamespacehead(struct namespace* ns) {

    // printf("--> getting namespace head for ns %d\n", ns->namespace_id);

    struct namespace* curr_ns = ns;
    struct proc* p = ns->head;
    while (curr_ns->level > 0 && p == 0) {
        curr_ns = curr_ns->parent;
        p = curr_ns->head;
    }
    return p;
}

// Pass p's abandoned children to the head of its namespace.
// Caller must hold wait_lock.
void
reparent(struct proc* p) {
    struct proc* pp;
    
    // printf("--> reparenting %d (%d)\n", p->pid, p->pids[p->ns->level]);
    
    for (pp = proc; pp < &proc[NPROC]; pp++) {
        if (pp->parent == p) {
            // printf("--> that's my parent, i'm %d(%d)\n", pp->pid, pp->pids[p->ns->level]); 
            struct proc* new_parent = getnamespacehead(pp->ns);
            // printf("--> new parent will be %d(%d)\n", new_parent->pid, new_parent->pids[p->ns->level]);
            pp->parent = new_parent;
            wakeup(new_parent);
        }
    }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status) {
    struct proc* p = myproc();

    if (p == initproc)
        panic("init exiting");

    // Close all open files.
    for (int fd = 0; fd < NOFILE; fd++) {
        if (p->ofile[fd]) {
            struct file* f = p->ofile[fd];
            fileclose(f);
            p->ofile[fd] = 0;
        }
    }

    begin_op();
    iput(p->cwd);
    end_op();
    p->cwd = 0;

    acquire(&wait_lock);

    // Reparent all children (with the namespace logic)
    reparent(p);

    // Parent might be sleeping in wait().
    wakeup(p->parent);

    acquire(&p->lock);

    p->xstate = status;

    if (p->state == RUNNING) {
        p->run_time += sys_uptime() - p->last_run_start;
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
wait(uint64 addr) {
    struct proc* pp;
    int havekids, pid;
    struct proc* p = myproc();

    acquire(&wait_lock);

    for (;;) {
        // Scan through table looking for exited children.
        havekids = 0;
        for (pp = proc; pp < &proc[NPROC]; pp++) {
            if (pp->parent == p) {
                // make sure the child isn't still in exit() or swtch().
                acquire(&pp->lock);

                havekids = 1;
                if (pp->state == ZOMBIE) {
                    // Found one.
                    pid = pp->pid;
                    if (addr != 0 && copyout(p->pagetable, addr, (char*) &pp->xstate,
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
        if (!havekids || killed(p)) {
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
scheduler(void) {
    struct proc* p;
    struct cpu* c = mycpu();

    c->proc = 0;
    for (;;) {
        // Avoid deadlock by ensuring that devices can interrupt.
        intr_on();

        for (p = proc; p < &proc[NPROC]; p++) {
            acquire(&p->lock);
            if (p->state == RUNNABLE) {
                // Switch to chosen process.  It is the process's job
                // to release its lock and then reacquire it
                // before jumping back to us.
                p->state = RUNNING;

                p->last_run_start = sys_uptime();

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
sched(void) {
    int intena;
    struct proc* p = myproc();

    if (!holding(&p->lock))
        panic("sched p->lock");
    if (mycpu()->noff != 1)
        panic("sched locks");
    if (p->state == RUNNING)
        panic("sched running");
    if (intr_get())
        panic("sched interruptible");

    intena = mycpu()->intena;
    swtch(&p->context, &mycpu()->context);
    mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void) {
    struct proc* p = myproc();
    acquire(&p->lock);
    p->state = RUNNABLE;
    sched();
    release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void) {
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
sleep(void* chan, struct spinlock* lk) {
    struct proc* p = myproc();

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

    if (p->state == RUNNING) {
        p->run_time += sys_uptime() - p->last_run_start;
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
wakeup(void* chan) {
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++) {
        if (p != myproc()) {
            acquire(&p->lock);
            if (p->state == SLEEPING && p->chan == chan) {
                p->state = RUNNABLE;
            }
            release(&p->lock);
        }
    }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid) {
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->pid == pid) {
            p->killed = 1;
            if (p->state == SLEEPING) {
                // Wake process from sleep().
                p->state = RUNNABLE;
            }
            release(&p->lock);
            return 0;
        }
        release(&p->lock);
    }
    return -1;
}

void
setkilled(struct proc* p) {
    acquire(&p->lock);
    p->killed = 1;
    release(&p->lock);
}

int
killed(struct proc* p) {
    int k;

    acquire(&p->lock);
    k = p->killed;
    release(&p->lock);
    return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void* src, uint64 len) {
    struct proc* p = myproc();
    if (user_dst) {
        return copyout(p->pagetable, dst, src, len);
    } else {
        memmove((char*) dst, src, len);
        return 0;
    }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void* dst, int user_src, uint64 src, uint64 len) {
    struct proc* p = myproc();
    if (user_src) {
        return copyin(p->pagetable, dst, src, len);
    } else {
        memmove(dst, (char*) src, len);
        return 0;
    }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void) {
    static char* states[] = {
            [UNUSED]    "unused",
            [USED]      "used",
            [SLEEPING]  "sleep ",
            [RUNNABLE]  "runble",
            [RUNNING]   "run   ",
            [ZOMBIE]    "zombie"
    };
    struct proc* p;
    char* state;

    printf("\n");
    for (p = proc; p < &proc[NPROC]; p++) {
        if (p->state == UNUSED)
            continue;
        if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
            state = states[p->state];
        else
            state = "???";
        printf("%d %s %s", p->pid, state, p->name);
        printf("\n");
    }
}

// returns process pid in lowest namespace
int get_pid_by_proc(struct proc* p) {
    return p->pids[p->ns->level];
}

// returns parent pid in child's lowest namespace
int get_parent_pid_by_proc(struct proc* p) {
    if (p->ns == p->parent->ns) {
        return p->parent->pids[p->ns->level];
    } else {
        return 0;
    }
}


// handles ps_list & ps_list_global syscalls
// return pids of all processes in caller namespace
// if global, returns global pids
// otherewise returns pids in this namespace
int
handle_ps(int limit, uint64 pids, int global) {

    int proc_cnt = 0;
    for (struct proc* p = proc; p < &proc[NPROC]; p++) {
        if (p->state != UNUSED && ischildnamespaceof(p->ns, myproc()->ns))
            proc_cnt++;
    }

    // ps pids
    if (limit != -1) {

        if (limit > proc_cnt) {
            limit = proc_cnt;
        }

        int ind = 0;
        for (struct proc* p = proc; p < &proc[NPROC]; p++) {

            if (p->state != UNUSED && ischildnamespaceof(p->ns, myproc()->ns) && ind < limit) {

                acquire(&p->lock);

                // we already checked that p is in child namespace of myproc
                // so p's pid in myproc-namespace is in p->pids
                // at the index = level of myproc-namespace
                int my_pid = p->pids[myproc()->ns->level];
                if (global == 1) {
                    // p's global pid (in namespace 0) is still stored in p->pid
                    my_pid = p->pid;
                }

                int success = copyout(myproc()->pagetable, pids + ind * sizeof(int), (char*) &(my_pid), sizeof(int));
                release(&p->lock);
                if (success != 0)
                    return -1;

                ind++;

            }

        }

    }
        // limit = -1 for ps count
    else {}


    return proc_cnt;

}


// handles ps info syscall
int handle_ps_info(int pid, uint64 psinfo) {

    struct proc* pid_proc = 0;
    for (struct proc* p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->pid == pid) {
            pid_proc = p;
        }
        release(&p->lock);
    }

    acquire(&pid_proc->lock);

    // state
    uint64 ptr = psinfo;
    enum procstate state = pid_proc->state;
    static char* states[] = {
            [UNUSED]    "unused",
            [USED]      "used",
            [SLEEPING]  "sleep ",
            [RUNNABLE]  "runble",
            [RUNNING]   "run   ",
            [ZOMBIE]    "zombie"
    };
    char* state_str = states[state];
    int success = copyout(myproc()->pagetable, ptr, state_str, sizeof(char) * STATE_SIZE);
    if (success != 0) {
        release(&pid_proc->lock);
        return -1;
    }



    // parent_id
    ptr += sizeof(char) * STATE_SIZE;

    acquire(&wait_lock);

    struct proc* parent = pid_proc->parent;
    int parent_pid = 0;
    if (parent != 0) {
        acquire(&parent->lock);
        parent_pid = get_parent_pid_by_proc(pid_proc);
        release(&parent->lock);
    }

    release(&wait_lock);


    success = copyout(myproc()->pagetable, ptr, (char*) &parent_pid, sizeof(int));
    if (success != 0) {
        release(&pid_proc->lock);
        return -1;
    }

    // mem_size
    ptr += sizeof(int);
    int mem_size = pid_proc->sz;

    success = copyout(myproc()->pagetable, ptr, (char*) &mem_size, sizeof(int));
    if (success != 0) {
        release(&pid_proc->lock);
        return -1;
    }

    // files_count
    ptr += sizeof(int);

    int files_count = 0;
    for (int i = 0; i < NOFILE; i++) {
        if (pid_proc->ofile[i]) {
            files_count++;
        }
    }

    success = copyout(myproc()->pagetable, ptr, (char*) &files_count, sizeof(int));
    if (success != 0) {
        release(&pid_proc->lock);
        return -1;
    }


    // proc_name
    ptr += sizeof(int);

    char* proc_name = pid_proc->name;

    success = copyout(myproc()->pagetable, ptr, proc_name, sizeof(char) * NAME_SIZE);
    if (success != 0) {
        release(&pid_proc->lock);
        return -1;
    }


    // proc_ticks
    ptr += sizeof(char) * NAME_SIZE;

    uint proc_ticks = sys_uptime() - pid_proc->init_ticks;

    success = copyout(myproc()->pagetable, ptr, (char*) &proc_ticks, sizeof(uint));
    if (success != 0) {
        release(&pid_proc->lock);
        return -1;
    }


    // run_time
    ptr += sizeof(uint);

    success = copyout(myproc()->pagetable, ptr, (char*) &(pid_proc->run_time), sizeof(uint));
    if (success != 0) {
        release(&pid_proc->lock);
        return -1;
    }

    // context_switches
    ptr += sizeof(uint);

    success = copyout(myproc()->pagetable, ptr, (char*) &(pid_proc->context_switches), sizeof(uint));
    if (success != 0) {
        release(&pid_proc->lock);
        return -1;
    }

    release(&pid_proc->lock);
    return 0;
}














