// thread.c - Threads
//

#include "thread.h"

#include <stddef.h>
#include <stdint.h>

#include "lock.h"
#include "halt.h"
#include "console.h"
#include "heap.h"
#include "string.h"
#include "csr.h"
#include "intr.h"
#include "process.h"
#include "memory.h"
#include "trap.h"

// COMPILE-TIME PARAMETERS
//

// NTHR is the maximum number of threads

#ifndef NTHR
#define NTHR 16
#endif

// EXPORTED GLOBAL VARIABLES
//

char thrmgr_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

enum thread_state {
    THREAD_UNINITIALIZED = 0,
    THREAD_STOPPED,
    THREAD_WAITING,
    THREAD_RUNNING,
    THREAD_READY,
    THREAD_EXITED
};

struct thread_context {
    uint64_t s[12];
    void (*ra)(uint64_t);
    void * sp;
};

struct thread {
    struct thread_context context; // must be first member (thrasm.s)
    const char * name;
    void * stack_base;
    size_t stack_size;
    enum thread_state state;
    int id;
    struct process * proc;
    struct thread * parent;
    struct thread * list_next;
    struct condition * wait_cond;
    struct condition child_exit;
};

// INTERNAL GLOBAL VARIABLES
//

#define MAIN_TID 0
#define IDLE_TID (NTHR-1)

struct thread main_thread = {
    .name = "main",
    .id = MAIN_TID,
    .state = THREAD_RUNNING,
    .child_exit = {
        .name = "main.child_exit"
    }
};

struct thread idle_thread = {
    .name = "idle",
    .id = IDLE_TID,
    .state = THREAD_READY,
    .parent = &main_thread
};

static struct thread * thrtab[NTHR] = {
    [MAIN_TID] = &main_thread,
    [IDLE_TID] = &idle_thread
};

static struct thread_list ready_list;

// INTERNAL MACRO DEFINITIONS
// 

// Macro for changing thread state. If compiled for debugging (DEBUG is
// defined), prints function that changed thread state.

#define set_thread_state(t,s) do { \
    debug("Thread \"%s\" state changed from %s to %s in %s", \
        (t)->name, thread_state_name((t)->state), thread_state_name(s), \
        __func__); \
    (t)->state = (s); \
} while (0)

// Pointer to current thread, which is kept in the tp (x4) register.

#define CURTHR ((struct thread*)__builtin_thread_pointer())

// INTERNAL FUNCTION DECLARATIONS
//

// Finishes initialization of the main thread; must be called in main thread.

static void init_main_thread(void);

// Initializes the special idle thread, which soaks up any idle CPU time.

static void init_idle_thread(void);

// Sets the RISC-V thread pointer to point to a thread.

static void set_running_thread(struct thread * thr);

// Returns a string representing the state name. Used by debug and trace
// statements, so marked unused to avoid compiler warnings.

static const char * thread_state_name(enum thread_state state)
    __attribute__ ((unused));

// void recycle_thread(int tid)
// Reclaims a thread's slot in thrtab and makes its parent the parent of its
// children. Frees the struct thread of the thread.

static void recycle_thread(int tid);

// void suspend_self(void)
// Suspends the currently running thread and resumes the next thread on the
// ready-to-run list using _thread_swtch (in threasm.s). Must be called with
// interrupts enabled. Returns when the current thread is next scheduled for
// execution. If the current thread is RUNNING, it is marked READY and placed
// on the ready-to-run list. Note that suspend_self will only return if the
// current thread becomes READY.

static void suspend_self(void);

// The following functions manipulate a thread list (struct thread_list). Note
// that threads form a linked list via the list_next member of each thread
// structure. Thread lists are used for the ready-to-run list (ready_list) and
// for the list of waiting threads of each condition variable. These functions
// are not interrupt-safe! The caller must disable interrupts before calling any
// thread list function that may modify a list that is used in an ISR.

static void tlclear(struct thread_list * list);
static int tlempty(const struct thread_list * list);
static void tlinsert(struct thread_list * list, struct thread * thr);
static struct thread * tlremove(struct thread_list * list);
static void tlappend(struct thread_list * l0, struct thread_list * l1);

static void idle_thread_func(void * arg);

// IMPORTED FUNCTION DECLARATIONS
// defined in thrasm.s
//

extern struct thread * _thread_swtch(struct thread * resuming_thread);

extern void _thread_finish_fork(struct thread *child, void* child_ksp, const struct trap_frame *parent_tfr);

extern void _thread_setup (
    struct thread * thr, void * ksp, void (*start)(void *), ...);

