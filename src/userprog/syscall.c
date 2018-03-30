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
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

void halt(void)
{
	shutdown_power_off();
}

void exit(int status)
{
	/* 실행중인스레드구조체를가져옴
	struct thread *thread_current = thread_current(); */
	/* 스레드이름과 exit status 출력*/
	printf("thread name : %s, status : %d\n", thread_name(), status);
	/* 스레드종료*/
	thread_exit();
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
	//test
	case SYS_WRITE :
		get_argument((f->esp), arg, 3);
		printf("hi \n");
		printf("%d, %d, %d\n", arg[0], arg[1], arg[2]);
		break;
	default :
		printf("bye");
		thread_exit();

}
	//printf ("system call!\n");
  	//thread_exit ();
	
}





	



