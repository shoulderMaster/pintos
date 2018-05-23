#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "vm/page.h"

/* 한 프로세스에 있는 FDT의 entry 최대 개수
   multi-oom 테스트 케이스가 126개까지 fd를 열어봄 */
#define FILE_MAX 192

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
int _get_argc(char* file_name);
char** _get_argv(char* file_name);
void argument_stack(char **parse, int count, void **esp);
struct thread *get_child_process (int pid);
void remove_child_process (struct thread *cp); 
void process_close_file (int fd);
struct file * process_get_file (int fd);
int process_add_file (struct file *f);

/* filesys_open() 따위에 열려서 넘겨진 파일 객체 포인터를 인자로 받아서
   해당 파일 객체를 해당 프로세스의 FDT에 추가해주는 함수
   파일 디스크립터 번호를 리턴함 */
int process_add_file (struct file *f) {
  struct thread *cur = thread_current ();
  // 파일 객체가 저장될 FDT entry번호를 저장함.
  int current_fd = cur->next_fd;
  
  // next_fd 번째 자리 FDT entry에 해당 파일 객체 포인터를 저장함.
  cur->FDT[cur->next_fd] = f;
  
  /* 다음번에 파일 객체를 받을 descriptor number를 정해야하는데
     이번에 할당한 숫자 다음 숫자부터 검사해서 NULL 값이 들어있는 entry번호를 찾아 놓음
     null 값이 들어 있다는 것은 파일 객체 포인터가 들어있지 않다는 것을 의미하므로
     다음번 파일 객체를 받을 때 next_fd번째에 바로 저장할 수 있도록 함.*/
  do {
    /* 수식이 복잡하지만 이렇게 하면 2~(FILE_MAX-1) 를 돌면서 next_fd를 setting할 수 있음 */
    cur->next_fd = (cur->next_fd + 1 - 2) % (FILE_MAX - 2) + 2;
    /* 한바퀴 돌고도 빈 FDT entry를 못찾으면 -1 리턴 */
    if (cur->next_fd == current_fd) 
      return -1;
  } while (cur->FDT[cur->next_fd] != NULL);
  
  // 리턴 값으로 해당 파일을 저장한 FD number를 리턴함
  return current_fd;
}

/* 파일 디스크립터 숫자를 넘겨주면 그에 해당하는 파일 객체 포인터를 리턴해줌 */
struct file * process_get_file (int fd) {
  /* 해당 엔트리에 파일이 있으면 파일 객체 포인터가 리턴되고,
     파일 객체가 없는 경우에는 해당 엔트리에 null값이 저장되어있으므로 null이 리턴됨 */
  return thread_current ()->FDT[fd];
}

