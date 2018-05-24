#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "lib/string.h"
#include "vm/page.h"
#include "threads/vaddr.h"

/* 보다 직관적인 check_address() 를 작성하기 위해 
   각 메모리 영역 시작 주소 값을 USER_START, KERNEL_START로 정의함. */
#define USER_START      0x08048000
#define KERNEL_START    0xc0000000

#define STDIN   0
#define STDOUT  1


typedef int pid_t;
static void syscall_handler (struct intr_frame *);
struct vm_entry *check_address (void *addr);
void check_vaild_buffer (void *buffer, unsigned size, void *esp, bool to_write);
void get_argument (void *esp, int *arg, int count);
void halt ();
void exit (int status);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
pid_t exec (const *cmd_line);
int wait (tid_t tid);
int open (const char *file);
int filesize (int fd); 
int read (int fd, void *buffer, unsigned size);
int write (int fd, void *buffer, unsigned size);
void seek (int fd, unsigned position);
void close (int fd);
unsigned tell (int fd);

/* read() write() 시스템콜 호출 시 사용될 lock
   disk 같은 공유자원에 접근 할 때는
   critical section, mutex등으로 
   공유자원에 대한 동시 접근 보호가 필요함 */
struct lock rw_lock;


void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  
  /* 핀토스 부팅할 때 init()에 의해 rw_lock이 초기화 될 수 있도록 함 */
  lock_init (&rw_lock);
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

    case SYS_WAIT :
        get_argument (esp, arg, 1);
        f->eax = wait ((tid_t)arg[0]);
        break;

   /* 
    case SYS_WRITE :
        get_argument(esp, arg, 3);
        printf("%d %s %d\n", arg[0], arg[1], arg[2]);
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

     case SYS_OPEN :
        get_argument (esp, arg, 1);
        check_address ((void*)arg[0]);
        f->eax = open ((const char*)arg[0]);
        break;

     case SYS_FILESIZE :
        get_argument (esp, arg, 1);
        f->eax = filesize ((int)arg[0]);
        break;

     case SYS_READ :
        get_argument (esp, arg, 3);
        //check_address ((void*)arg[1]);
        check_vaild_buffer ((void*)arg[1], (unsigned)arg[2], esp, false);
        f->eax = read ((int)arg[0], (void*)arg[1], (unsigned)arg[2]);
        break;

     case SYS_WRITE :
        get_argument (esp, arg, 3);
        check_address ((void*)arg[1]);
        f->eax = write ((int)arg[0], (void*)arg[1], (unsigned)arg[2]);
        break;

     case SYS_SEEK :
        get_argument (esp, arg, 2);
        seek ((int)arg[0], (unsigned)arg[1]);
        break;

     case SYS_TELL :
        get_argument (esp, arg, 1);
        f->eax = tell ((int)arg[0]);
        break;

     case SYS_CLOSE :
        get_argument (esp, arg, 1);
        close ((int)arg[0]);
        break;

  }

}

/* 파일 디스크립터 사용이 끝나면 
   이 시스템콜을 호출하여 해당 fd의 file object를 해제할 수 있음 */
void close (int fd) {
  return process_close_file (fd);  
}

/* 파일 객체의 읽고 쓸 position이 어딘지 알려주는 시스템 콜 */
unsigned tell (int fd) {
  struct file *file_object = process_get_file (fd);
  
  if (file_object == NULL) {
    /* 유효하지 않은 파일 디스크립터거나 STDIN이거나 STDOUT인 경우 return -1
       만약 null값을 file_tell()에 넘겨서 호출할 경우 assertion발생*/
    return -1;
  } else {
    /* file_tell() 내부적으로 file_object->pos 값을 읽어와줌 */
    return file_tell (file_object);
  }

}

/* file object에서 pos멤버를 바꿔 
   다음에 read하거나 write할때 시작 위치를 바꿔주는 시스템콜  함수 */
void seek (int fd, unsigned position) {
  struct file *file_object = process_get_file (fd);
  
 if (file_object == NULL || position >= file_length (file_object)) {
   
    /* fd가 STDIN or STDOUT 이거나 파일 객체가 없거나
       position이 파일 크기를 벗어나면 그냥 리턴함 */ 
    return;
  } else {

    /* 내부적으로 파일 객체의 pos멤버를 position값을 바꾸도록 작동함 */
    file_seek (file_object, position);
    return position; 
    // file_seek() 함수가 따로 바꾼 위치 값을 리턴하지 않아서 position 인자값 그대로 사용함
  }
}


