#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <kern/unistd.h>
#include <lib.h>
#include <uio.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <vnode.h>
#include <file.h>
#include <syscall.h>
#include <copyinout.h>
#include <proc.h>

/*
 * Initalise open file table
 */
int init_of_table(void) {

	int i;

	/* Initialise File open table if not intialised already */
	if (of_table == NULL) {
		of_table = kmalloc(sizeof(struct open_file_table));
		if (of_table == NULL) {
			return ENOMEM;
		}

		for(i = 0; i < OPEN_MAX; i++) {
            of_table->openedFiles[i] = NULL;
		}

		of_table->of_lock = lock_create("of_table_lock");
        KASSERT(of_table->of_lock != 0);
        if (of_table->of_lock == 0) {
            return ENOMEM;
		}
	}

	return 0;
}

/*
 * Initalise file descriptor table
 */
int init_fd_table(void) {

	int result, i;

	curproc->fdtable = kmalloc(sizeof(struct fd_table));
	if (curproc->fdtable == NULL) {
		return ENOMEM;
	}

	/* empty the new table */
	for (i = 0; i < OPEN_MAX; i++) {
		curproc->fdtable->fd[i] = CLOSED_FILE;
	}

	/* make stdin file descriptor */
	char pathStdin[5] = "con:\0";
	result = open_stdfds(pathStdin, O_RDONLY, 0, STDIN_FILENO);
	if (result) {
		kfree(curproc->fdtable); //Free the fd table
		return result;
	}

	/* make stdout file descriptor */
	char pathStdout[5] = "con:\0";
	result = open_stdfds(pathStdout, O_WRONLY, 0, STDOUT_FILENO);
	if (result) {
		sys_close(STDIN_FILENO);
		kfree(curproc->fdtable); //Free the fd table
		return result;
	}
	/* make stderror file descriptor */
	char pathStderror[5] = "con:\0";
	result = open_stdfds(pathStderror, O_WRONLY, 0, STDERR_FILENO);
	if (result) {
		sys_close(STDIN_FILENO);
		sys_close(STDOUT_FILENO);
		kfree(curproc->fdtable); //Free the fd table
		return result;
	}

	return 0;
}

/*
 * Destroy the process fd table
 */
 void destroy_fd_table(struct fd_table *fdt) {
	/* Ensure all the files are closed before freeing */
	for (int i = 0; i < OPEN_MAX; i++) {
		if (fdt->fd[i] != CLOSED_FILE) {
			sys_close(i);
		}
	}
	kfree(fdt);
}

/*
 * sys_open() - Opens a file
 *
 * RETURN VALUE
 * On Success: The new file descriptor
 * On Failure: -1
 *	-	setting errno appropiately
 */
int sys_open(userptr_t pathname, int flags, mode_t mode, int*retval) {

	int allflags =
		O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_NOCTTY;
	int result;
	char *path;
	struct open_file *of;
	*retval = -1;

	/* Check if unknown flags were set */
	if ((flags & allflags) != flags) {
		return EINVAL;
	}

	path = kmalloc(PATH_MAX);
	if (path == NULL) {
		return ENOMEM;
	}

	/* Get Path from Userland to kernal */
	result = copyinstr(pathname, path, PATH_MAX, NULL);
	if (result) {
		kfree(path);
		return result;
	}

	/* Open the file */
	result = create_open_file(path, flags, mode, &of);
	kfree(path); // Free path
	if (result) {
		return result;
	}

	/* Insert the file in file table */
	lock_acquire(of_table->of_lock);
	result = insert_open_file(of, retval);
	if (result) {
		vfs_close(of->vn);
		kfree(of); // Clean up of
		lock_release(of_table->of_lock);
		return result;
	}
	lock_release(of_table->of_lock);

	return 0;
}

/*
 * sys_lseek
 *
 * RETURN VALUE
 * On Success: the resulting offset location
 * On Failure: -1
 *	-	setting errno appropiately
 */