/* 파일을 닫기 위해 파일 디스크립터를 해제함 */
void process_close_file (int fd) {
  struct file *file = process_get_file (fd);
  /* 닫고자 하는 파일의 inode reference count를 1내림
     inode는 근데 뭐임? */
  file_close (file);
  /* 해당 FDT의 엔트리를 null 값으로 바꿈 */
  thread_current ()->FDT[fd] = NULL;

}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy, *_saved_ptr, *_trimed_file_name;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);
  

  /*  실행 파일 이름과 인자가 한 문자열로 되어 있는 file_name을 tokenize하여
      실행 파일 이름을 가져옴.
      strtok_r()의 리턴 값을 따로 받을 필요없이 file_name을 그대로 사용해도 되지만,
      가독성을 위해 _trimed_file_name 변수에 따로 받아서 사용함.
     */
  _trimed_file_name = strtok_r(file_name, " ", &_saved_ptr);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (_trimed_file_name, PRI_DEFAULT, start_process, fn_copy);
  
  if (tid == TID_ERROR) {
    palloc_free_page (fn_copy);
  }
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  char *file_name = file_name_;
  char **argv;
  int argc;
  struct intr_frame if_;
  bool success;
 
  // 인자의 개수를 셈.
  argc = _get_argc(file_name);
  
  /* file_name에 프로그램 이름과 인자들을 각각 구분하여 잘라
     문자열 포인터 배열을 반환함.
     내부적으로 malloc을 사용하여 메모리 동적 할당을 하기 때문에
     사용을 마치고 나면 반드시 리턴 값에 대해 free를 해줘야함.  */
  argv = _get_argv(file_name);

  /* vm_init() 함수를 이용하여 해시테이블 초기화 */
  vm_init (&thread_current ()->vm); 
  
  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);
  
  /* load()에 의해 ELF파일이 프로세스 주소공간에 로드가 완료되면
     부모 프로세스에게 세마포어 객체를 up하여 이를 알림.
     부모 프로세스는 갓 생성한 자식 프로세스가 load를 마칠 때 까지 block상태로 기다림.
  */
  sema_up (&thread_current()->load_sema);
  
  /* If load failed, quit. */
  if (!success)
  {

    /* 스레드가 비정상 종료인 것을 알리기 위해 
       그냥 thread_exit()로 종료하는 것이 아닌,
       PCB->exit_status에 종료코드로 -1를 넣어 비정상 종료임을 표시함. 
       근데 그냥 exit(-1) 하면 되는거 아님? */
    //thread_current()->exit_status = -1;
    //thread_exit ();
    exit (-1);
  } else {
    
    /* 프로세스가 성공적으로 load되었으면
       PCB에 해당하는 thread 구조체의 
       현재 프로세스 로드 상태,
       현재 프로세스 종료 상태 코드
       현재 프로세스 종료 여부 등을 초기화함.
    */
    thread_current()->loaded = true;
    thread_current()->exit_status = 0;
    thread_current()->exited = false;

    /* 새로 실행될 프로세스는 커널 메모리 영역에 있는 인자들을
       그대로 사용할 수 없다.
       프로세스의 유저 스택에 현재 커널에 저장된 argc랑 argv를 복사해서 넘겨준다.
    */
    argument_stack(argv, argc, &if_.esp);
  }

  // get_argv() 의 리턴값으로 받아온 동적할당 메모리 주소를 해제함.
  /* file_name_에 대한 해제는 process_execute()에서 수행
     하는 줄 알았는데 process_execute()가 비정상 종료 될 때만 해제 해주는 거였음*/
  palloc_free_page (file_name_);
  free (argv);



  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

// command_line에서 인자의 개수를 세어서 리턴함.
int _get_argc(char* file_name)
{
  char *tmp_argv, *save_ptr, *tmp_str;
  int argc = 0;

  tmp_argv = (char*)malloc((strlen(file_name)+1) * sizeof(char));
  strlcpy(tmp_argv, file_name, strlen(file_name)+1);

  for (tmp_str = strtok_r(tmp_argv, " ", &save_ptr); tmp_str != NULL; tmp_str = strtok_r(NULL, " ", &save_ptr))
  {
      argc++;
  }

  free(tmp_argv);

  return argc;

}

/* command_line으로 받아온 문자열을 바로 tokenize하여 사용하기 보다,
   새로 메모리 동적할당을 받아 복사를 하여 tokenize해서 해당 문자열 포인터 배열을 반환함. */
char** _get_argv(char* file_name) 
{
  char **argv, *save_ptr, *tmp_str;
  int i = 0;
  
  argv = (char**)malloc(_get_argc(file_name) * sizeof(char*));
  
  for (tmp_str = strtok_r(file_name, " ", &save_ptr); tmp_str != NULL; tmp_str = strtok_r(NULL, " ", &save_ptr))
  {
    argv[i] = tmp_str;
    i++;
  }

  return argv;
}

