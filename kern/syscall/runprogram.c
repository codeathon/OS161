/* 
 * Although this file existed prior to asst2, it is almost completely
 * rewritten for SOL2:
 * 
 * sys_execv() added, along with its helper functions.
 * runprogram() was modified to use sys_execv's helper functions, and open the
 * std file descriptors if necessary (since file descriptors are *inherited*
 * with execv).
 */

#include <types.h>
#include <kern/unistd.h>
#include <limits.h>
#include <kern/errno.h>
#include <lib.h>
#include <synch.h>
#include <addrspace.h>
#include <thread.h>
#include <current.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include <copyinout.h>
#include <kern/fcntl.h>
#include <file.h>

/*
 * argvdata struct
 * temporary storage for argv, global and synchronized.  this is because
 * a large number of simultaneous execv's could bring the system to its knees
 * with a huge number of kmallocs (and even reasonable sized command lines
 * might not fit on the stack).
 */
struct argvdata {
	char *buffer;
	char *bufend;
	size_t *offsets;
	int nargs;
	struct lock *lock;
};

static struct argvdata argdata;

void
execv_bootstrap(void)
{
	argdata.lock = lock_create("arglock");
	if (argdata.lock == NULL) {
		panic("Cannot create argv data lock\n");
	}
}

void
execv_shutdown(void)
{
	lock_destroy(argdata.lock);
}

/*
 * copyin_args
 * copies the argv into the kernel space argvdata. 
 * read through the comments to see how it works.
 */
static
int
copyin_args(userptr_t argv, struct argvdata *ad)
{
	userptr_t argptr;
	size_t arglen;
	size_t bufsize, bufresid;
	int result;

	KASSERT(lock_do_i_hold(ad->lock));

	/* for convenience */
	bufsize = bufresid = ARG_MAX;

	/* reset the argvdata */
	ad->bufend = ad->buffer;

	/* loop through the argv, grabbing each arg string */
	for (ad->nargs = 0; ad->nargs <= NARG_MAX; ad->nargs++) {
	
		/* 
		 * first, copyin the pointer at argv 
		 * (shifted at the end of the loop)
		 */ 
		result = copyin(argv, &argptr, sizeof(userptr_t));
		if (result) {
			return result;
		}

		/* if the argptr is NULL, we hit the end of the argv */
		if (argptr == NULL) {
			break;
		}

		/* too many args? bail */
		if (ad->nargs >= NARG_MAX) {
			return E2BIG;
		}
		 
		/* otherwise, copyinstr the arg into the argvdata buffer */
		result = copyinstr(argptr, ad->bufend, bufresid, &arglen);
		if (result == ENAMETOOLONG) {
			return E2BIG;
		}
		else if (result) {
			return result;
		}
		
		/* got one -- update the argvdata and the local argv userptr */
		ad->offsets[ad->nargs] = bufsize - bufresid;
		ad->bufend += arglen;
		bufresid -= arglen;
		argv += sizeof(userptr_t);
	}

	return 0;
}

/*
 * copyout_args
 * copies the argv out of the kernel space argvdata into the userspace.
 * read through the comments to see how it works.
 */