/* fd가 나타내는 파일에 내용을 입력할 수 있게 해주는 시스템콜 함수 */
int write (int fd, void *buffer, unsigned size) {
  struct file *file_object = process_get_file (fd);
  int bytes_write;
  
 
  if (fd == STDOUT) {
   
    /* 콘솔 화면에 buffer에 있는 내용을 size만큼 출력해줌
       근데 따로 출력해 준 size를 리턴해주지 않아서 size를 그대로 return 값으로 사용함*/
    putbuf(buffer, size);
    bytes_write = size;
  } else {
   
    lock_acquire (&rw_lock);
    //----------- start critical section ---------------
  
    /* file_write()가 해당 파일에서 buffer에다가 size만큼 읽어와서 읽은 byte수만큼 리턴해줌
       그럼 파일을 어디서 부터 읽느냐 그건 file object에 pos라는 읽을 위치를 저장해놓은 멤버가 있어서
       내부적으로 이 pos부터 시작해서 size만큼 읽음 */
    bytes_write = file_write(file_object, buffer, size);
    
    //------------ finish critical section -------------
    lock_release (&rw_lock);

 }
  
  return bytes_write;
}

/* fd가 나타내는 파일의 내용을 읽어주는 시스템콜 함수 */
int read (int fd, void *buffer, unsigned size) {
  struct file *file_object = process_get_file (fd);
  int bytes_read = 0; 
  int i;

  if (fd == STDIN) {
    for (i = 0; i < size; i++) {
     
      /* input_getc()가 사용자 입력이 없으면 
         생길때 까지 기다려서 받는 함수이기 때문에
         따로 EOF 이런 것을 고려할 필요가 없음
         but '\n' 개행문자 받으면 끊어 줘야함 */
      ((char*)buffer)[i] = input_getc ();
      if (((char*)buffer)[i] == '\n')
        break;
    }  

    // 항상 size와 같은 값이겠지만 buffer에 저장된 크기라는 종속적으로 결정될 값을 의도함.
    bytes_read = i + 1; 
  
  } else {
  /* 파일을 읽는 동안 다른 프로세스가 같은 파일에 접근하지 못하도록 lock을 걸음 */
    lock_acquire (&rw_lock);
    //---------- start critical section -----------

    /* 해당 파일에서 size크기 만큼 읽고 (혹은 EOF 읽는 지점까지) 
       buffer주소에 읽어온 내용을 저장하며
       읽은 byte 수를 반환함. */
    bytes_read = file_read (file_object, buffer, size); 
    //------------- finish critical section ----------------
    lock_release (&rw_lock);

 }

  // 읽어온 byte 수를 리턴함
  return bytes_read;

}

/* 파일 디스크립터에 해당하는 파일의 size를 리턴해줌 */
int filesize (int fd) {
  struct file *file_object = process_get_file (fd);
  
  /* file_length() 는 인자로 받아온 파일 객체 포인터가 null일 경우 assertion을 일으킴
     받아온 fd에 해당하는 파일 객체가 null일 경우 -1을 리턴할 수 있게 예외처리를 함*/
  if (file_object == NULL) {
    
    return -1;
  } else { 

    /* file_length() 내부적으로 
       file object->inode->inode_disk.length 를 
       참조하여 파일 사이즈를 가져옴 */
    return file_length (file_object);
  }
  
}


/* 파일 이름을 받으면 해당 파일 객체를 열어서 
   그 파일 객체를 usermode에서 간접적으로 지정할 수 있게 
   FDT에 추가하여 해당 FD를 리턴해주는 시스템 콜 함수
   
   (유저모드에서는 어차피 커널의 파일 객체에 직접 접근 못하니까
   파일을 fd를 통해 간접지정하면 커널에서 알아서 해석해서
   해당 객체를 참조해준다는 의미)  */
