#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/printk.h>

#include "store.h"
#include "vtfs.h"

// references:
// https://docs.kernel.org/filesystems/vfs.html

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
    struct inode *inode = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, ROOT_INODE_NUM, NULL);
    if (inode == NULL) {
        return -ENOMEM;
    }

    // allocate vinode for root ('/')
    struct vtfs_inode *root_vinode = NULL;
    int slot = -1;
    int ret = vtfs_vinode_alloc(&slot, &root_vinode);
    if (ret != 0) {
        return ret;
    }

    root_vinode->nlink = 2;
    set_nlink(inode, root_vinode->nlink);

    ret = vtfs_vinode_fill(inode, root_vinode);
    inode->i_private = root_vinode;

    sb->s_root = d_make_root(inode);
    if (sb->s_root == NULL) {
        vtfs_vinode_remove(root_vinode);
        return -ENOMEM;
    }

    LOG("return 0\n");
    return 0;
}

static void vtfs_kill_sb(struct super_block *sb) {
    LOG("vtfs super block is destroyed. Unmount successfully.\n");
}

static struct dentry *
vtfs_mount(struct file_system_type *fs_type, int flags, const char *token, void *data) {
    struct dentry *ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
    if (ret == NULL) {
        ERR("Can't mount file system\n");
    } else {
        LOG("Mounted successfully\n");
    }

    return ret;
}

struct file_system_type vtfs_fs_type = {
    .name = "vtfs",
    .mount = vtfs_mount,
    .kill_sb = vtfs_kill_sb
};

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
