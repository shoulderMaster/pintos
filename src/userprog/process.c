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
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#define MAX_FDT 64

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

int process_add_file(struct file *f)
{
	struct thread *t = thread_current();
	int i;
	
	for(i = (t->next_fd)%MAX_FDT; i < MAX_FDT; i++) 
		t->next_fd = i;
		if(t->fdt[i] == NULL) {
			t->fdt[t->next_fd++] = f;
			return i-1;	
		}
		return -1; //return NULL;??
	}
}

struct file* process_get_file(int fd)
{
	struct thread *t = thread_current();
	if(t->fdt[fd] == NULL)
		return NULL;
	return t->fdt[fd];
}

void process_close_file(int fd)
{
	/* fd에 해당하는파일을 file_close()를 호출하여 inode reference count를 1감소*/
	struct file *f = process_get_file(fd);
	file_close(f);
	/* 파일디스크립터테이블 해당엔트리를 NULL값으로 초기화*/
	f = NULL;
	thread_current()->next_fd = fd;
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy, *save_ptr;
  tid_t tid;
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  strtok_r(file_name, " ", &save_ptr);
	

  /* Create a new thread to execute FILE_NAME. */
	//question token[0] addres??
  tid = thread_create (file_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR)
    palloc_free_page (fn_copy); 
  return tid;
}

// 자식프로세스 디스크립터의 주솟값 리턴하는 함수
struct thread *get_child_process(int pid)

{
  struct thread *cur = thread_current();
	struct thread *tmp_t;
  struct list_elem *elem;
  
  //'자식 프로세스 리스트'의 head.next부터 tail.prev 까지 돌면서 pid(tid)를 검색함. 
  for (elem = cur->child_list.head.next; elem != cur->child_list.tail.prev; elem = elem->next)
  {
		//list_entry() : list_elem을 인자로 elem이포함된 struct의 포인터를반환한다.
    tmp_t = list_entry (elem, struct thread, child_elem);
    if (pid ==  tmp_t->tid)
      {     
        // 해당하는 pid를 찾으면 해당자식 프로세스의 PCB의 주솟값 리턴.
        return tmp_t;
      }
  }
  return NULL;
}

// 자식 프로세스 디스크립터를 삭제하는 함수
void remove_child_process(struct thread *cp)
{
  //list_remove (&cp->parent->child_list, &cp->child_elem);
	list_remove (&cp->child_elem);
	palloc_free_page(cp);
}

void argument_stack(char **parse ,int count , void **esp)
{ 
	int i, align_size;
	void *arg_ptr[count];

	for (i = count -1; i>=0; i--)
	{
		*esp -= strlen(parse[i])+1;
		arg_ptr[i] = *esp;
		strlcpy(*esp, parse[i], strlen(parse[i])+1);
	}
  
  // bit mask
	// 0x3 -> 0011 얘랑 esp랑 AND연산한 값만큼 패딩해주면
	// esp주소가 32bit 단위로 addressing가능해져서 memory I/O효율이 유리해짐.  
    align_size = (((int)*esp)&0x00000003);
    *esp -= align_size; //(뒤의 2자리를 00으로만듬)
    memset(*esp, 0x00, align_size);
	
	// unit8_t is 1byte. padding
	// *esp -= sizeof(uint8_t); *(uint8_t*)*esp = 0;
    
	*esp -= sizeof(char *);
	*(char **)*esp = 0;
    
	for(i = count-1; i >= 0; i--)
	{
		*esp -= sizeof(char *);
		*(char **)*esp = arg_ptr[i];
	// *(char **)*esp = parse[i]; 로 쓰면 안되는 이유는
	// (포인터 배열 하나 더만들어지는이유는)
	// esp는 유저 모드 스텍인데, (arg_ptr에는 esp주소 담아놓음) 
	// parse는 커널안에서 만들어진 동적배열이기때문에,
	// echo프로그램이 주소 접근 불가(커널영역)
	// 그래서 스택의 주소를 복사해놈	
	}

	*esp -= sizeof(char **);
	*(char ***)*esp = *esp + sizeof(char **);

	*esp -= sizeof(int);
	*(int *)*esp = count;

	*esp -= sizeof(void *);
	*(void **)*esp = 0;

  /* 프로그램이름및인자(문자열) 저장*/
  /* word-align */
  /* 프로그램이름및인자를가리키는주소저장*/
  /* argv(문자열을가리키는주소들의배열을가리킴) */
  /* argc(문자열의개수저장) */
  /* fake address(0) 저장*/

}


