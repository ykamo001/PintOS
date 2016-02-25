#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/init.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "devices/input.h"
#include "devices/shutdown.h"

static int sys_halt(void);
//static int sys_exit(int status);
static int sys_exec(const char *cmd_line);
static int sys_wait(tid_t proc_id);
static int sys_create(const char *file, unsigned initial_size);
static bool sys_remove(const char *file);
//static int sys_open(const char *file);
//static int sys_filesize(int fd);
//static int sys_read(int fd, void *buffer, unsigned size);
//static int sys_write(int fd, const void *buffer, unsigned size);
//static void sys_seek(int fd, unsigned position);
//static int sys_tell(int fd);
//static void sys_close(int fd);

static bool verify_user (const void *uaddr);
static void copy_in (void *dst_, const void *usrc_, size_t size);

static void syscall_handler (struct intr_frame *);
static struct lock file_sys_lock;

struct file_info
{
	struct list_elem elem;
	struct file *ptr_to_file;
	int holder;
};

void syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&file_sys_lock);
}

static void syscall_handler (struct intr_frame *f)
{
	typedef int function_to_call(int, int, int);
	
	struct function_info
	{
		size_t argc;
		function_to_call *ptr_to_func;
	};
	
	static const struct function_info table_of_funcs[] = 
	{
		{0, (function_to_call *) sys_halt},
		{1, (function_to_call *) sys_exit},
		{1, (function_to_call *) sys_exec},
		{1, (function_to_call *) sys_wait},
		{2, (function_to_call *) sys_create},
		{1, (function_to_call *) sys_remove},
		//{1, (function_to_call *) sys_open},
		//{1, (function_to_call *) sys_filesize},
		//{3, (function_to_call *) sys_read},
		//{3, (function_to_call *) sys_write},
		//{2, (function_to_call *) sys_seek},
		//{1, (function_to_call *) sys_tell},
		//{1, (function_to_call *) sys_close},
	};
	unsigned callNum;
	int args[3];
	const struct function_info *func_to_call;

	if(!verify_user(f->esp))
	{
		sys_exit(-1);
	}

	//##Get syscall number
	copy_in (&callNum, f->esp, sizeof callNum);
	//##Using the number find out which system call is being used
	if(callNum >= (sizeof(table_of_funcs)/sizeof(*table_of_funcs)))
	{
		thread_exit();
	}
	//if it is a proper call number, move the pointer to the proper function struct
	func_to_call = table_of_funcs + callNum;
	
	ASSERT (func_to_call->argc <= (sizeof(args)/sizeof(*args)));
	//initialize args to 0
	memset(args, 0, sizeof args);
	copy_in (args, (uint32_t *) f->esp + 1, sizeof *args * func_to_call->argc);
	//##Use switch statement or something and run this below for each
	//##Depending on the callNum...
	f->eax = func_to_call->ptr_to_func(args[0], args[1], args[2]);
}

/* Returns true if UADDR is a valid, mapped user address,
 *    false otherwise. */
static bool verify_user (const void *uaddr) 
{
	if(uaddr == NULL)
	{
		return false;
	}
	return (uaddr < PHYS_BASE && pagedir_get_page (thread_current ()->pagedir, uaddr) != NULL);
}

/* Copies a byte from user address USRC to kernel address DST.
 *    USRC must be below PHYS_BASE.
 *       Returns true if successful, false if a segfault occurred. */
static inline bool get_user (uint8_t *dst, const uint8_t *usrc)
{
	int eax;
	    asm ("movl $1f, %%eax; movb %2, %%al; movb %%al, %0; 1:"
	           : "=m" (*dst), "=&a" (eax) : "m" (*usrc));
	      return eax != 0;
}

static inline bool put_user(uint8_t *udst, uint8_t byte)
{
	int eax;
	asm ("movl $1f, %%eax; movb %b2, %0; 1:" : "=m" (*udst), "=&a" (eax) : "q" (byte));
	return eax != 0;
}

/* Copies SIZE bytes from user address USRC to kernel address
 *    DST.
 *       Call thread_exit() if any of the user accesses are invalid. */
static void copy_in (void *dst_, const void *usrc_, size_t size) 
{
	uint8_t *dst = dst_;
	const uint8_t *usrc = usrc_;
	for (; size > 0; size--, dst++, usrc++)
	{
		if (usrc >= (uint8_t *) PHYS_BASE || !get_user (dst, usrc))
		{
			thread_exit ();
		}
	}
}

/* Creates a copy of user string US in kernel memory
*and returns it as a page that must be freed with
*palloc_free_page().
* Truncates the string at PGSIZE bytes in size.
* Call thread_exit() if any of the user accesses are invalid. */
static char *copy_in_string (const char *us) 
{
	char *ks;
	size_t length;     
	
	ks = palloc_get_page (0);
	if (ks == NULL) 
		thread_exit ();
		
	for (length = 0; length < PGSIZE; length++)
	{
		if (us >= (char *) PHYS_BASE || !get_user (/*(uint8_t *)*/ (ks + length), /*(const uint8_t *)*/ us++)) 
		{
			palloc_free_page (ks);
			thread_exit ();
		}
		if (ks[length] == '\0')
			return ks;
	}
	ks[PGSIZE - 1] = '\0';
	return ks;
}

static int sys_halt(void)
{
	shutdown_power_off();
}

static int sys_exit(int status)
{
	thread_current()->waiter->exit_stat = status;
	thread_exit();
	NOT_REACHED();
}

static int sys_exec(const char *cmd_line)
{
	if((cmd_line == NULL) || !verify_user(cmd_line))
	{
		return -1;
	}
	char *string_to_page = copy_in_string(cmd_line);
	int pid;
	lock_acquire(&file_sys_lock);
	pid = process_execute(string_to_page);
	lock_release(&file_sys_lock);
	return pid;
}

static int sys_wait(tid_t proc_id)
{
	return process_wait(proc_id);
}

static int sys_create(const char *file, unsigned initial_size)
{
	bool check = verify_user(file);
	if(!check)
	{
		sys_exit(-1);
	}
	return filesys_create(file, initial_size);
}

static bool sys_remove(const char *file)
{
	bool check = verify_user(file);
	if(!check)
	{
		sys_exit(-1);
	}
	return filesys_remove(file);
}


static int sys_open(const char *file)
{
	bool check = verify_user(file);
	struct thread *t;
	if(!check)
	{
		sys_exit(-1);
	}
	char *string_to_page = copy_in_string(file);
	struct file_info *fd;
	int cur_stat = -1;

	fd = malloc(sizeof *fd);
	if(fd != NULL)
	{
		lock_acquire(file_sys_lock);
		fd->ptr_to_file = filesys_open(file);
		if(fd->ptr_to_file != NULL)
		{
			t = thread_current();
			fd->holder = t->
}