int sys_lseek(int fd, off_t pos, int whence, off_t *retval) {

	struct stat vn_stat;
	int result;
	off_t file_size;
	off_t offset;

	*retval = -1;

	/* check if fd is valid */
	int OFT_key = fd_sanity_check(fd);
	if(OFT_key == CLOSED_FILE) {
		return EBADF;
	}

	struct vnode *vnode = of_table->openedFiles[OFT_key]->vn;

	/* check file can be seeked through VOP_ISSEEKABLE */
	if(!VOP_ISSEEKABLE(vnode)) {
		return ESPIPE; //TODO: Check if this is right
	}

	/* Get total file size */
	result = VOP_STAT(vnode, &vn_stat);
	if (result) {
		return result;
	}
	file_size = vn_stat.st_size;

	lock_acquire(of_table->of_lock);

	/* whence conditions */
	switch(whence) {
		case SEEK_SET:
			offset = pos;
			break;
		case SEEK_CUR:
			offset = pos + of_table->openedFiles[OFT_key]->offset;
			break;
		case SEEK_END:
			offset = pos + file_size;
			break;
		default:
			lock_release(of_table->of_lock);
			return EINVAL;
	}

	/* Check if offset is in range of the file */
	if (offset < (off_t) 0 || offset > file_size) {
		lock_release(of_table->of_lock);
		return EINVAL;
	}

	/* update open file */
	of_table->openedFiles[OFT_key]->offset = offset;

	lock_release(of_table->of_lock);

	*retval = offset;

	return 0;
}


/*
 * sys_dup2
 *
 * fid = dup2(fd, new_fd);
 *
 * RETURN VALUE
 * On Success: new_fd
 * On Failure: -1
 *	-	setting errno appropiately
 *
 * CASES
 * If new_fd is negative or greater than or equal to OPEN_MAX, the dup2()
 *		function returns a value of -1 and sets errno to EBADF.
 * If fd is a valid file descriptor and is equal to new_fd, the dup2()
 *		function returns new_fd without closing it.
 * If fd is not a valid file descriptor, dup2() fails and does not close new_fd.
 * If successful, dup2() returns a value that is equal to the value of new_fd.
 *		If a failure occurs, it returns a value of -1.
 * Cases provided from https://www.mkssoftware.com/docs/man3/dup2.3.asp
 */
int sys_dup2(int fd, int new_fd, int *retval) {

	*retval = -1;

	/* check if fd and new_fd are valid */
	if (fd < 0 || fd >= OPEN_MAX || new_fd < 0 || new_fd >= OPEN_MAX) {
		return EBADF;
	}

	/* Check if fd has a corresponding open file */
	int OFT_key = fd_sanity_check(fd);
	if (OFT_key == CLOSED_FILE) {
		return EBADF;
	}

	/* Check if fd is equal to new_fd */
	if (fd == new_fd) {
		*retval = new_fd;
		return 0;
	}

	/* If new_fd has a file close it */
	if(curproc->fdtable->fd[new_fd] != CLOSED_FILE){
    	if(sys_close(new_fd)) {
        	return EBADF;
        }
    }

    /* Set file of fd equal to file of new_fd */
    lock_acquire(of_table->of_lock);

    curproc->fdtable->fd[new_fd] = curproc->fdtable->fd[fd];
    of_table->openedFiles[OFT_key]->rc++;

	lock_release(of_table->of_lock);

	*retval = new_fd;

	return 0;
}

/*
 * sys_write
 * */
int sys_write(int fd, void *write_buf, size_t nbytes, size_t *retval)
{
    return sys_rw(fd, write_buf, nbytes, UIO_WRITE, retval);
}


