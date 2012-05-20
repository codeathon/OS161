/*

	Ravi Patel and Rohit Ainapure

*/

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/seek.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <types.h>
#include <machine/trapframe.h>
#include <lib.h>
#include <current.h>
#include <synch.h>
#include <syscall.h>
#include <thread.h>
#include <spl.h>
#include <addrspace.h>
#include <copyinout.h>
#include <uio.h>
#include <vfs.h>
#include <vm.h>
#include <vnode.h>

///////////////////////////////////////////////

int
sys_open (const char *filename, int flags, mode_t mode)
{
	
	char *kbuf = NULL;
	openfile *file = NULL;
	int result;

	DEBUG(DB_SYSCALL, "Thread %u: sys_open\n", curthread -> pid);

	kbuf = (void *)kmalloc(32 + 1);
	result = copyin((userptr_t)filename, (void *)kbuf, 32 + 1);
	if (result) { errno = result; return (-1); }

        file = (openfile *)kmalloc(sizeof(openfile));
	KASSERT(file != NULL);

	file -> offset = 0;

	if (mode != O_RDWR || mode != O_WRONLY || mode != O_RDONLY) {
	        mode = O_RDWR;
	}
	file -> mode = mode;

        file -> lock = lock_create(kbuf);

	lock_acquire(file -> lock);
        result = vfs_open(kbuf, flags, mode, &(file -> vn));
 	lock_release(file -> lock);
        if (result) { errno = result; }

        result = ft_add(curthread -> ft, file, 0, &errno);
        if (errno) { result = errno; }

	kfree(kbuf);
        
        return (result);

}

int
sys_read (int fd, void *buf, size_t buflen)
{

	openfile *fd_read = NULL;
        struct uio read_data;
	struct iovec iov;
	void *kbuf = NULL;
	int result;

	DEBUG(DB_SYSCALL, "Thread %u: sys_read\n", curthread -> pid);

        if (buf == NULL) {
		errno = EFAULT;
		return (-1);
	}

	if (fd < 0 || fd >= MAX_FILES) {
		errno = EBADF;
		kfree(kbuf);
		return (-1);
	}

        fd_read = ft_get(curthread -> ft, fd);
        if (fd_read == NULL || fd_read -> mode == O_WRONLY) {
		errno = EBADF;
		return (-1);
        }

        kbuf = (void *)kmalloc(buflen + 1);
	uio_kinit(&iov, &read_data, kbuf, buflen, fd_read -> offset, UIO_READ);

        lock_acquire(fd_read -> lock);
        result = VOP_READ(fd_read -> vn, &read_data);
	lock_release(fd_read -> lock);
	if (result) { errno = result; return (-1); }

	fd_read -> offset = read_data.uio_offset;

	result = copyout((void *)kbuf, (userptr_t)buf, buflen - read_data.uio_resid + 1);
	if (result) { errno = result; return (-1); }

	return (buflen - read_data.uio_resid);
}

int
sys_write (int fd, void *buf, size_t buflen)
{

	openfile *fd_read = NULL;
        struct uio read_data;
        struct iovec iov;
        void *kbuf = NULL;
        int result;

        DEBUG(DB_SYSCALL, "Thread %u: sys_write .. ", curthread -> pid);

        if (buf == NULL) {
		errno = EFAULT;
		return (-1);
	}

	if (fd < 0 || fd >= MAX_FILES) {
		errno = EBADF;
		kfree(kbuf);
		return (-1);
	}

        fd_read = ft_get(curthread -> ft, fd);
        if (fd_read == NULL || fd_read -> mode == O_RDONLY) {
		errno = EBADF;
                kfree(kbuf);
                return (-1);
        }

        kbuf = (void *)kmalloc(buflen + 1);
	result = copyin((userptr_t)buf, (void *)kbuf, buflen + 1);
	if (result) { errno = result; kfree(kbuf); return (-1); }

	uio_kinit(&iov, &read_data, kbuf, buflen, fd_read -> offset, UIO_WRITE);
	read_data.uio_iov->iov_kbase = (userptr_t)buf;

        lock_acquire(fd_read -> lock);
        result = VOP_WRITE(fd_read -> vn, &read_data);
        lock_release(fd_read -> lock);

        kfree(kbuf);

        if (result) { errno = result; return (-1); }
        return (buflen - read_data.uio_resid);

}