int open (const char *file) {
  
  struct file *file_object; 
  // open-null testcase를 통과하기 위한 예외처리
  if (file == NULL) {
    exit(-1);
  } else {
    /* filesys_open() 으로 해당 파일이름과 경로에 해당하는 파일을 열어서 파일객체를 반환함
       이 과정에서 해당 파일이 없거나 권한에 문제가 있어 열지 못한다면 null을 반환함 */
    file_object = filesys_open (file);
  }
  
  /* 해당 파일이 문제가 있어 null을 반환 받았다면 -1리턴 */
  if (file_object == NULL) {
    return -1;
  } else {
    /* 정상적으로 파일 객체를 반환 받았으면
       해당 FDT에 파일 객체 포인터를 추가하여 
       해당 FD number를 리턴함*/
    return process_add_file(file_object);
  }
}

/* user mode에서도 process_wait를 사용할 수 있도록 시스템 콜에 추가함 */
int wait(tid_t tid) 
{
  return process_wait(tid);
}


// 인자로 받아온 포인터 주소가 kernel영역 메모리 주소면 해당 프로세스를 종료시킨다.
struct vm_entry *check_address (void *addr)
{
  struct vm_entry *vme;
  if (!( (void*)USER_START <= addr && addr < (void*)KERNEL_START ))
  {
    //printf ("invailed parameter.\n");
    exit (-1);
  }
  
  /* addr이 vm_entry에 존재하면 vm_entry를 반환하도록 코드 작성 */
  /* find_vme() 사용*/ 
  vme = find_vme (addr);
  if (!vme)
    return NULL;
  
  return vme;

}


void check_vaild_buffer (void *buffer, unsigned size,
                         void *esp, bool to_write) {
  void *from = NULL, *to = NULL;
  void *vaddr = NULL;
  struct vm_entry *vme = NULL;
  unsigned number_of_page, i;

  /*  인자로 받은 buffer부터 buffer + size까지의 크기가 한 페이지의
      크기를 넘을 수도 있음 */
  /* 검사해야할 페이지 개수를 구함 */
  from = (unsigned)pg_round_down (buffer);
  to = (unsigned)pg_round_down ((unsigned)buffer + size - 1);
  num_of_page = ((to - from) >> PGBITS) + 1; 
  
  for (i = 0; i < num_of_page; i++) {
    /*  check_address를 이용해서 주소의 유저영역 여부를 검사함과 동시
        에 vm_entry 구조체를 얻음 */ 
    if (i != num_of_page - 1) {
      vaddr = (unsigned)buffer + (PGSIZE * i);
    } else {
      vaddr = (unsigned)buffer + size - 1; 
    }
    vme = check_address (vaddr);

    /*  해당 주소에 대한 vm_entry 존재여부와 vm_entry의 writable 멤
        버가 true인지 검사 */
    if (vme == NULL ||
        (to_write && vme->writable == false)) {
      exit (-1);
    }
  }
  /*  위 내용을 buffer 부터 buffer + size까지의 주소에 포함되는
      vm_entry들에 대해 적용 */
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
  printf ("%s: exit(%d)\n", current_thread->name, status);
  thread_exit ();
}

// 디스크에서 파일을 생성하는 시스템 콜 함수
bool create (const char *file, unsigned initial_size)
{
  // create-null testcase를 pass하기 위한 예외 처리
  if (file == NULL) {
    exit (-1);
  } else {
    return filesys_create (file, initial_size);
  }

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
  char *file_name = NULL;
  pid_t child_pid = 0;
  
  /* 유저 메모리 공간에 저장된 cmd_line을 직접 조작할 수 없으니 
     커널 메모리 공간에다가 복사함. 
     -> 커널 모드에서 유저 메모리 공간을 수정하려고 해서 생긴 문제라고 생각했는데
     알고보니까 const 를 수정하려고 해서 발생한 문제로 예상됨.*/
  file_name = palloc_get_page (0);
  if (file_name == NULL) {
    return -1;
  }
  
  strlcpy (file_name, cmd_line, strlen(cmd_line)+1);
    
  // cmd_line의 프로그램과 인자들을 실행할 자식 프로세스를 생성함.
  child_pid = process_execute (file_name);
  child = get_child_process (child_pid);
  
  sema_down (&child->load_sema); // 자식 프로세스가 완전히 load 될 때까지 기다린다.
  palloc_free_page (file_name);
  // 자식 프로세스가 비정상 종료된 경우 return -1를 함
  if (child->exit_status == -1) 
  {
    return -1;
  }
  
  return child_pid;

}