/*
 * sys_read
 *
 * Return Values
 * The count of bytes read is returned. This count should be positive. A return value of 0 should be construed as
 * signifying end-of-file. On error, read returns -1 and sets errno to a suitable error code for the error condition
 * encountered.
 *
 * Errors
 * The following error codes should be returned under the conditions given. Other error codes may be returned for other
 * cases not mentioned here.
 *  	EBADF	fd is not a valid file descriptor, or was not opened for reading.
 *  	EFAULT	Part or all of the address space pointed to by buf is invalid.
 *  	EIO	A hardware I/O error occurred reading the data.
 * */


int sys_read(int fd, void *read_buf, size_t nbytes, size_t *retval) {
    return sys_rw(fd, read_buf, nbytes, UIO_READ, retval);
}


/*
 * sys_rw
 * Shared function with sys_read and sys_write as both function uses the same logic
 * retval = bytes written to/ read
 * mode distinguish between read and write
 */
int sys_rw(int fd, void *buf, size_t nbytes, enum uio_rw mode, size_t *retval) {
    //entering critical region
    lock_acquire(of_table->of_lock);
    // fd sanity check
    int OFT_key = fd_sanity_check(fd);
    if (OFT_key < 0) {
        /* sanity check failed
         * abort: release lock and return error code
         */
        lock_release(of_table->of_lock);
        return EBADF;
    }
    struct open_file *of = of_table->openedFiles[OFT_key];

    /* Check if opened file is for writing/reading */
    if (mode == UIO_WRITE) {
        switch (of->flags & O_ACCMODE) {
            case O_WRONLY:
                break;
            case O_RDWR:
                break;
            default:
                /* abort release lock and return err code */
                lock_release(of_table->of_lock);
                return EBADF;
        }
    }
    if (mode == UIO_READ) {
        switch (of->flags & O_ACCMODE) {
            case O_RDONLY:
                break;
            case O_RDWR:
                break;
            default:
                /* abort release lock and return err code */
                lock_release(of_table->of_lock);
                return EBADF;
        }
    }

    /* Check if opened file is seekable */
    int is_seekable = VOP_ISSEEKABLE(of->vn);
    off_t offset = 0;
    if (is_seekable) {
        /* Change the file to file's offset */
        offset = of->offset;
    }

    /* setting up variables for uio */
    struct vnode *vn = of->vn;
    struct iovec iov;
    struct uio ku;


    uio_kinit(&iov, &ku, buf, nbytes, offset, mode);
    // write/read function
    int res;
    if (mode == UIO_WRITE) {
        res = VOP_WRITE(vn, &ku);
    } else {
        res = VOP_READ(vn, &ku);
    }

    /* write function catching error/exception */
    if (res != 0) {
        lock_release(of_table->of_lock);
        return res;
    }

    /*
     * Update table status and retval to the number of bytes read/write
     * Formula used is byteswritten/read = (Intended amount to write/read) - (actual amount that is written/read)
     * */

    of->offset = ku.uio_offset;
    /* Exit Critical Region */
    lock_release(of_table->of_lock);

    *retval = nbytes - ku.uio_resid;

    return 0;
}

/*
 * sys_close ()
 *int close(int fd);
 * Return
 * On success, close returns 0. On error, -1 is returned, and errno is set according to the error encountered.
 *
 * Errors
 * The following error codes should be returned under the conditions given. Other error codes may be returned for other cases not mentioned here.
 * EBADF	fd is not a valid file handle.
 * EIO	A hard I/O error occurred.
 */
int sys_close(int fd) {
    //entering critical region
    lock_acquire(of_table->of_lock);
    // fd sanity check
    int OFT_key = fd_sanity_check(fd);
    if (OFT_key < 0) {
        /*
         * Sanity check failed and abort.
         * Release lock and return error code
         */
        lock_release(of_table->of_lock);
        return EBADF;
    }

    struct open_file *of = of_table->openedFiles[OFT_key];

    vfs_close(of->vn);

    // Update of_table
    // Reduce ref_count
    of->rc--;
    // Clean up op table if ref_count is 0
    if (of->rc == 0) {
        op_entry_cleanup(of);
        of_table->openedFiles[OFT_key] = NULL;
        // variable of is gone
    }
    // Exiting critical region
    lock_release(of_table->of_lock);
    return 0;
}

