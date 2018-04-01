#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

#define USER_START      0x08048000
#define KERNEL_START    0xc0000000

typedef int pid_t;
static void syscall_handler (struct intr_frame *);
void check_address(void *addr);
void get_argument(void *esp, int *arg, int count);
void halt();
void exit(int status);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
pid_t exec(const *cmd_line);


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

  check_address(esp);

  syscall_num = *esp;

  
  switch (syscall_num) 
  {

    case SYS_HALT :
        halt();
        break;

    case SYS_EXIT :
        get_argument(esp, arg, 1);
        exit(arg[0]);
        f->eax = arg[0];
        break;

    case SYS_WRITE :
        get_argument(esp, arg, 3);
        hex_dump(esp, esp, (int*)KERNEL_START - esp, true);
        hex_dump(arg, arg, 32, true);
        printf("debug : \narg[0] = %d\narg[1] = %s\n arg[2] = %d\n", arg[0], arg[1], arg[2]);
        break;

    case SYS_CREATE :
        get_argument(esp, arg, 2);
        check_address(arg[0]);
        f->eax = create((const char*)arg[0], (unsigned)arg[1]);
        break;

    case SYS_REMOVE :
        get_argument(esp, arg, 1);
        check_address(arg[0]);
        f->eax = remove((const char*)arg[0]);
        break;

    case SYS_EXEC :
        get_argument (esp, arg, 1);
        check_address ((void*)arg[0]);
        f->eax = exec ((const char*)arg[0]);
        break;

  }

  thread_exit ();
}

void check_address(void *addr)
{
  if (!( (void*)USER_START <= addr && addr < (void*)KERNEL_START ))
  {
    exit(-1);
  }
}

void get_argument(void *esp, int *arg, int count)
{
  int i;
  int *user_arg = (int*)esp + 5;
  
  for (i = 0; i < count; i++)
  {
    check_address(&user_arg[i]);
    arg[i] = user_arg[i];
  }

}

void halt()
{
  shutdown_power_off();     
}

void exit(int status)
{
  struct thread *current_thread = thread_current();
  current_thread->exit_status = status;
  printf("process : %s exit(%d)\n", current_thread->name, status);
  thread_exit();
}

bool create(const char *file, unsigned initial_size)
{
  return filesys_create(file, initial_size);
}

bool remove(const char *file)
{
  return filesys_remove(file);
}

pid_t exec(const *cmd_line)
{
  struct thread *child = NULL;
  pid_t child_pid = 0;

  child_pid = process_execute (cmd_line);
  child = get_child_process (child_pid);
  
  if(!child->loaded) 
  {
    sema_down (&child->load_sema);
  } else if (!child->exit_status) {
 
    return -1;
  }
  
  return child_pid;

}





