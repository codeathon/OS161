/*
 * Declarations for file handle and file table management.
 * New for SOL2.
 */

#ifndef _FILE_H_
#define _FILE_H_

#include <limits.h>

struct lock;
struct vnode;

/*** openfile section ***/

/* 
 * openfile struct 
 * note that there's not too much to keep track of, since the vnode does most
 * of that.  note that it does require synchronization, because a single
 * openfile can be shared between processes (filetable inheritance).
 */
struct openfile {
	struct vnode *of_vnode;
	
	struct lock *of_lock;
	off_t of_offset;
	int of_accmode;	/* from open: O_RDONLY, O_WRONLY, or O_RDWR */
	int of_refcount;
};

/* opens a file (must be kernel pointers in the args) */
int file_open(char *filename, int flags, int mode, int *retfd);

/* closes a file */
int file_close(int fd);


/*** file table section ***/

/*
 * filetable struct
 * just an array of open files.  nice and simple.  doesn't require
 * synchronization, because a table can only be owned by a single process (on
 * inheritance in fork, the table is copied).
 */
struct filetable {
	struct openfile *ft_openfiles[OPEN_MAX];
};

/* these all have an implicit arg of the curthread's filetable */
int filetable_init(const char *inpath, const char *outpath, 
		   const char *errpath);
int filetable_copy(struct filetable **copy);
int filetable_placefile(struct openfile *file, int *fd);
int filetable_findfile(int fd, struct openfile **file);
int filetable_dup2file(int oldfd, int newfd);
void filetable_destroy(struct filetable *ft);

#endif /* _FILE_H_ */
