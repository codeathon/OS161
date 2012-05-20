/*
 * File-related system call implementations.
 * New for SOL2.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/unistd.h>
#include <kern/fcntl.h>
#include <kern/seek.h>
#include <lib.h>
#include <synch.h>
#include <uio.h>
#include <thread.h>
#include <current.h>
#include <vfs.h>
#include <vnode.h>
#include <file.h>
#include <syscall.h>
#include <copyinout.h>

/*
 * sys_open
 * just copies in the filename, then passes work to file_open.
 */
int
sys_open(userptr_t filename, int flags, int mode, int *retval)
{
	char fname[PATH_MAX];
	int result;

	result = copyinstr(filename, fname, sizeof(fname), NULL);
	if (result) {
		return result;
	}

	return file_open(fname, flags, mode, retval);
}

/*
 * sys_read
 * translates the fd into its openfile, then calls VOP_READ.
 */
int
sys_read(int fd, userptr_t buf, size_t size, int *retval)
{
  struct iovec iov;
	struct uio useruio;
	struct openfile *file;
	int result;

	/* better be a valid file descriptor */
	result = filetable_findfile(fd, &file);
	if (result) {
		return result;
	}

	lock_acquire(file->of_lock);

	if (file->of_accmode == O_WRONLY) {
		lock_release(file->of_lock);
		return EBADF;
	}

	/* set up a uio with the buffer, its size, and the current offset */
  uio_uinit(&iov, &useruio, buf, size, file->of_offset, UIO_READ);
  
  // mk_useruio(&useruio, buf, size, file->of_offset, UIO_READ);

	/* does the read */
	result = VOP_READ(file->of_vnode, &useruio);
	if (result) {
		lock_release(file->of_lock);
		return result;
	}

	/* set the offset to the updated offset in the uio */
	file->of_offset = useruio.uio_offset;

	lock_release(file->of_lock);
	
	/*
	 * The amount read is the size of the buffer originally, minus
	 * how much is left in it.
	 */
	*retval = size - useruio.uio_resid;

	return 0;
}

/*
 * sys_write
 * translates the fd into its openfile, then calls VOP_WRITE.
 */
int
sys_write(int fd, userptr_t buf, size_t size, int *retval)
{
  struct iovec iov;
	struct uio useruio;
	struct openfile *file;
	int result;

	result = filetable_findfile(fd, &file);
	if (result) {
		return result;
	}

	lock_acquire(file->of_lock);

	if (file->of_accmode == O_RDONLY) {
		lock_release(file->of_lock);
		return EBADF;
	}

	/* set up a uio with the buffer, its size, and the current offset */
  uio_uinit(&iov, &useruio, buf, size, file->of_offset, UIO_WRITE);
	
  //mk_useruio(&useruio, buf, size, file->of_offset, UIO_WRITE);

	/* does the write */
	result = VOP_WRITE(file->of_vnode, &useruio);
	if (result) {
		lock_release(file->of_lock);
		return result;
	}

	/* set the offset to the updated offset in the uio */
	file->of_offset = useruio.uio_offset;

	lock_release(file->of_lock);

	/*
	 * the amount written is the size of the buffer originally,
	 * minus how much is left in it.
	 */
	*retval = size - useruio.uio_resid;

	return 0;
}

/* 
 * sys_close
 * just pass off the work to file_close.
 */
int
sys_close(int fd)
{
	return file_close(fd);
}

/*
 * sys_lseek
 * translates the fd into its openfile, then based on the type of seek,
 * figure out the new offset, try the seek, if that succeeds, update the
 * openfile.
 */
int
sys_lseek(int fd, off_t offset, int32_t whence, off_t *retval)
{
	struct stat info;
	struct openfile *file;
	int result;

	result = filetable_findfile(fd, &file);
	if (result) {
		return result;
	}

	lock_acquire(file->of_lock);
	
	/* based on the type of seek, set the retval */ 
	switch (whence) {
	    case SEEK_SET:
		*retval = offset;
		break;
	    case SEEK_CUR:
		*retval = file->of_offset + offset;
		break;
	    case SEEK_END:
		result = VOP_STAT(file->of_vnode, &info);
		if (result) {
			lock_release(file->of_lock);
			return result;
		}
		*retval = info.st_size + offset;
		break;
	    default:
		lock_release(file->of_lock);
		return EINVAL;
	}

	/* try the seek -- if it fails, return */
	result = VOP_TRYSEEK(file->of_vnode, *retval);
	if (result) {
		lock_release(file->of_lock);
		return result;
	}
	
	/* success -- update the file structure */
	file->of_offset = *retval;

	lock_release(file->of_lock);

	return 0;
}

/* 
 * sys_dup2
 * just pass the work off to the filetable
 */
int
sys_dup2(int oldfd, int newfd, int *retval)
{
	int result;

	result = filetable_dup2file(oldfd, newfd);
	if (result) {
		return result;
	}

	*retval = newfd;
	return 0;
}

/* really not "file" calls, per se, but might as well put it here */

/*
 * sys_chdir
 * copyin the path and pass it off to vfs.
 */
int
sys_chdir(userptr_t path)
{
	char pathbuf[PATH_MAX];
	int result;
	
	result = copyinstr(path, pathbuf, PATH_MAX, NULL);
	if (result) {
		return result;
	}

	return vfs_chdir(pathbuf);
}

/*
 * sys___getcwd
 * just use vfs_getcwd.
 */
int
sys___getcwd(userptr_t buf, size_t buflen, int *retval)
{
  struct iovec iov;
	struct uio useruio;
	int result;
  
  uio_uinit(&iov, &useruio, buf, buflen, 0, UIO_READ);

	// mk_useruio(&useruio, buf, buflen, 0, UIO_READ);

	result = vfs_getcwd(&useruio);
	if (result) {
		return result;
	}

	*retval = buflen - useruio.uio_resid;

	return 0;
}

void*
sys_sbrk(intptr_t amount) 
{
	vaddr_t bot, top;
	struct vm_object *vmo = NULL;

	vmo = array_get(curthread->t_addrspace->as_objects);


}