extern void __attribute__ ((noreturn)) _thread_finish_jump (
    const struct thread_stack_anchor * stack_anchor,
    uintptr_t usp, uintptr_t upc, ...);


// EXPORTED FUNCTION DEFINITIONS
//

int running_thread(void) {
    return CURTHR->id;
}

void thread_init(void) {
    init_main_thread();
    init_idle_thread();
    set_running_thread(&main_thread);
    thrmgr_initialized = 1;
}

int thread_spawn(const char * name, void (*start)(void *), void * arg) {
    struct thread_stack_anchor * stack_anchor;
    void * stack_page;
    struct thread * child;
    int saved_intr_state;
    int tid;

    trace("%s(name=\"%s\") in %s", __func__, name, CURTHR->name);

    // Find a free thread slot.

    tid = 0;
    while (++tid < NTHR)
        if (thrtab[tid] == NULL)
            break;
    
    if (tid == NTHR)
        panic("Too many threads");
    
    // Allocate a struct thread and a stack

    child = kmalloc(sizeof(struct thread));

    stack_page = memory_alloc_page();
    stack_anchor = stack_page + PAGE_SIZE;
    stack_anchor -= 1;
    stack_anchor->thread = child;
    stack_anchor->reserved = 0;


    thrtab[tid] = child;

    child->id = tid;
    child->name = name;
    child->parent = CURTHR;
    child->proc = CURTHR->proc;
    child->stack_base = stack_anchor;
    child->stack_size = child->stack_base - stack_page;
    set_thread_state(child, THREAD_READY);

    saved_intr_state = intr_disable();
    tlinsert(&ready_list, child);
    intr_restore(saved_intr_state);

    _thread_setup(child, child->stack_base, start, arg);
    
    return tid;
}

/**
 * @brief Forks a new thread for a child process and sets up its execution context.
 *
 * This function allocates new memory for the child process and sets up another thread struct.
 * It also initializes a stack anchor to reclaim the thread pointer when coming back from a U mode interrupt.
 * The child's memory space is switched into and the thread is set to be run.
 * 
 *
 * @param child_proc Pointer to the child process structure.
 * @param parent_tfr Pointer to the trap frame of the parent process.
 * @return The thread ID of the newly created child thread.
 *
 * @note Several things that this function (and other functions that works with it) assures:
 * 1. The child has the same trap frame (in kernel stack) as the parent (including user stack pointer).
 *    We choose to make the child have the same kernel stack as the parent by memcpy, so the trap frame is also copied.
 * 2. The child has the same user stack as the parent (handled in memory_space_clone()).
 * 3. The child has a different kernel stack pointer (handled in thread_finish_fork()).
 * 4. Need to handle the fact that there is only one sscratch but multiple threads (need to preserve user stack pointer).
 *    This is stored in the trap frame (handled in trap_entry_from_umode).
 * 5. Context switch to the child thread (save parent context), child context copies parent context except tp and sp
 *    (handled in thread_finish_fork()).
 * 6. The child and parent need to sret with different values
 *    For parent we can just return, sysfork will store the return value into trap frame and then restored before sret
 *    For child we need to manually put this into the trap frame of the child, done in this function
 *     
 */