// 인자를 새로 실행될 프로세스의 유저 스택에다가 복사하여 넘겨주는 함수
void argument_stack(char **parse, int count, void **esp) 
{
  char **tmp_argv, **tmp_pointer;
  int i, str_size, align_size;
  
  // for fake return address
  void * const RETURN_ADDRESS = 0x00000000;
  
  tmp_argv = (char**)malloc(sizeof(char*)*count);

  // for문을 돌며 인자를 구성하는 문자열들을 스택에 넣고 임시배열에 각 문자열들을 가리키는 포인터를 저장함.
  for (i = count - 1; i >= 0; i--) 
  {
    str_size = sizeof(char)*(strlen(parse[i]) + 1);
    *esp -= str_size;
    strlcpy(*esp, parse[i], str_size);
    tmp_argv[i] = *esp;
  }
  
  /* 4바이트 단위의 효율적 메모리 접근을 위해
     esp를 4의 배수로 만들고 그 사이를 0으로 패딩함.
     mod 연산이 직관적일 수는 있겠지만 굳이 연산 효율을 고려한다면
     비트연산이 보다 낫다고 판단되어 비트 마스킹으로 구현함. */
  align_size = (((int)*esp)&0x00000003);
  *esp -= align_size;
  memset(*esp, 0x00, align_size);

  /* argv[argc] 에 0x00000000을 넣어주어서 argv의 마지막임을 표시.
     사실 근데 argv에 접근할 때는 ebp기준으로 argc만큼 접근하기 때문에 이런 표시 없이도 동작 가능
     하다고 생각 했었는데 arg 관련 테스트 케이스를 통과하려면 넣어줘야함....  */
  *esp -= sizeof(char*);
  *(char**)*esp = 0;
  
  // for 문을 돌면서 임시 배열에 저장해 놓았던 argv[]를 유저스택에 PUSH함
  for (i = count - 1; i >= 0; i--)
  {
    *esp -= sizeof(tmp_argv[i]);
    *(char**)*esp = tmp_argv[i];
  }
  
  // for 문을 돌고나면 esp가 argv시작주소를 가리킴. 이 esp 값을 유저스택에 argv로서 저장함.
  tmp_pointer = *esp;
  *esp -= sizeof(char**);
  *(char***)*esp = tmp_pointer;
  
  // argc를 유저 스택에 PUSH함 
  *esp -= sizeof(int);
  *(int*)*esp = count;

  // fake return address를 push하여 스택 프레임을 형성함.
  *esp -= 4;
  *(void**)*esp = RETURN_ADDRESS;

  //사용을 마친 임시 배열을 해제함.
  free(tmp_argv);

}

/* 현재 프로세스의 자식프로세스 중 pid를 검색하여 PCB를 리턴함.
   검색된 pid가 없으면 null을 리턴함.
   */
struct thread *get_child_process (int pid) 
{
  struct thread *cur = thread_current(), *tmp_t;
  struct list_elem *elem;
  
  // for문을 자식 프로세스 리스트 head.next부터 tail 까지 돌면서 pid(tid)를 검색함. 
  for (elem = cur->child_list.head.next; elem != &cur->child_list.tail; elem = elem->next)
  {
    tmp_t = list_entry (elem, struct thread, child_elem);
    if (pid ==  tmp_t->tid)
      {
        
        // 해당하는 pid를 찾으면 해당자식 프로세스의 PCB 리턴.
        return tmp_t;
      }
  }
  return NULL;
}

/* 인자로 받아온 PCB를 해제함. 
   부모 프로세스가 자식프로세스의 종료를 기다리고 
   자식프로세스의 PCB를 해제하기 위해 사용. */
void remove_child_process (struct thread *cp) 
{
  list_remove (&cp->child_elem);
  palloc_free_page(cp);
}


