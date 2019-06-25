#ifndef _FILE_H_
#define _FILE_H_

/*
 * Contains some file-related maximum length constants
 */
#include <limits.h>
#include <vnode.h>
#include <synch.h>
#include <syscall.h>
#include <uio.h>

#define CLOSED_FILE -1

/***************
 * Declarations for file handle and file table management.
 **************/

/*
* File Descriptor Table
*/
struct fd_table {
	int fd[OPEN_MAX]; // Index of the coresponding open file
};

/*
* Open File
*/
struct  open_file {
	struct vnode *vn; // vnode of the file
	int flags; // Flags of the file
	int rc; // Reference Count
	off_t offset; // Offset of the file
};

/*
* Open File Table
*/
struct  open_file_table {
	struct open_file *openedFiles[OPEN_MAX]; // Array of the open files
	struct lock *of_lock; // Open File lock
};

/*
 * Put your function declarations and data types here ...
 */

struct open_file_table *of_table;

// Initialise the fd table and of table
int init_of_table(void);
int init_fd_table(void);
void destroy_fd_table(struct fd_table *fdt);

/* Sys functions */
int sys_open(userptr_t pathname, int flags, mode_t mode, int *retval);
int sys_close(int fd);

int sys_read(int fd, void *read_buf, size_t nbytes, size_t *retval);
int sys_write(int fd, void *write_buf, size_t nbytes, size_t *retval);
int sys_lseek(int fd, off_t pos, int whence, off_t *retval);
int sys_dup2(int fd, int new_fd, int *retval);

/* Healper Functions */
int insert_open_file(struct open_file *of, int *retval);
int create_open_file(char *path, int flags, mode_t mode, struct open_file **ret);
int sys_rw(int fd, void *buf, size_t nbytes, enum uio_rw mode, size_t *retval);
int open_stdfds(char path[PATH_MAX], int flags, mode_t mode, int fd_index);
void op_entry_cleanup(struct open_file *of);
int fd_sanity_check(int fd);

#endif /* _FILE_H_ */