int thread_fork_to_user (struct process * child_proc, const struct trap_frame * parent_tfr){
    /*This function allocates new memory for the child process and sets up another thread struct. It also initializes
    a stack anchor to reclaim the thread pointer when coming back from a U mode interrupt. The child's memory
    space is switched into and the thread is set to be run. Another helper function, with the following signature，
    should be written in assembly which perforns the context switch*/

    // several things to consider:
    // 1. child need to have the same trap frame (in kernel stack) as the parent (including user stack pointer)
    // Note: We choose to make child have the same kernel stack as the parent by memcpy, so trap frame is also copied
    // (done below)

    // 2. child need to have the same user stack as the parent (need to copy in memory_space_clone())
    // (done in memory_space_clone()) 

    // 3. child need to have a different kernel stack pointer
    // (done in thread_finish_fork()) 

    // 4. need to find a way to deal with the fact that only one sscratch but multiple threads (need to preserve user stack pointer)
    // because when _thread_swtch, we are in S mode, so the sp in context will be kernel stack pointer
    // therefore we need a way to store user stack pointer. we can store this also in thread context
    // but we can also store this in trap frame (trap_entry_from_umode)
    // (done in trapasm.s) 

    // 5. Context switch to child thread, (save parent context), child context copies parent context except tp and sp
    // (done in thread_finish_fork())

    // 6. child and parent need to sret with different values.
    // (done in process_fork())

    intr_disable();

    trace("%s() in %s", __func__, CURTHR->name);

    // at this point, child_proc should have been initialized within process_fork

    assert(child_proc != NULL);

    // initialize the child thread
    // TODO: here starts initialization of the child thread, for the current case, we might assume one process got only one thread but there are multiple processes allowed
    
    // find a free thread slot.
    int child_tid = 0;
    while(child_tid < NTHR){
        if(thrtab[child_tid] == NULL){
            break;
        }
        child_tid ++;
    }

    if(child_tid == NTHR){
        panic("Too many threads");
    }

    void * child_kernel_stack_lowest = kmalloc(PAGE_SIZE);
    void * child_kernel_stack_base = child_kernel_stack_lowest + PAGE_SIZE;

    struct thread_stack_anchor * child_stack_anchor = (struct thread_stack_anchor *)(child_kernel_stack_base - sizeof(struct thread_stack_anchor));
    child_stack_anchor->thread = kmalloc(sizeof(struct thread));
    child_stack_anchor->reserved = 0;

    struct thread* child = child_stack_anchor->thread;
    thrtab[child_tid] = child;

    child->id = child_tid;
    child->name = "a forked thread";
    child->parent = CURTHR;
    child->proc = child_proc;
    child->stack_base = child_kernel_stack_base - sizeof(struct thread_stack_anchor);
    child->stack_size = child->stack_base - child_kernel_stack_lowest;
    set_thread_state(child, THREAD_RUNNING); // run child thread

    set_thread_state(CURTHR, THREAD_READY); // parent thread added to ready list
    tlinsert(&ready_list, CURTHR);

    memory_space_switch(child_proc->mtag); // switch to child memory space
    // get kernel stack pointer
    void* parent_kernel_sp;
    void* child_kernel_sp;
    asm inline ("mv %0, sp" : "=r" (parent_kernel_sp));

    // copy parent kernel stack to child kernel stack
    uint64_t parent_kstack_used_size = CURTHR->stack_base - parent_kernel_sp;


    // copy [parent sp ~ parent->stack_base] to [child_kernel_sp - used_stack_size ~ child->stack_base]
    child_kernel_sp = child->stack_base - parent_kstack_used_size;
    memcpy(child_kernel_sp, parent_kernel_sp, parent_kstack_used_size);
    
    // performs context switch
    _thread_finish_fork(child, child_kernel_sp, parent_tfr);

    // child thread
    if(running_thread() == child_tid){
        // if we are in child thread, then we need to store 0 into the a0 of child trap frame
        struct trap_frame * c_tfr = (struct trap_frame *)(child->stack_base) - 1;
        c_tfr->x[TFR_A0] = 0;    
    }
    return child_tid;
}

void thread_exit(void) {
    if (CURTHR == &main_thread){
        kprintf("ending main thread\n");
        halt_success();
    }
    set_thread_state(CURTHR, THREAD_EXITED);

    // Signal parent in case it is waiting for us to exit

    assert(CURTHR->parent != NULL);
    condition_broadcast(&CURTHR->parent->child_exit);

    suspend_self(); // should not return
    panic("thread_exit() failed");
}

void thread_jump_to_user(uintptr_t usp, uintptr_t upc) {
    intr_disable(); // disable interrupt because we are in smode but we set stvec to umode entry point
    csrw_stvec(_trap_entry_from_umode); // set stvec to umode entry point so it know sp is not in kernel stack
    csrc_sstatus(RISCV_SSTATUS_SPP); // so that sret returns to user mode
    csrs_sstatus(RISCV_SSTATUS_SPIE); // enable supervisor mode interrupt so that user process can trigger int
    _thread_finish_jump(CURTHR->stack_base, usp, upc);
}

void thread_yield(void) {
    trace("%s() in %s", __func__, CURTHR->name);

    // assert (intr_enabled());
    assert (CURTHR->state == THREAD_RUNNING);

    suspend_self();
}

int thread_join_any(void) {
    int childcnt = 0;
    int tid;

    trace("%s() in %s", __func__, CURTHR->name);

    // See if there are any children of the current thread, and if they have
    // already exited. If so, call thread_wait_one() to finish up.

    for (tid = 1; tid < NTHR; tid++) {
        if (thrtab[tid] != NULL && thrtab[tid]->parent == CURTHR) {
            if (thrtab[tid]->state == THREAD_EXITED)
            {

                return thread_join(tid);
            }

            childcnt++;
        }
    }

    // If the current thread has no children, this is a bug. We could also
    // return -EINVAL if we want to allow the calling thread to recover.
    if (childcnt == 0)
        panic("thread_wait called by childless thread");

    // Wait for some child to exit. An exiting thread signals its parent's
    // child_exit condition.

    condition_wait(&CURTHR->child_exit);

    for (tid = 1; tid < NTHR; tid++) {
        if (thrtab[tid] != NULL &&
            thrtab[tid]->parent == CURTHR &&
            thrtab[tid]->state == THREAD_EXITED)
        {
            recycle_thread(tid);
            return tid;
        }
    }

    panic("spurious child_exit signal");
}

