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
static struct dentry* vtfs_lookup(struct inode* parent_inode, struct dentry* child_dentry, unsigned int flag);
static int vtfs_iterate(struct file* filp, struct dir_context* ctx);

struct file_system_type vtfs_fs_type = {
    .name = "vtfs",
    .mount = vtfs_mount,
    .kill_sb = vtfs_kill_sb
};

struct inode_operations vtfs_inode_ops = {
    .lookup = vtfs_lookup
};

struct file_operations vtfs_dir_ops = {
    .iterate_shared = vtfs_iterate,
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
    inode->i_op = &vtfs_inode_ops;

    inode->i_fop = &vtfs_dir_ops;

    return inode;
}

static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
    struct inode *inode = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, 100);

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

struct dentry *vtfs_lookup(
    struct inode *parent_inode,  // родительская нода
    struct dentry *child_dentry, // объект, к которому мы пытаемся получить доступ
    unsigned int flag            // неиспользуемое значение
) {
    ino_t root = parent_inode->i_ino;
    const char *name = child_dentry->d_name.name;

    if (root == 100 && !strcmp(name, "test.txt")) {
        struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFREG, 101);
        d_add(child_dentry, inode);
    } else if (root == 100 && !strcmp(name, "dir")) {
        struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFDIR, 200);
        d_add(child_dentry, inode);
    }
    return NULL;
}

int vtfs_iterate(struct file *filp, struct dir_context *ctx) {
    char fsname[10];
    struct dentry* dentry = filp->f_path.dentry;
    struct inode* inode = dentry->d_inode;
    unsigned long offset = ctx->pos;
    int stored = 0;
    ino_t ino = inode->i_ino;

    unsigned char ftype;
    ino_t dino;

    switch (offset) {
        case 0: {
            strcpy(fsname, ".");
            ftype = DT_DIR;
            dino = ino;
            if (dir_emit(ctx, fsname, 1, dino, ftype)) {
                ctx->pos++;
            }
            return stored;
        }
        case 1: {
            strcpy(fsname, "..");
            ftype = DT_DIR;
            dino = dentry->d_parent->d_inode->i_ino;
            if (dir_emit(ctx, fsname, 2, dino, ftype)) {
                ctx->pos++;
            }
            return stored;
        }
        case 2: {
            strcpy(fsname, "test.txt");
            ftype = DT_REG;
            dino = 101;
            if (dir_emit(ctx, fsname, 8, dino, ftype)) {
                ctx->pos++;
            }
            return stored;
        }
        case 3: {
            strcpy(fsname, "dir");
            ftype = DT_DIR;
            dino = 200; 
            if (dir_emit(ctx, fsname, 3, dino, ftype)) {
                ctx->pos++;
            }
            return stored;
        }
        default: {
            LOG("return stored!!!\n");
            return stored;
        }
    }
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
