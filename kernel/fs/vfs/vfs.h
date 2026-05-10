#pragma once

#include "lib/types.h"

#ifndef off_t
typedef int64_t off_t;
#endif

#define MAX_FD_PER_PROCESS 16
#define MAX_FILES_SYSTEM   256

typedef enum {
    FT_REGULAR = 0,
    FT_DIRECTORY = 1,
    FT_CHARDEV = 2,
    FT_BLOCKDEV = 3,
    FT_PIPE = 4,
    FT_SYMLINK = 5
} file_type_t;

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   4
#define O_TRUNC   8
#define O_APPEND  16

#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

// Forward declarations
struct file;
struct inode;

typedef struct file_ops {
    int (*read)(struct file *f, void *buf, size_t count);
    int (*write)(struct file *f, const void *buf, size_t count);
    int (*seek)(struct file *f, off_t offset, int whence);
    int (*close)(struct file *f);
} file_ops_t;

typedef struct inode_ops {
    struct inode* (*lookup)(struct inode *dir, const char *name);
    int (*create)(struct inode *dir, const char *name, file_type_t type);
    int (*remove)(struct inode *dir, const char *name);
    int (*readdir)(struct inode *dir, void *buf, int index);
} inode_ops_t;

typedef struct inode {
    uint32_t ino;
    file_type_t type;
    uint32_t size;
    uint32_t ref_count;
    void *private_data;
    inode_ops_t *ops;
} inode_t;

typedef struct file {
    int fd;
    inode_t *inode;
    uint32_t offset;
    int flags;
    file_ops_t *ops;
} file_t;

typedef struct fd_table {
    file_t *files[MAX_FD_PER_PROCESS];
    int count;
} fd_table_t;

typedef struct filesystem {
    char name[32];
    int (*mount)(const char *device);
    int (*unmount)(void);
    inode_t* (*get_root)(void);
} filesystem_t;

// API functions
void vfs_init(void);
int vfs_open(const char *path, int flags);
int vfs_close(int fd);
int vfs_read(int fd, void *buf, size_t count);
int vfs_write(int fd, const void *buf, size_t count);
int vfs_seek(int fd, off_t offset, int whence);
int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path);
int vfs_readdir(int fd, void *buf);
inode_t* vfs_lookup(const char *path);
inode_t* vfs_get_root(void);
inode_t* vfs_create_inode(uint32_t ino, file_type_t type, 
                           inode_ops_t *ops, void *private_data);
void vfs_register_dev(const char *name, file_ops_t *fops, file_type_t type);
int alloc_fd(void);

// Exported globals
extern file_t *file_table[];
extern fd_table_t *current_fd_table;