// Wait for specific child thread to exit. Returns the thread id of the child.

int thread_join(int tid) {
    struct thread * const child = thrtab[tid];

    trace("%s(tid=%d)", __func__, tid);

    if (tid <= 0 || NTHR <= tid)
        return -1;

    trace("%s(tid=%d) in %s", __func__, tid, CURTHR->name);

    // Can only wait for child if we're the parent

    if (child == NULL || child->parent != CURTHR)
        return -1;
    
    // Wait for child to exit. Whenever a child exits, it signals its parent's
    // child_exit condition.

    while (child->state != THREAD_EXITED)
        condition_wait(&CURTHR->child_exit);
    
    recycle_thread(tid);

    return tid;
}

struct process * thread_process(int tid) {
    assert (0 <= tid || tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->proc;
}

void thread_set_process(int tid, struct process * proc) {
    assert (0 <= tid || tid < NTHR);
    assert (thrtab[tid] != NULL);
    thrtab[tid]->proc = proc;
}

const char * thread_name(int tid) {
    assert (0 <= tid || tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->name;
}

void condition_init(struct condition * cond, const char * name) {
    cond->name = name;
    tlclear(&cond->wait_list);
}

void condition_wait(struct condition * cond) {
    int saved_intr_state;

    trace("%s(cond=<%s>) in %s", __func__, cond->name, CURTHR->name);

    assert(CURTHR->state == THREAD_RUNNING);

    // Insert current thread into condition wait list
    
    set_thread_state(CURTHR, THREAD_WAITING);
    CURTHR->wait_cond = cond;
    CURTHR->list_next = NULL;

    saved_intr_state = intr_disable();
    tlinsert(&cond->wait_list, CURTHR);
    intr_restore(saved_intr_state);
    suspend_self();
}

void condition_broadcast(struct condition * cond) {
    int saved_intr_state;
    struct thread * thr;

    // Fast path: if there are no threads waiting, return.

    if (tlempty(&cond->wait_list))
        return;

    // Mark all waiting threads runnable. This is *not* a constant-time
    // operation, however, keeping having an enum thread_state member of struct
    // thread for keeping track of thread state is useful for debugging.

    saved_intr_state = intr_disable();

    for (thr = cond->wait_list.head; thr != NULL; thr = thr->list_next) {
        assert (thr->state == THREAD_WAITING);
        assert (thr->wait_cond == cond);
        set_thread_state(thr, THREAD_READY);
        thr->wait_cond = NULL;
    }

    // Append condition variable wait list to run list

    tlappend(&ready_list, &cond->wait_list);
    tlclear(&cond->wait_list);

    intr_restore(saved_intr_state);
}

// INTERNAL FUNCTION DEFINITIONS
//

void init_main_thread(void) {
    extern char _main_stack_anchor[]; // from thrasm.s
    extern char _main_stack_lowest[]; // from thrasm.s

    main_thread.stack_base = _main_stack_anchor;
    main_thread.stack_size = _main_stack_anchor - _main_stack_lowest;
}

void init_idle_thread(void) {
    extern char _idle_stack_anchor[]; // from thrasm.s
    extern char _idle_stack_lowest[]; // from thrasm.s

    extern void _thread_setup (
        struct thread * thr, void * sp, void (*start)(void *), ...);

    idle_thread.stack_base = _idle_stack_anchor;
    idle_thread.stack_size = _idle_stack_anchor - _idle_stack_lowest;
    _thread_setup(&idle_thread, _idle_stack_anchor, (void *)idle_thread_func);
    tlinsert(&ready_list, &idle_thread); // interrupts still disabled

}

static void set_running_thread(struct thread * thr) {
    asm inline ("mv tp, %0" :: "r"(thr) : "tp");
}

const char * thread_state_name(enum thread_state state) {
    static const char * const names[] = {
        [THREAD_UNINITIALIZED] = "UNINITIALIZED",
        [THREAD_STOPPED] = "STOPPED",
        [THREAD_WAITING] = "WAITING",
        [THREAD_RUNNING] = "RUNNING",
        [THREAD_READY] = "READY",
        [THREAD_EXITED] = "EXITED"
    };

    if (0 <= (int)state && (int)state < sizeof(names)/sizeof(names[0]))
        return names[state];
    else
        return "UNDEFINED";
};

void recycle_thread(int tid) {
    struct thread * const thr = thrtab[tid];
    int ctid;

    assert (0 < tid && tid < NTHR && thr != NULL);
    assert (thr->state == THREAD_EXITED);

    // Make our parent the parent of our children

    for (ctid = 1; ctid < NTHR; ctid++) {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == thr)
            thrtab[ctid]->parent = thr->parent;
    }

    thrtab[tid] = NULL;
    kfree(thr);
}

void suspend_self(void) {
    struct thread * susp_thread; // suspending thread
    struct thread * next_thread; // resuming thread
    struct thread * prev_thread; // previously thread
    int saved_intr_state;

    trace("%s() in %s", __func__, CURTHR->name);

    // The idle thread is always runnable, and the idle thread only calls
    // suspend_self() if the ready_list is not empty.

    assert (!tlempty(&ready_list));

    susp_thread = CURTHR;

    // Get a READY thread from the ready list and mark it running

    saved_intr_state = intr_disable();

    next_thread = tlremove(&ready_list);
    assert(next_thread->state == THREAD_READY);
    set_thread_state(next_thread, THREAD_RUNNING);
    
    // If the current thread is still running, mark it ready-to-run and put it
    // in the back of the ready-to-run list.

    if (susp_thread->state == THREAD_RUNNING) {
        set_thread_state(susp_thread, THREAD_READY);
        tlinsert(&ready_list, susp_thread);
    }

    intr_enable();

    if (next_thread->proc != NULL)
        memory_space_switch(next_thread->proc->mtag);

    trace("Thread <%s> calling _thread_swtch(<%s>)",
        CURTHR->name, next_thread->name);
    
    prev_thread = _thread_swtch(next_thread);

    trace("_thread_swtch() returned in %s", CURTHR->name);

    if (prev_thread->state == THREAD_EXITED) {
        memory_free_page(prev_thread->stack_base - PAGE_SIZE);
        prev_thread->stack_base = NULL;
        prev_thread->stack_size = 0;
    }

    intr_restore(saved_intr_state);
}

void tlclear(struct thread_list * list) {
    list->head = NULL;
    list->tail = NULL;
}

int tlempty(const struct thread_list * list) {
    return (list->head == NULL);
}

void tlinsert(struct thread_list * list, struct thread * thr) {
    thr->list_next = NULL;

    if (thr == NULL)
        return;

    if (list->tail != NULL) {
        assert (list->head != NULL);
        list->tail->list_next = thr;
    } else {
        assert(list->head == NULL);
        list->head = thr;
    }

    list->tail = thr;
}

struct thread * tlremove(struct thread_list * list) {
    struct thread * thr;

