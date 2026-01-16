#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/mount.h>

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) pr_err("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

static struct dentry* vtfs_mount(struct file_system_type*, int, const char*, void*);
static void vtfs_kill_sb(struct super_block*);
static int vtfs_fill_super(struct super_block*, void*, int);
static struct inode* vtfs_get_inode(struct super_block*, const struct inode*, umode_t, int);

struct file_system_type vtfs_fs_type = {
    .name = "vtfs",
    .mount = vtfs_mount,
    .kill_sb = vtfs_kill_sb
};

static struct inode *vtfs_get_inode(
    struct super_block *sb,
    const struct inode *dir,
    umode_t mode,
    int i_ino
) {
    struct inode *inode = new_inode(sb);
    if (inode == NULL) {
        return NULL;
    }

    inode->i_mode = mode;
    inode->i_uid = GLOBAL_ROOT_UID;
    inode->i_gid = GLOBAL_ROOT_GID;
    inode->i_ino = i_ino;

    return inode;
}

static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
    struct inode *inode = vtfs_get_inode(sb, NULL, S_IFDIR, 1000);

    sb->s_root = d_make_root(inode);
    if (sb->s_root == NULL) {
        return -ENOMEM;
    }

    LOG("return 0\n");
    return 0;
}

static void vtfs_kill_sb(struct super_block* sb) {
    LOG("vtfs super block is destroyed. Unmount successfully.\n");
}

static struct dentry *vtfs_mount(
    struct file_system_type *fs_type,
    int flags,
    const char *token,
    void *data
) {
    struct dentry *ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
    if (ret == NULL) {
        ERR("Can't mount file system");
    } else {
        LOG("Mounted successfully");
    }

    return ret;
}

static int __init vtfs_init(void) {
    int ret = register_filesystem(&vtfs_fs_type);
    if (ret != 0) {
        ERR("can't register filesystem\n");
        return ret;
    }

    LOG("VTFS joined the kernel\n");
    return 0;
}

static void __exit vtfs_exit(void) {
    int ret = unregister_filesystem(&vtfs_fs_type);
    if (ret != 0) {
        ERR("can't unregister filesystem\n");
    } else {
        LOG("VTFS left the kernel\n");
    }
}

module_init(vtfs_init);
module_exit(vtfs_exit);