int
sys_lseek (int fd, off_t pos, int whence)
{

	openfile *file = NULL;
        struct device *dev = NULL;
	void* kbuf;
        struct uio read_data;
        struct iovec iov;
        int result;

	DEBUG(DB_SYSCALL, "Thread %u: sys_lseek\n", curthread -> pid);

	if (fd < 0 || fd >= MAX_FILES) {
		errno = EBADF;
		return (-1);
	}

        file = ft_get(curthread -> ft, fd);
        if (file == NULL) {
		errno = EBADF;
                return (-1);
        }

        // get vnode to get file size
        dev = file -> vn -> vn_data;

        if (whence == SEEK_SET) {
		pos = pos;
	}
        else if (whence == SEEK_CUR) {
                pos += file -> offset;
	}
	else if (whence == SEEK_END) {
		kbuf = (void *)kmalloc(pos);
		uio_kinit(&iov, &read_data, kbuf, pos, file -> offset, UIO_READ);
		do {
			result = VOP_READ((file->vn), &read_data);
		}while(result > 0);
		pos += read_data.uio_resid;
	}
	else {
		errno = EINVAL;
		return (-1);
	}

	// trial seek
	lock_acquire(file -> lock);
        result = VOP_TRYSEEK(file -> vn, pos);
	lock_release(file -> lock);
        if (result != 0) { errno = result; return (-1); }

        // actual seek
        file -> offset = pos;
        return (pos);
}

int
sys_dup2 (int oldfd, int newfd)
{

	openfile *fl_old = NULL;
	openfile *fl_temp = NULL;
	openfile *fl_new = NULL;

	DEBUG(DB_SYSCALL, "Thread %u: sys_dup2\n", curthread -> pid);

	if (oldfd < 0 || oldfd >= MAX_FILES || newfd < 0 || newfd >= MAX_FILES) {
		errno = EBADF;
		return (-1);
	}

        fl_old = ft_get(curthread -> ft, oldfd);
	if (fl_old == NULL) { errno = EBADF; return (-1); }

        fl_temp = ft_get(curthread -> ft, newfd);
        if (fl_temp != NULL) {
		sys_close(newfd);
	}

        fl_new -> vn = fl_old -> vn;
        fl_new -> offset = fl_old -> offset;
        fl_new -> mode = fl_old -> mode;
        fl_new -> lock = fl_old -> lock;

        ft_set(curthread -> ft, newfd, fl_new);

        return (newfd);

}

int
sys_chdir (char *pathname)
{

	int result;

	DEBUG(DB_SYSCALL, "Thread %u: sys_chdir\n", curthread -> pid);

	result = vfs_chdir((char *)pathname);
	if (result) { errno = result; return (-1); }

	return (0);

}

int
sys_close (int fd)
{

	int result;

	DEBUG(DB_SYSCALL, "Thread %u: sys_close\n", curthread -> pid);

	if (fd < 0 || fd >= MAX_FILES) {
		errno = EBADF;
		return (-1);
	}

	result = ft_remove(curthread -> ft, fd);
	if (result) { errno = result; return (-1); }

        return (0);

}

int
sys___getcwd (char *buf, size_t buflen)
{

        (void)buf;
	(void)buflen;

	return (-1);

}

///////////////////////////////////////////////

pid_t
sys_fork (struct trapframe *tf)
{

	struct thread *child = NULL;
	char *name = NULL;
	int result;
   
	DEBUG(DB_SYSCALL, "Thread %u: sys_fork\n", curthread -> pid);

	name = kstrdup(curthread -> t_name);
	result = thread_fork2(name, enter_forked_process, (void *)tf, 1, &child);
	if (result) { errno = result; return (-1); }

	P(curthread -> forksem);

	return ((pid_t)curthread -> cpid);

}

