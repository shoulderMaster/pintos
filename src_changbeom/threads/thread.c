#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

#define MAX_DEPTH 8

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* sleep된 프로세스 구조체들을 관리하는 리스트. alarm clock 구현시 사용 */
static struct list sleep_list;

/* sleep_list 에 저장된 프로세스 wakeup_tick 시간 중 가장 작은 것을 저장.
   초기 값으로 가장 큰 숫자인 INT64_MAX 넣어줌 */
int64_t next_tick_to_awake = INT64_MAX;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
void test_max_priority (void); 
bool cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) ;
void remove_with_lock (struct lock *lock);
void donate_priority (void);
void refresh_priority (void); 



bool cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *thread_a, *thread_b;
  thread_a = list_entry (a, struct thread, elem);
  thread_b = list_entry (b, struct thread, elem);
  
  return thread_a->priority > thread_b->priority;
}


void donate_priority (void) {
  int depth = 1;
  struct thread *thread = thread_current (); 
  struct lock *lock = thread->wait_on_lock;
  while (depth < MAX_DEPTH && lock) {
    if (   lock->holder 
        && lock->holder->priority < thread->priority ) { 
      lock->holder->priority = thread->priority;
      thread = lock->holder;
      lock = thread->wait_on_lock;
      depth++;
    } else {
      break;
    }
  }
}

void remove_with_lock (struct lock *lock) {
  struct list_elem *elem = NULL;
  struct list *lock_waiters = &lock->semaphore.waiters;
  struct thread *donator = NULL;
  for (elem = list_begin (lock_waiters)
      ; elem != list_end (lock_waiters); elem = list_next (elem)) {
    donator = list_entry (elem, struct thread, elem);
    list_remove (&donator->donation_elem);
  }
}

void refresh_priority (void) {
  struct thread *cur = thread_current (); 
  struct thread *donator = NULL;
  struct list_elem *elem = NULL;
  int max_priority = cur->init_priority;
  
  if (list_empty (&cur->donations)) {
    cur->priority = cur->init_priority;
    return;
  }
  
  elem = list_begin (&cur->donations);  
  donator = list_entry (elem, struct thread, donation_elem);
  if (max_priority < donator->priority) {
    max_priority = donator->priority;
  }
  cur->priority = max_priority;
}

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  /* sleep_list 초기화 함. */
  list_init (&sleep_list);


  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}



void thread_sleep (int64_t ticks) {
  struct thread *cur = thread_current ();
  enum intr_level old_level = intr_disable ();
  //struct thread *prev_t = NULL, *next_t = NULL;
  if (cur->tid != 2)  {

    //cur->status = THREAD_BLOCKED;
    cur->wakeup_tick = ticks; 
    list_push_back (&sleep_list, &cur->elem);
    update_next_tick_to_awake (ticks); 
    thread_block ();
    /*  
    for (elem = sleep_list.head.next; elem != &sleep_list.tail; elem = elem->next) { 
      cur_t = list_entry (elem, struct thread, elem);
      next_t = list_entry (elem->next, struct thread, elem);
      if (prev_t->wakeup_tick <= cur->wakeup_tick )
     */
  }
  intr_set_level (old_level);
}

void thread_awake (int64_t ticks) {
  struct list_elem *elem = list_begin (&sleep_list);
  struct thread *pcb = NULL;
  /* for (elem = sleep_list.head.next; elem != &sleep_list.tail; elem = elem->next) {
    pcb = list_entry (elem, struct thread, sleep_elem);
    if (pcb->wakeup_tick <= ticks) {
      list_remove (elem);
      thread_unblock (pcb);
    } else {
      update_next_tick_to_awake (pcb->wakeup_tick);
    }
  } */
  while (elem != list_end (&sleep_list)) {
    pcb = list_entry (elem, struct thread, elem);
    elem = list_next (elem);
    if (pcb->wakeup_tick <= ticks) {
      thread_unblock (pcb);
    } else {
      update_next_tick_to_awake (pcb->wakeup_tick);
    }
  }
}

void update_next_tick_to_awake (int64_t ticks) {
  if (ticks < next_tick_to_awake ) {
    next_tick_to_awake = ticks;
  }
}

