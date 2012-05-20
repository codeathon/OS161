/*

 	Ravi Patel and Rohit Ainapure

 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/seek.h>
#include <kern/unistd.h>
#include <filetable.h>
#include <lib.h>
#include <vfs.h>
#include <vnode.h>
#include <synch.h>

filetable *
ft_create (void)
{

	int i;
        filetable *ft = NULL;

        ft = (filetable *)kmalloc(sizeof(filetable));
        KASSERT(ft != NULL);

	for (i = 0; i < MAX_FILES; i++) {
		ft -> handles[i] = NULL;
	}

        ft -> last = 0;
	ft -> lock = lock_create("filetable");

        return (ft);

}

int
ft_init (filetable *ft)
{

        struct vnode *v = NULL;
	openfile *fd_IN = NULL;
	openfile *fd_OUT = NULL;
	openfile *fd_ERR = NULL;
	char *con1 = NULL;
	char *con2 = NULL;
	char *con3 = NULL;
        int result;

        KASSERT(ft != NULL);

        // STDIN //

        fd_IN = (openfile *)kmalloc(sizeof(openfile));
        KASSERT(fd_IN != NULL);

	con1 = kstrdup("con:");
        result = vfs_open(con1, O_RDONLY, 0, &v);
        if (result) {
                vfs_close(v);
                ft_destroy(ft);
                return (-1);
        }
        fd_IN -> vn = v;
        fd_IN -> offset = 0;
        fd_IN -> mode = O_RDONLY;
        fd_IN -> refs = 0;
        fd_IN -> lock = lock_create("stdin");
        ft_add(ft, fd_IN, 1, NULL);

        // STDOUT //

        fd_OUT = (openfile *)kmalloc(sizeof(openfile));
        KASSERT(fd_OUT != NULL);

	con2 = kstrdup("con:");
        result = vfs_open(con2, O_WRONLY, 0, &v);
        if (result) {
                vfs_close(v);
                ft_destroy(ft);
                return (-1);
        }
        fd_OUT -> vn = v;
        fd_OUT -> offset = 0;
        fd_OUT -> mode = O_WRONLY;
        fd_OUT -> refs = 0;
        fd_OUT -> lock = lock_create("stdout");
        ft_add(ft, fd_OUT, 1, NULL);

        // STDERR

        fd_ERR = (openfile *)kmalloc(sizeof(openfile));
        KASSERT(fd_ERR != NULL);

	con3 = kstrdup("con:");
        result = vfs_open(con3, O_WRONLY, 0, &v);
        if (result) {
                vfs_close(v);
                ft_destroy(ft);
                return (-1);
        }
        fd_ERR -> vn = v;
        fd_ERR -> offset = 0;
        fd_ERR -> mode = O_WRONLY;
        fd_ERR -> refs = 0;
        fd_ERR -> lock = lock_create("stderr");
        ft_add(ft, fd_ERR, 1, NULL);

	kfree(con1); kfree(con2); kfree(con3);
        return (0);

}

int
ft_add (filetable *ft, openfile *file, int init, int *err)
{

	unsigned int cntr;

        KASSERT(ft != NULL);
	KASSERT(file != NULL);

	if (init == 0) { ft_init(ft); }

	lock_acquire(ft -> lock);

	cntr = 0;
	while (ft -> handles[ft -> last] != NULL) {

		ft -> last++;
		if (ft -> last == MAX_FILES) { ft -> last = 0; }

		cntr++;
		if (cntr >= MAX_FILES) { *err = EMFILE; }

	}

	ft -> handles[ft -> last] = file;
	ft -> handles[ft -> last] -> refs++;

	lock_release(ft -> lock);

        return (ft -> last);

}

int
ft_remove (filetable *ft, unsigned int fd)
{

        openfile *file = NULL;

        KASSERT(ft != NULL);
	KASSERT(fd < MAX_FILES);

	lock_acquire(ft -> lock);

        file = ft -> handles[fd];
	if (file != NULL) {
		file -> refs -= 1;
	        if (file -> refs == 0) {
	                vfs_close(file -> vn);
	                kfree(file);
		}
		ft -> handles[fd] = NULL;
		if (fd < ft -> last) { ft -> last = fd; }
	}
	else { lock_release(ft -> lock); return EBADF; }

	lock_release(ft -> lock);
        
        return (0);

}

openfile *
ft_get (filetable *ft, unsigned int fd)
{

	openfile *temp = NULL;

	KASSERT(ft != NULL);
	KASSERT(fd < MAX_FILES);

	if (ft -> handles[0] == NULL) { ft_init(ft); }

	lock_acquire(ft -> lock);
	temp = ft -> handles[fd];
	lock_release(ft -> lock);
	return (temp);

}

int
ft_set (filetable *ft, unsigned int fd, openfile *file)
{

	KASSERT(ft != NULL);
	KASSERT(fd < MAX_FILES);

	if (ft -> handles[0] == NULL) { ft_init(ft); }

	lock_acquire(ft -> lock);
        ft -> handles[fd] = file;
	lock_release(ft -> lock);

	return (0);	

}

int
ft_copy (filetable *copyfrom, filetable *copyto)
{
	
        int i;

        KASSERT(copyfrom != NULL);
        KASSERT(copyto != NULL);

	lock_acquire(copyfrom -> lock);

        for (i = 0; i < MAX_FILES; i++) {
		if (copyfrom -> handles[i] != NULL) {
			ft_add(copyto, copyfrom -> handles[i], 1, NULL);
			copyto -> handles[i] -> refs++;
		}
        }
	copyto -> last = copyfrom -> last;

	lock_release(copyfrom -> lock);

	return (0);

}


int
ft_destroy (filetable *ft)
{

	int i;

        KASSERT(ft != NULL);

	for (i = 0; i < MAX_FILES; i++) {
		ft_remove(ft, i);
        }

	lock_destroy(ft -> lock);

        kfree(ft);

	return (0);

}