/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  char *file_name = file_name_;
  char *token, *save_ptr;
	char **parse;
  int token_count = 0;
	int i;

	file_name = palloc_get_page (0);
	if (file_name == NULL)
		thread_exit ();

	//file_name = file_name_;라고 하면 포인터 바껴서 
	//palloc_free_page할때 메모리 누수가능성있음.
	//동적 할당 했던 주소를 free 해줘야 하는데 다른데를 해제 하려고 해서 ERROR
	strlcpy (file_name, file_name_, PGSIZE);
	strtok_r(file_name, " ", &save_ptr);

  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);

  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  //디스크에서 echo에 해당하는 실행파일을 찾아서 메모리에 적재를하고, 
  //그 실행파일의 메인함수로 점프를 해주는 것임. 
	success = load(file_name, &if_.eip, &if_.esp);
	thread_current()->loaded = success;

	// 대기리스트에 스레드가 존재하면 리스트 맨 처음에 위치한 스레드를
	// THREAD_READY 상태로 변경후 schedule()호출
	// parent : BLOKED -> READY 
	sema_up(&thread_current()->load_sema);

	token_count++;
	while(strtok_r(NULL, " ", &save_ptr))
	{
		token_count++;
	}

	parse = ((char**)malloc((sizeof(char*))*token_count));
	strlcpy (file_name, file_name_, PGSIZE);

	for(token = strtok_r(file_name, " ", &save_ptr), i = 0; token != NULL; token 		= strtok_r(NULL, " ", &save_ptr), i++)
	{
		parse[i] = token;
	}
	
	//hex_dump((uintptr_t)if_.esp, if_.esp, (size_t)(PHYS_BASE-if_.esp), true);

  /* If load failed, quit. */
  palloc_free_page (file_name);
  if (!success) {
		thread_current()->exit_status = -1;
    thread_exit ();
	} else {
    thread_current()->exit_status = 0;
    thread_current()->exited = false;
	
		argument_stack(parse, token_count, &if_.esp);	
	}

  /* Start the user process by simulating a return from an	
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
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

	/* 자식프로세스의 프로세스 디스크립터 검색
		 예외처리 발생시-1 리턴 */
	if(!child = get_child_process(child_pid))
		return -1;

	/* 자식프로세스가 종료될때까지 부모프로세스대기(세마포어이용) */
	if (!child->exited)
		sema_down(&child->exit_sema);

	/* 자식프로세스디스크립터삭제*/
	status = child->exit_status;
	remove_child_process(child);

	/* 자식프로세스의 exit status 리턴*/
	return status;

}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
	
	
	//file_allow_write(cur->run_file);
	

	/* 프로세스에 열린 모든파일을 process_close_file(int fd)함수를 호출하여 
	inode referece 값 1감소
	process_close_file(int fd) : 파일디스크립터에 해당하는 파일을 닫고 해당 엔트리초기화
	파일디스크립터 테이블의 최대값을 이용해 파일디스크립터의 최소값인2가될때까지 파일을닫음*/
	int i;	
	for (i =2; < i<MAX_FDT; i++) {
		process_close_file(i);
	}
	/* 파일디스크립터테이블메모리해제*/
  free(cur->fdt);
	
	/* 실행중인 파일close*/
	if(cur->run_file != NULL)	
		file_close(cur->run_rile); //file_close에 file_allow_write()있음.


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

	/* filesys_lock획득, denying write를 제대로 해주기위한 lock.*/
	lock_acquire(filesys_lock);

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

	/* thread 구조체의run_file을 현재실행할파일로초기화*/
	t->run_file = file;
	/* file_deny_write()를이용하여파일에대한write를거부*/
	file_deny_write(t->run_file);
	/* filesys_lock해제*/
	lock_release(filesys_lock);

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
  file_close (file);
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
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
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

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

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