static
int
copyout_args(struct argvdata *ad, userptr_t *argv, vaddr_t *stackptr)
{
	userptr_t argbase, userargv, arg;
	vaddr_t stack;
	size_t buflen;
	int i, result;

	KASSERT(lock_do_i_hold(ad->lock));

	/* we use the buflen a lot, precalc it */
	buflen = ad->bufend - ad->buffer;
	
	/* begin the stack at the passed in top */
	stack = *stackptr;

	/*
	 * copy the block of strings to the top of the user stack.
	 * we can do it as one big blob.
	 */

	/* figure out where the strings start */
	stack -= buflen;

	/* align to sizeof(void *) boundary, this is the argbase */
	stack -= (stack & (sizeof(void *) - 1));
	argbase = (userptr_t)stack;

	/* now just copyout the whole block of arg strings  */
	result = copyout(ad->buffer, argbase, buflen);
	if (result) {
		return result;
	}

	/*
	 * now copy out the argv itself.
	 * the stack pointer is already suitably aligned.
	 * allow an extra slot for the NULL that terminates the vector.
	 */
	stack -= (ad->nargs + 1)*sizeof(userptr_t);
	userargv = (userptr_t)stack;
	
  if (curthread->t_filetable == NULL) {
		result = filetable_init("con:", "con:", "con:");
		if (result) {
			return result;
		}
	}

	for (i = 0; i < ad->nargs; i++) {
		arg = argbase + ad->offsets[i];
		result = copyout(&arg, userargv, sizeof(userptr_t));
		if (result) {
			return result;
		}
		userargv += sizeof(userptr_t);
	}

	/* NULL terminate it */
	arg = NULL;
	result = copyout(&arg, userargv, sizeof(userptr_t));
	if (result) {
		return result;
	}

	*argv = (userptr_t)stack;
	*stackptr = stack;
	return 0;
}

/*
 * loadexec
 * common code for execv and runprogram: loading the executable
 */
static
int
loadexec(char *path, vaddr_t *entrypoint, vaddr_t *stackptr)
{
	struct addrspace *newvm, *oldvm;
	struct vnode *v;
	char *newname;
	int result;

	/* new name for thread */
	newname = kstrdup(path);
	if (newname == NULL) {
		return ENOMEM;
	}

	/* open the file. */
	result = vfs_open(path, O_RDONLY, (mode_t) 0, &v);
	if (result) {
		kfree(newname);
		return result;
	}

	/* make a new address space. */
	newvm = as_create();
	if (newvm == NULL) {
		vfs_close(v);
		kfree(newname);
		return ENOMEM;
	}

	/* replace address spaces, and activate the new one */
	oldvm = curthread->t_addrspace;
	curthread->t_addrspace = newvm;
	as_activate(curthread->t_addrspace);

	/* load the executable. if it fails, restore the old address space and
	 * activate it.
	 */
	result = load_elf(v, entrypoint);
	if (result) {
		vfs_close(v);
		curthread->t_addrspace = oldvm;
		as_activate(curthread->t_addrspace);
		as_destroy(newvm);
		kfree(newname);
		return result;
	}

	/* done with the file */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(curthread->t_addrspace, stackptr);
	if (result) {
		curthread->t_addrspace = oldvm;
		as_activate(curthread->t_addrspace);
		as_destroy(newvm);
		kfree(newname);
		return result;
	}

	/*
	 * wipe out old address space
	 *
	 * note: once this is done, execv() must not fail, because there's
	 * nothing left for it to return an error to.
	 */
	if (oldvm) {
		as_destroy(oldvm);
	}

	/*
	 * Now that we know we're succeeding, change the current thread's
	 * name to reflect the new process.
	 */
	kfree(curthread->t_name);
	curthread->t_name = newname;

	return 0;
}


/*
 * runprogram
 * load program "progname" and start running it in usermode.
 * does not return except on error.
 *
 * opens the std file descriptors if necessary.
 *
 * note -- the pathname must be mutable, since it passes it to loadexec which
 * passes it to vfs_open.
 */