/**************************
 * HELPER FUNCTION
 * ************************/

/*
 * checks if fd is valid
 * returns OFT key if true
 * else -1
 */
int fd_sanity_check(int fd)
{
    // check the range of fd
    if (fd < 0 || fd >= OPEN_MAX) {
        return -1;
    }
    //get get OFT key
    int OFT_key = curproc->fdtable->fd[fd];
    // check array if fd exist
    if (of_table->openedFiles[OFT_key] == NULL) {
        return -1;
    }
    return OFT_key;
}


/* called by sys_close()
 * destroys op's entry
 * but doesnt destroy the memory its holding
 */
void op_entry_cleanup(struct open_file *of) {
    KASSERT(of->rc == 0);
    kfree(of);
}

/*
 * Creates an open file
 */
int create_open_file(char *path, int flags, mode_t mode, struct open_file **ret) {

	struct vnode *vn;
	struct open_file *of;

	int result = vfs_open(path, flags, mode, &vn);
	if (result) {
		return result;
	}

	of = kmalloc(sizeof(struct open_file));
	if (of == NULL) {
		vfs_close(vn); // Close vnode
		// Release lock on failure
		return ENOMEM;
	}

	of->vn = vn; // vnode of the file
	of->flags = flags; // Permissions of the file
	of->rc = 1;
	of->offset = 0;
	if(flags & O_APPEND)
    {
        struct stat inode;
        int v;
        v = VOP_STAT(of->vn, &inode);
        if(v)
        {
            return v;
        }

        of->offset = inode.st_size;
    }

	*ret = of;

	return 0;
}

/*
 * Insert into file table
 */

 int insert_open_file(struct open_file *of, int *retval) {

	 int i;
	 int fd_index = -1;
	 int of_index = -1;

	/* find space in open file table */
	for(i = 0; i < OPEN_MAX; i++) {
		if(of_table->openedFiles[i] == NULL) {
			of_index = i;
			break;
		}
	}
	/* find space in file decriptor table */
	for(i = 0; i < OPEN_MAX; i++) {
		if(curproc->fdtable->fd[i] == CLOSED_FILE) {
			fd_index = i;
			break;
		}
	}

	/* Check of can be inserted in the file tables */
	if (fd_index == -1) { // too many files opened in process
		return EMFILE;
	}
	if (of_index == -1) { // too many files opened in system
		return ENFILE;
	}

	/* Insert into files tables */
	curproc->fdtable->fd[fd_index] = of_index;
	of_table->openedFiles[of_index] = of;

	*retval = of_index;

	return 0;
 }


/*
 * Opens the standard in, out and error
 */
 int open_stdfds(char path[PATH_MAX], int flags, mode_t mode, int fd_index) {

	int of_index = -1;
	int i;
	struct open_file *of;

	int result = create_open_file(path, flags, mode, &of);
	if (result) {
		return result;
	}

	lock_acquire(of_table->of_lock);

	/* find space in open file table */
	for(i = 0; i < OPEN_MAX; i++) {
		if(of_table->openedFiles[i] == NULL) {
			of_index = i;
			break;
		}
	}

	/* Check the new open file can be inserted in the file tables */
	if (fd_index == -1) { // too many files opened in process
		vfs_close(of->vn);
		kfree(of); // Clean up of
		lock_release(of_table->of_lock);
		return EMFILE;
	}
	if (of_index == -1) { // too many files opened in system
		vfs_close(of->vn);
		kfree(of); // Clean up of
		lock_release(of_table->of_lock);
		return ENFILE;
	}

	/* Insert into files tables */
	curproc->fdtable->fd[fd_index] = of_index;
	of_table->openedFiles[of_index] = of;

	lock_release(of_table->of_lock);

	return 0;
}
