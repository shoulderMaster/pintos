#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* 보다 직관적인 check_address() 를 작성하기 위해 
   각 메모리 영역 시작 주소 값을 USER_START, KERNEL_START로 정의함. */
#define USER_START      0x08048000
#define KERNEL_START    0xc0000000

typedef int pid_t;
static void syscall_handler (struct intr_frame *);
void check_address (void *addr);
void get_argument (void *esp, int *arg, int count);
void halt ();
void exit (int status);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
pid_t exec (const *cmd_line);


void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int syscall_num;
  int arg[5] = {0, };
  int *esp = f->esp;
  
  /* esp가 유저모드의 메모리 주소 영역인지 체크한다.
     유저모드에서 넘어온 모든 인자의 주소와 포인터는
     check_address() 로 메모리 범위를 검사해줘야 한다.
     현재 상태는 kernel모드인 privilege 모드이므로, 
     커널 입장에서 비신뢰적인 유저모드 코드에서 넘어온 인자를 검사하지 않으면,
     유저모드에서 커널 메모리 영역 접근을 의도할 수 있다. */
  check_address (esp);

  
  /* 유저모드에서 int 0x30 (시스템콜 인터럽트) 를 발생시키기 전에 스택에 인자로 넘겨준
     system call number를 가져온다.  */
  syscall_num = *esp;

  // system call number는 src/lib/syscall-nr.h에 enum 구조체에 정의되어 있다.
  // 각 system call number별로 switch문에서 분기하여 system call을 수행한다.
  switch (syscall_num) 
  {

    case SYS_HALT :
        halt ();
        break;

    case SYS_EXIT :
        get_argument (esp, arg, 1);
        exit (arg[0]);
        f->eax = arg[0];
        break;

    /*
    case SYS_WRITE :
        get_argument(esp, arg, 3);
        break;
    */

    case SYS_CREATE :
        get_argument (esp, arg, 2);
        check_address (arg[0]);
        f->eax = create ((const char*)arg[0], (unsigned)arg[1]);
        break;

    case SYS_REMOVE :
        get_argument (esp, arg, 1);
        check_address (arg[0]);
        f->eax = remove ((const char*)arg[0]);
        break;

    case SYS_EXEC :
        get_argument (esp, arg, 1);
        check_address ((void*)arg[0]);
        f->eax = exec ((const char*)arg[0]);
        break;

  }

}


// 인자로 받아온 포인터 주소가 kernel영역 메모리 주소면 해당 프로세스를 종료시킨다.
void check_address (void *addr)
{
  if (!( (void*)USER_START <= addr && addr < (void*)KERNEL_START ))
  {
    printf ("invailed parameter.\n");
    exit (-1);
  }
}

/* 유저모드 스택에 저장된 시스템 콜 함수에 대한 인자를 커널 스택에 복사한다.
   esp 위치에 system call number가 저장되어 있고, esp+4 위치부터 인자들이 저장되어 있다. */
void get_argument (void *esp, int *arg, int count)
{
  int i;
  int *user_arg = (int*)esp + 1;
  
  for (i = 0; i < count; i++)
  {
    check_address (&user_arg[i]); // 유저모드 메모리 주소에서 넘어온 값이 맞는지 확인
    arg[i] = user_arg[i];
  }

}

// pintos를 종료하는 시스템 콜 함수
void halt ()
{
  shutdown_power_off ();     
}

// 현재 돌아가는 프로세스를 종료시키는 시스템 콜 함수
void exit (int status)
{
  struct thread *current_thread = thread_current ();
  current_thread->exit_status = status;
  printf ("process : %s exit(%d)\n", current_thread->name, status);
  thread_exit ();
}

// 디스크에서 파일을 생성하는 시스템 콜 함수
bool create (const char *file, unsigned initial_size)
{
  return filesys_create (file, initial_size);
}

// 디스크에서 파일을 지우는 시스템 콜 함수
bool remove (const char *file)
{
  return filesys_remove(file);
}

// 새로운 자식 프로세스를 생성하고 디스크에 있는 프로그램을 실행함
pid_t exec (const *cmd_line)
{
  struct thread *child = NULL;
  pid_t child_pid = 0;
  
  // cmd_line의 프로그램과 인자들을 실행할 자식 프로세스를 생성함.
  child_pid = process_execute (cmd_line);
  child = get_child_process (child_pid);
  
  if(!child->loaded) 
  {
    sema_down (&child->load_sema); // 자식 프로세스가 완전히 load 될 때까지 기다린다.
  } else if (child->exit_status == -1) { // 자식 프로세스가 비정상 종료된 경우 return -1를 함
 
    return -1;
  }
  
  return child_pid;

}





