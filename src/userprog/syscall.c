#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include <filesys/filesys.h>
#include <devices/shutdown.h>

static void syscall_handler (struct intr_frame *);

//idt
void
syscall_init (void) 
{
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

int open(const char *file)
{
	struct file *f = filesys_open(file); //실패할 경우 NULL을 리턴
	int fd;

	if(file ==NULL) {
		exit(-1);
	}
	if (!f) //case f == NULL
		return -1;
	fd = process_add_file(f);
	if (fd == -1)
		return -1;
	return fd;
}

int filesize(int fd)
{
	struct file *f = process_get_file(fd);
	if (!f)
		return -1;
	return file_length(f);
}

int read(int fd, void *buffer, unsigned size)
{
	//fd -> file pointer !  
	struct file *f = process_get_file(fd);
	int bytes_read;

	lock_acquire(&filesys_lock);	
	if(f) {
		//파일에 내용 쓰는함수
		bytes_read = file_read(f, buffer, size);
		lock_relase(&filesys_lock);
		return bytes_read;
	} 
	// f가 널이여도 단순히 -1 로 반환해주면안됨.
	//fdt 에서 0인 파일은 파일 사이즈가 없음, stdin.stdout은 filesize가 0이므로 -1 리턴 하는게 맞음. 그렇지만 read함수에서는 읽고 쓸수있게 작업을 해줘야함.
	else if(fd == 0) {
		int i;
		//input_getc()가 개행문자열을 가져온다 하면 브레키크 해서 멈춰야함. 
		for (i=0; i<size; i++) {
			burffer[i] = input_getc();
			if(input_getc() == "\0") {
				i++;
				break;
			}
		}
		lock_relase(&filesys_lock);
		return i;
	}
}

int write(int fd, void *buffer, unsigned size)
{
	struct file *f = process_get_file(fd);
	int bytes_write;

	lock_acquire(&filesys_lock);	
	if(f) {
		bytes_write = file_write(f, buffer,size);
		lock_release(&filesys_lock);
		return bytes_write;
	} 
	else if(fd == 1) {
		putbuf(buffer,size);
		lock_release(&filesys_lock);
		return size;	
	}
	
}

void seek (int fd, unsigned position)
{
	struct file *f = process_get_file(fd);
	file_seek(f, position);
}
unsigned tell(int fd)
{
	struct file *f = process_get_file(fd);
	file_tell(f);
}

void close(int fd)
{
	process_close_file(fd);
}

//
void halt(void)
{
	shutdown_power_off();
}

void exit(int status)
{
	struct thread *cur = thread_current();
	cur->exit_status = status;

	/* 실행중인스레드구조체를가져옴
	struct thread *thread_current = thread_current(); */
	/* 스레드이름과 exit status 출력*/
	printf("thread name : %s, status : %d\n", thread_name(), status);
	/* 스레드종료*/
	thread_exit();
}

void wait(tid_t pid)
{
	/* process_wait()사용, 자식프로세스가종료될때까지대기*/	
	process_wait(pid);
}

bool create(const char *file , unsigned initial_size)
{
	/* 파일이름과크기에해당하는파일생성*/
	/* 파일생성성공시true 반환, 실패시false 반환*/
	return filesys_create(file, initial_size);
}

bool remove(const char*file)
{
	/* 파일이름에해당하는파일을제거*/
	/* 파일제거성공시true 반환, 실패시false 반환*/
	return filesys_remove(file);
}

void check_address(void*addr)
{
	/* 주소값이유저영역에서사용하는주소값인지확인하는함수		
	유저영역을벗어난영역일경우프로세스종료(exit(-1)) */
	if(addr > 0xC0000000 || addr < 0x8048000)
		exit(-1);
}

void get_argument(void* esp, int* arg, int count)
{
	/* 유저스택에 있는 인자들을 커널에 저장하는함수
	스택포인터(esp)에 count(인자의개수) 만큼의 데이터를 arg에저장*/
	int i;
	check_address(esp);
	for(i=0; i<count; i++)
	{
		arg[i]=*(int*)(esp+(i+1)*4);
	}
}

tid_t exec(const char *cmd_line)
{
  struct thread *child = NULL;
  pid_t child_pid = 0;
  
  // cmd_line의 프로그램과 인자들을 실행할 자식 프로세스를 생성함.
  child_pid = process_execute (cmd_line);
	// 커널은 생성된 자식프로세스의 pid에 해당하는 pcb를 찾고, pcb의 세마포어에 접근해야함.
  child = get_child_process(child_pid);
  
	// 자식 프로세스로 실행흐름이 변경되어, 자식프로세스가 schedule()함수를 호출할동안 READY상태로 대기한다
	sema_down (&child->load_sema); 
	if (&child->exit_status == -1) { // 여기서 그냥 else나 else(child->loaded)하게되면,
																	 // 로드가 다 되어서 true로 바뀌었을때도 return -1을 하게됨.
    return -1;
  }
  
	return child_pid;


}

static void
syscall_handler (struct intr_frame *f) 
{
	//주소 인데, 무슨 type의 주소인지 모르므로(void *esp이므로), 'int타입의 주소'라고 알려줌
	int sys_num = *(int*)(f->esp); //syscall.c(lib.c같은)의 유저스텍의 esp
	int arg[5];

	check_address(f->esp);

	switch(sys_num)
{
	case SYS_HALT :
		halt();
		break;

	case SYS_EXIT :
		get_argument((f->esp), arg, 1);		
		exit(arg[0]);
		break;

	case SYS_CREATE :
		get_argument((f->esp), arg, 2);
		create((const char*)arg[0],(unsigned)arg[1]); 
		//arg[0]에 저장되어있는 주소가 int 값으로 저장되어 있기 때문에 형변환을 해줌
		break;

	case SYS_REMOVE :
		get_argument((f->esp), arg, 1);
		remove((const char*)arg[0]);
		break;

	//for test
	case SYS_WRITE :
		//get_argument((f->esp), arg, 3);
		//printf("hi \n");
		//printf("%d, %d, %d\n", arg[0], arg[1], arg[2]);
		printf ("system call!\n");
		break;
	case SYS_EXEC :
		get_argument((f->esp), arg, 1);
		exec((const char *)arg[0]);
		break;
	default :
		//printf("bye");
		thread_exit();

}
	//printf ("system call!\n");
  	//thread_exit ();
	
}





	



