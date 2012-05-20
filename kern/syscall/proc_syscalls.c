/*
 * Process-related syscalls.
 * New for SOL2.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <pid.h>
#include <clock.h>
#include <syscall.h>
#include <copyinout.h>
#include <machine/trapframe.h>
#include <kern/wait.h>

/* note that sys_execv is defined in runprogram.c for convenience */

/*
 * sys_getpid
 * love easy syscalls. :)
 */
int
sys_getpid(pid_t *retval)
{
	*retval = curthread->t_pid;
	return 0;
}

/*
 * sys__exit
 * Just do thread_exit, which does all the work.
 */
void
sys__exit(int status)
{
	thread_exit(status);
}

/*
 * sys_fork
 * 
 * create a new process, which begins executing in child_thread().
 */

static
void
child_thread(void *vtf, unsigned long junk)
{
	struct trapframe mytf;
	struct trapframe *ntf = vtf;

	(void)junk;

	/*
	 * Now copy the trapframe to our stack, so we can free the one
	 * that was malloced and use the one on our stack for going to
	 * userspace.
	 */

	mytf = *ntf;
	kfree(ntf);

	enter_forked_process(&mytf);
}

int
sys_fork(struct trapframe *tf, pid_t *retval)
{
	struct trapframe *ntf;
	int result;

	/*
	 * Copy the trapframe to the heap, because we might return to
	 * userlevel and make another syscall (changing the trapframe)
	 * before the child runs. The child will free the copy.
	 */

	ntf = kmalloc(sizeof(struct trapframe));
	if (ntf==NULL) {
		return ENOMEM;
	}
	*ntf = *tf;

	result = thread_fork(curthread->t_name, child_thread, ntf, 0, retval);

	if (result) {
		kfree(ntf);
		return result;
	}

	return 0;
}

/*
 * sys_waitpid
 * just pass off the work to the pid code.
 */
int
sys_waitpid(pid_t pid, userptr_t retstatus, int flags, pid_t *retval)
{
	int status; 
	int result;

	result = pid_wait(pid, &status, flags, retval);
	if (result) {
		return result;
	}
  status = _MKWAIT_EXIT(status);	
	return copyout(&status, retstatus, sizeof(int));
}