int
runprogram(char *progname)
{
	vaddr_t entrypoint, stackptr;
	int argc;
	userptr_t argv;
	int result;

	/* we must be a user process thread */
	KASSERT(curthread->t_pid >= PID_MIN && curthread->t_pid <= PID_MAX);

	/* we should be a new thread. */
	KASSERT(curthread->t_addrspace == NULL);

	lock_acquire(argdata.lock);

	/* make up argv strings */

	if (strlen(progname) + 1 > ARG_MAX) {
		lock_release(argdata.lock);
		return E2BIG;
	}
	
	/* allocate the space */
	argdata.buffer = kmalloc(strlen(progname) + 1);
	if (argdata.buffer == NULL) {
		lock_release(argdata.lock);
		return ENOMEM;
	}
	argdata.offsets = kmalloc(sizeof(size_t));
	if (argdata.offsets == NULL) {
		kfree(argdata.buffer);
		lock_release(argdata.lock);
		return ENOMEM;
	}
	
	/* copy it in, set the single offset */
	strcpy(argdata.buffer, progname);
	argdata.bufend = argdata.buffer + (strlen(argdata.buffer) + 1);
	argdata.offsets[0] = 0;
	argdata.nargs = 1;

	/* load the executable. note: must not fail after this succeeds. */
	result = loadexec(progname, &entrypoint, &stackptr);
	if (result) {
		kfree(argdata.buffer);
		kfree(argdata.offsets);
		lock_release(argdata.lock);
		return result;
	}

	result = copyout_args(&argdata, &argv, &stackptr);
	if (result) {
		kfree(argdata.buffer);
		kfree(argdata.offsets);
		lock_release(argdata.lock);

		/* If copyout fails, *we* messed up, so panic */
		panic("execv: copyout_args failed: %s\n", strerror(result));
	}

	argc = argdata.nargs;

	/* free the space */
	kfree(argdata.buffer);
	kfree(argdata.offsets);

	lock_release(argdata.lock);

	/* warp to user mode. */
	enter_new_process(argc, argv, stackptr, entrypoint);

	/* md_usermode does not return */
	panic("md_usermode returned\n");
	return EINVAL; /* quell compiler warning */
}

/*
 * sys_execv
 * 1. copyin the program name
 * 2. copyin_args the argv
 * 3. load the executable
 * 4. copyout_args the argv
 * 5. warp to usermode
 */
int
sys_execv(userptr_t prog, userptr_t argv)
{
	char *path;
	vaddr_t entrypoint, stackptr;
	int argc;
	int result;

	path = kmalloc(PATH_MAX);

	/* get the filename. */
	result = copyinstr(prog, path, PATH_MAX, NULL);
	if (result) {
		kfree(path);
		return result;
	}

	/* get the argv strings. */

	lock_acquire(argdata.lock);
	
	/* allocate the space */
	argdata.buffer = kmalloc(ARG_MAX);
	if (argdata.buffer == NULL) {
		lock_release(argdata.lock);
		kfree(path);
		return ENOMEM;
	}
	argdata.offsets = kmalloc(NARG_MAX * sizeof(size_t));
	if (argdata.offsets == NULL) {
		kfree(argdata.buffer);
		lock_release(argdata.lock);
		kfree(path);
		return ENOMEM;
	}
	
	/* do the copyin */
	result = copyin_args(argv, &argdata);
	if (result) {
		kfree(path);
		kfree(argdata.buffer);
		kfree(argdata.offsets);
		lock_release(argdata.lock);
		return result;
	}

	/* load the executable. Note: must not fail after this succeeds. */
	result = loadexec(path, &entrypoint, &stackptr);
	if (result) {
		kfree(path);
		kfree(argdata.buffer);
		kfree(argdata.offsets);
		lock_release(argdata.lock);
		return result;
	}

	/* don't need this any more */
	kfree(path);

	/* send the argv strings to the process. */
	result = copyout_args(&argdata, &argv, &stackptr);
	if (result) {
		lock_release(argdata.lock);

		/* if copyout fails, *we* messed up, so panic */
		panic("execv: copyout_args failed: %s\n", strerror(result));
	}
	argc = argdata.nargs;

	/* free the argdata space */	
	kfree(argdata.buffer);
	kfree(argdata.offsets);
	
	lock_release(argdata.lock);

	/* warp to user mode. */
	enter_new_process(argc, argv, stackptr, entrypoint);

	/* md_usermode does not return */
	panic("md_usermode returned\n");
	return EINVAL;
}