int
sys_execv (char *program, char **args)
{

    int nargs = 0, i = 0;
    int result = 0;
    size_t len;
    struct vnode *v;
    vaddr_t entrypoint, stackptr;
	char *path = NULL;
    int test;
	char **run_args;
	userptr_t *usr_argv = NULL;
    int argIndex;

    if (program == NULL) {
        errno = EFAULT;
        return -1;
    }

    result = copyinstr((userptr_t)program, path, 1024, &len);
    if(result) {
        errno = EFAULT;
        return -1;
    }
    if (strlen(path)==0) {
        errno = EINVAL;
        return -1;
    }

    if (args == NULL) {
        errno = EFAULT;
        return -1;
    }


    result = copyin((userptr_t)args, &test, sizeof(int));
    if (result) {
        errno = EFAULT;
        return -1;
    }

    while(args[nargs] != NULL) {
        result = copyin((userptr_t)args + nargs*4, &test, sizeof(int));
        if (result) {
            errno = EFAULT;
            return -1;
        }

        nargs += 1;
    }
    run_args = kmalloc(sizeof(char)*nargs);
    for(i =0; i<nargs; i+=1) {
        run_args[i] = kmalloc(sizeof(char)*1024);
        result = copyinstr((userptr_t)args[i], run_args[i], 1024, &len);
        if(result) {
            errno = EFAULT;
            return -1;
        }
    }
    if (result) {
        errno = EFAULT;
        return -1;
    }

    as_destroy(curthread->t_addrspace);
    curthread->t_addrspace = NULL;

    /* Stuff from runprogram.c */

    /* Open the file. */
    result = vfs_open(program, O_RDONLY, 0, &v);
    if (result) {
        return result;
    }

    /* We should be a new thread. */
    KASSERT(curthread->t_addrspace == NULL);

    /* Create a new address space. */
    DEBUG(DB_SYSCALL,"Creating a new address space with as_create()");
    curthread->t_addrspace = as_create();
    if (curthread->t_addrspace==NULL) {
        vfs_close(v);
        return ENOMEM;
    }

    /* Activate it. */
    as_activate(curthread->t_addrspace);

    /* Load the executable. */
    result = load_elf(v, &entrypoint);
    if (result) {
        /* thread_exit destroys curthread->t_addrspace */
        vfs_close(v);
        return result;
    }

    /* Done with the file now. */
    vfs_close(v);

    /* Define the user stack in the address space */
    result = as_define_stack(curthread->t_addrspace, &stackptr);
    if (result) {
        /* thread_exit destroys curthread->t_addrspace */
        return result;
    }

    stackptr -= sizeof(char*) * (nargs + 1);

    usr_argv = (userptr_t*)stackptr;
    argIndex = 0;
    copyout(args, (userptr_t)usr_argv, sizeof(char*)*(nargs + 1));

    for(argIndex = 0; argIndex < nargs; argIndex += 1) {
        stackptr -= sizeof(char) * (strlen(args[argIndex]) + 1);
        usr_argv[argIndex] = (userptr_t)stackptr;
        copyout(args[argIndex], usr_argv[argIndex], sizeof(char) * (strlen(args[argIndex]) + 1) );

    }
    usr_argv[nargs] = 0;
    stackptr -= stackptr%8;

    /* Warp to user mode. */
    enter_new_process(nargs, (userptr_t)usr_argv, stackptr, entrypoint);

    /* enter_new_process does not return. */
    panic("enter_new_process returned\n");
    return EINVAL;

}

int
sys_waitpid (pid_t pid, int *returncode, int flags)
{

	struct thread *waitthread = NULL;
	int exit; int result;

	DEBUG(DB_SYSCALL, "Thread %u: sys_waitpid .. waiting on Thread %u \n", curthread -> pid, pid);

	if (pid <= 0 || pid >= MAX_PROCESSES) {
		errno = ESRCH;
		return (-1);
	}
	if (pid == curthread -> pid) {
		errno = ECHILD;
		return (-1);
	}

	if (flags != 0 && flags != WNOHANG) {
		errno = EINVAL;
		return (-1);
	}
	if (returncode == NULL) {
		errno = EFAULT;
		return (-1);
	}

	waitthread = pt_getthread(pid);
	if (waitthread == NULL) {
		exit = pt_getexitcode(pid);
		if (exit != -1) {
			result = copyout(&exit, (userptr_t)returncode, sizeof(int));
			if (result) { errno = result; return (-1); }
			return (pid);
		}
		errno = ESRCH;
		return (-1);
	}

	if (waitthread -> ppid != curthread -> pid) {
		errno = ECHILD;
		return (-1);
	}

	lock_acquire(waitthread -> waitlock); exit = 0;
	if (flags != WNOHANG) { waitthread -> waiters++; }
	while (exit == 0) {
		lock_release(waitthread -> waitlock);
		if (flags == WNOHANG) { return (0); }
		cv_wait(waitthread -> waitcv, waitthread -> waitlock);
		lock_acquire(waitthread -> waitlock);
		exit = pt_getexitcode(pid);
	}
	result = copyout(&exit, (userptr_t)returncode, sizeof(int));
	if (flags != WNOHANG) { waitthread -> waiters--; }
	if (result) { errno = result; lock_release(waitthread -> waitlock); return (-1); }
	lock_release(waitthread -> waitlock);
	cv_signal(waitthread -> exitcv, waitthread -> waitlock);

	return (pid);
	
}

pid_t 
sys_getpid (void)
{
	DEBUG(DB_SYSCALL, "Thread %u: sys_getpid\n", curthread -> pid);
	return ((pid_t)curthread -> pid);
}

void 
sys__exit (int exitcode)
{

	DEBUG(DB_SYSCALL, "Thread %u: sys_exit .. broadcasting, and exiting\n", curthread -> pid);
	
	curthread -> exited = 1;
	curthread -> exitcode = exitcode;
	cv_broadcast(curthread -> waitcv, curthread -> waitlock);
	lock_acquire(curthread -> waitlock);
	while (curthread -> waiters > 0) {
		lock_release(curthread -> waitlock);
		cv_wait(curthread -> exitcv, curthread -> waitlock);
		lock_acquire(curthread -> waitlock);
	}
	pt_recyclepid(curthread -> pid, exitcode);
	lock_release(curthread -> waitlock);
	thread_exit();

}