int64_t get_next_tick_to_awake (void) {
  return next_tick_to_awake;  
}


/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* 새 스레드를 생성하고 초기화를 함.
     PCB에 새로 추가한 자식 프로세스 리스트 멤버를 여기 init_thread()에서 초기화함. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);
  /* PCB가 생성되면 PCB 내부의 몇몇 멤버 변수들을 초기화함.
     부모 프로세스
     현재 프로그램이 프로세스 메모리 공간에 load됐는지 여부
     현재 프로세스가 종료됐는지 여부
     로드용, 종료용 세마포어 객체
   */
  t->parent = thread_current();
  t->loaded = false;
  t->exited = false;
  sema_init(&t->load_sema, 0);
  sema_init(&t->exit_sema, 0);

  /* file 관련 구조체들 초기화 함
     FDT는 반드시 0으로 초기화 해야하고 커널 풀에 할당해야함 */
  t->FDT = palloc_get_page (PAL_ZERO);
  /* 0과 1은 이미 STDIN과 STDOUT이 사용중이므로 
     2번째 엔트리부터 파일 디스크립터를 할당 받을 수 있도록 2로 초기화 함 */
  t->next_fd = 2;

  //커널에서 관리하는 모든 프로세스 리스트 구조체에 새로 생성된 PCB를 삽입함.
  list_push_back(&thread_current()->child_list, &t->child_elem);
  /* Add to run queue. */
  thread_unblock (t);
  test_max_priority ();
  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  //list_push_back (&ready_list, &t->elem);
  list_insert_ordered (&ready_list, &t->elem, cmp_priority, NULL);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}


void test_max_priority (void) {

  struct list_elem *cur = &thread_current ()->elem;
  struct list_elem *highest_priority_thread;
  
  if (!list_empty (&ready_list)) {
    highest_priority_thread = list_begin (&ready_list);
   
    if (cmp_priority (highest_priority_thread, cur, NULL))
      thread_yield ();
  }
  return;
}



/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();

  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();

  // 커널에서 전체 프로세스 목록을 관리하는 리스트에서 종료하고자 하는 프로세스 PCB element를 제거함.
  list_remove (&thread_current()->allelem);
  
  // 현재 프로세스의 PCB에 종료된 프로세스임을 표시함.
  thread_current ()->exited = true;

  /* 커널이 부팅하고 나서 첫번째와 두번째로 생기는 main 프로세스와 idle프로세스는 
     PCB내부의 세마포어 객체를 사용하지 않고 커널에서 관리하는 전용 전역 semaphore객체를 사용하므로
     PCB 내부의 세마포어 객체를 다루는 이 루틴에서 main 프로세스와 idle프로세스에 대해선 작동하지 않게함.*/
  if (!(thread_current()->tid == 1 || thread_current()->tid == 2)) 
  {
    // 종료 세마포어 객체를 up하여 wait하고 있는 부모프로세스에게 프로세스 종료를 알림.
    sema_up (&thread_current ()->exit_sema);
  }
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  /* if (cur != idle_thread) 
    list_push_back (&ready_list, &cur->elem); */
  list_insert_ordered (&ready_list, &cur->elem, cmp_priority, NULL);  
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  struct thread *cur = thread_current ();
  int old_priority = cur->priority;
  cur->init_priority = new_priority;
  refresh_priority ();
  if (old_priority < cur->priority) {
    donate_priority ();
  }
  test_max_priority (); 
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  /* Not yet implemented. */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
/* 여기가 커널 main thread 생성할 때도 호출되네.. 
   그냥 여기에 thread 초기화 하는 내용은 다 넣어도 될거 같은데
   복잡하게 process_create에다가 나눠 넣지 말고.. 특히 세마포어 초기화*/
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;
  list_push_back (&all_list, &t->allelem);
  
  // 자식 리스트 구조체 멤버를 초기화 함.
  list_init(&t->child_list);
  /* process_exit () 에 의해 혹시 진짜 스레기 값이 file_close()되지 않을까 하여
     기본 초기 값을 NULL로 초기화 해줌. NULL값은 알아서 예외처리 됨. */
  t->run_file = NULL;

  t->init_priority = priority;
  t->wait_on_lock = NULL;
  list_init (&t->donations);


}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      //palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