    thr = list->head;
    
    if (thr == NULL)
        return NULL;

    list->head = thr->list_next;
    
    if (list->head != NULL)
        thr->list_next = NULL;
    else
        list->tail = NULL;

    thr->list_next = NULL;
    return thr;
}

// Appends elements of l1 to the end of l0 and clears l1.

void tlappend(struct thread_list * l0, struct thread_list * l1) {
    if (l0->head != NULL) {
        assert(l0->tail != NULL);
        
        if (l1->head != NULL) {
            assert(l1->tail != NULL);
            l0->tail->list_next = l1->head;
            l0->tail = l1->tail;
        }
    } else {
        assert(l0->tail == NULL);
        l0->head = l1->head;
        l0->tail = l1->tail;
    }

    l1->head = NULL;
    l1->tail = NULL;
}

void idle_thread_func(void * arg __attribute__ ((unused))) {
    // The idle thread sleeps using wfi if the ready list is empty. Note that we
    // need to disable interrupts before checking if the thread list is empty to
    // avoid a race condition where an ISR marks a thread ready to run between
    // the call to tlempty() and the wfi instruction.

    for (;;) {
        // If there are runnable threads, yield to them.

        while (!tlempty(&ready_list))
            thread_yield();
        
        // No runnable threads. Sleep using the wfi instruction. Note that we
        // need to disable interrupts and check the runnable thread list one
        // more time (make sure it is empty) to avoid a race condition where an
        // ISR marks a thread ready before we call the wfi instruction.

        intr_disable();
        if (tlempty(&ready_list))
            asm ("wfi");
        intr_enable();
    }
}
