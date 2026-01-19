#pragma once

#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/types.h>
#include <linux/printk.h>

#define MODULE_NAME "vtfs"

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) pr_err("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

#define VTFS_NODES_MAX 1024
#define ROOT_INODE_NUM 1
#define FTYPE_FILE 0
#define FTYPE_DIR 1
#define FTYPE_HLINK 2
#define MAXFILE_SIZE (128 * 1024 * 1024)

struct vtfs_inode {
    bool used;
    ino_t ino;
    umode_t mode;
    nlink_t nlink;
    loff_t size;
    loff_t capacity;
    void *data;
    size_t children;
};

struct vtfs_dirent {
    bool used;
    char name[NAME_MAX];
    struct vtfs_inode *parent_vinode;
    struct vtfs_inode *vinode;
};

// iterator
struct vtfs_list_iter {
    struct vtfs_inode *parent_vinode;
    size_t idx;
};

// get_inode
struct inode *vtfs_get_inode(struct super_block *sb, const struct inode *dir, umode_t mode, ino_t i_ino, struct mnt_idmap *idmap);

extern const struct inode_operations vtfs_inode_ops;
extern const struct file_operations vtfs_file_ops;
extern const struct file_operations vtfs_dir_ops;