/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_pid) 
{
  struct thread *child = NULL;
  int status = 0;
  
  /* 인자로 받아온 자식 프로세스의 pid를 자식 프로세스 리스트에서 linear search하여 
     해당 프로세스의 PCB를 가져옴*/
  child = get_child_process(child_pid);
  
  // 해당 pid를 가진 자식 프로세스가 없으면 null을 리턴값으로 받아 옴. return -1로 wait함수를 종료시킴.
  if (!child)
  {
    return -1;
  }

  /*  wait하고자 하는 자식 프로세스가 아직 종료가 안되었다면 
      프로세스가 종료되고 sema_up될 때까지 block상태로 기다린다.*/
  if (!child->exited)
  {
    sema_down(&child->exit_sema);
  }
  
  /*  자식 프로세스 종료 코드를 받아오고 
      자식 프로세스의 PCB를 해제하는 작업을 함. */
  status = child->exit_status;
  remove_child_process(child);
  
  return status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  int i;
  

  /* FDT에 있는 모든 file 객체들의 inode 의 reference count를 1씩 뺀다
     process_close_file 내부적으로 null값에 대해서 file_close()에 의해 알아서 예외 처리됨.
     고로 파일 디스크립터가 할당 안된 FDT entry에 대해서도 그냥 인자를 넘겨서 실행해도 문제가 없음
     파일객체 포인터가 들어있지 않다면 NULL이 들어있기 때문
     STDIN, STDOUT이 있는 0번째, 1번째 FDT entry에는 따로 해제를 하지 않음 
     그리고 kernel의 main 프로세스와 idle process는 프로세스 초기화 할 때 FDT를 초기화 하지 않으므로
     double free가 발생하지 않게 이 루틴은 작동하지 않게 함.*/
  if (!(cur->tid == 1 || cur->tid == 2)) {
  
    for (i = 2; i < FILE_MAX; i++) {
      process_close_file (i);
    }
     /* FDT는 PCB와 같은 페이지에 있는 것이 아닌 따로 할당을 해줬었음.
        고로 따로 페이지 해제를 해줘야함 */
     palloc_free_page(cur->FDT);
  }
  /* load() 함수에 의해 열린 ELF파일 객체를 프로세스 종료 때 해제함.
     이 파일을 해제함으로 다른 프로세스가 ELF파일에 write할 수 있게 허용해줌.
     물론 이 ELF로 아직 실행 중인 다른 프로세스가 있다면 그 프로세스들이 모두 종료될 때까지 기다려야함 */
  file_close (cur->run_file);

  /* vm_entry들을 제거하는 함수 추가 */
  vm_destory (&cur->vm);
  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
        /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();
  
  /* 파일 read, write시 lock을 사용해 동시 접근을 막아야하므로 
     syscall.c 에서 전역변수로 만들었던 rw_lock을 사용하여 lock을 걺 */
  lock_acquire (&rw_lock);
  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* ELF 파일을 열어서 프로세스에 올려 실행하는동안 다른 프로세스가 해당 ELF파일에 접근하면 
     원래 실행하고자 하는 파일과 내용이 달라져 무결성이 깨질 수 있으므로
     ELF 파일을 실행파일로서 작동시키는 중에는 다른 프로세스로부터 접근되어 write를 하지 못하도록
     해당 파일을 보호해야함. file_deny_write()으로 해당 파일 객체를 넘기면 해당 파일 객체의 
     deny_write 멤버가 true로 체크되어 해당 파일이 executable파일로서 실행 중인것을 구분하여주고
     해당 파일 객체의 inode 객체의 deny_write_count 멤버를 1늘려 
     해당 파일을 실행중인 프로세스의 개수를 표시하여준다. 
     해당 파일을 실행중인 프로세스가 나중에 종료되면 해당 실행 중인 thread구조체를 참조하여 해당 파일 객체를
     file_close() 를 호출하여 닫아주는데, 이 과정에서 함수 내부적으로 
     file_allow_write() 를 호출하게 되어 있다. 그러면 해당 파일 객체의 deny_write 멤버를 false로 바꿔
     해당 파일이 쓰기가 가능하단 것을 표시하고 (근데 어차피 이 객체는 해제될 것임)
     커널에 파일 별로 유일하게 존재하는 inode구조체에 deny_write_count를 1 줄여서
     해당 파일을 실행파일로서 실행중인 프로세스가 1개 줄었다는 것을 표시한다. */
  file_deny_write (file);
  t->run_file = file;

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  /* ELF 파일에 대한 해제는 process_exit()에서 수행하므로 기존에 있던 file_close()는 없애버림.*/
  //file_close (file);
  /* elf file load 전에 걸었던 lock 반 */  
  lock_release (&rw_lock);
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES: bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        } */
           
      /*  vm_entry 생성 (malloc 사용) */
      struct vm_entry *vme = (struct vm_entry*)malloc (sizeof (struct vm_entry));
      memset (vme, NULL, sizeof (struct vm_entry));
      if (vme == NULL) 
        return false;
      
      /*  vm_entry 멤버들 설정, 가상페이지가 요구될 때 읽어야할 파일의 오프
       셋과 사이즈, 마지막에 패딩할 제로 바이트 등등 */
      vme->type = VM_FILE;
      vme->writable = writable;
      vme->is_loaded = false;
      vme->file = file;
      vme->offset = ofs;
      vme->read_bytes = read_bytes;
      vme->zero_bytes = zero_bytes;
      

      /*  insert_vme() 함수를 사용해서 생성한 vm_entry를 해시테이블에 추가 */
      insert_vme (thread_current ()->vm, vme);

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
