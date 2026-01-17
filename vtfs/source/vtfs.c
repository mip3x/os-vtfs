#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/mount.h>

// references:
// https://docs.kernel.org/filesystems/vfs.html

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) pr_err("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

#define VTFS_NODES_MAX 1024
#define ROOT_INODE_NUM 1

struct vtfs_node {
    char name[NAME_MAX];
    ino_t parent_ino;
    ino_t ino;
    umode_t mode;
};

static struct vtfs_node vtfs_nodes[VTFS_NODES_MAX];

static struct dentry* vtfs_mount(struct file_system_type*, int, const char*, void*);
static void vtfs_kill_sb(struct super_block*);
static int vtfs_fill_super(struct super_block*, void*, int);
static struct inode* vtfs_get_inode(struct super_block*, const struct inode*, umode_t, int);
static struct dentry* vtfs_lookup(struct inode* parent_inode, struct dentry* child_dentry, unsigned int flag);
static int vtfs_create(struct mnt_idmap *idmap, struct inode* parent_inode, struct dentry* child_dentry, umode_t mode, bool b);
static int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry);
static int vtfs_iterate(struct file* filp, struct dir_context* ctx);

struct file_system_type vtfs_fs_type = {
    .name = "vtfs",
    .mount = vtfs_mount,
    .kill_sb = vtfs_kill_sb
};

struct inode_operations vtfs_inode_ops = {
    .lookup = vtfs_lookup,
    .create = vtfs_create,
    .unlink = vtfs_unlink,
};

struct file_operations vtfs_dir_ops = {
    .iterate_shared = vtfs_iterate,
};

struct file_operations vtfs_file_ops = {
    .read = NULL,
    .write = NULL,
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

    if (S_ISDIR(mode)) {
        inode->i_op = &vtfs_inode_ops;
        inode->i_fop = &simple_dir_operations;
        inc_nlink(inode);
    }

    return inode;
}

static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
    struct inode *inode = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, ROOT_INODE_NUM);
    if (inode == NULL) {
        return -ENOMEM;
    }

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

static struct dentry *vtfs_lookup(
    struct inode *parent_inode,  // родительская нода
    struct dentry *child_dentry, // объект, к которому мы пытаемся получить доступ
    unsigned int flag            // неиспользуемое значение
) {
    ino_t root = parent_inode->i_ino;
    const char *name = (const char *)child_dentry->d_name.name;
    struct inode *inode = NULL;

    if (child_dentry->d_name.len > NAME_MAX) {
        return ERR_PTR(-ENAMETOOLONG);
    }

    for (size_t i = 0; i < VTFS_NODES_MAX; i++) {
        if (vtfs_nodes[i].parent_ino == root && !strcmp(vtfs_nodes[i].name, name)) {
            inode = vtfs_get_inode(parent_inode->i_sb, NULL,
                                                 vtfs_nodes[i].mode, (int)vtfs_nodes[i].ino);
            if (inode == NULL) {
                return ERR_PTR(-ENOMEM);
            }

            inode->i_op = &vtfs_inode_ops;
            inode->i_fop = &vtfs_file_ops;
            atomic_inc(&inode->i_count);

            break;
        }
    }

    d_add(child_dentry, inode);
    return NULL;
}

static int vtfs_create(
    struct mnt_idmap *idmap,
    struct inode *parent_inode, 
    struct dentry *child_dentry, 
    umode_t mode, 
    bool b
) {
    ino_t root = parent_inode->i_ino;
    const char *name = (const char *)child_dentry->d_name.name;

    if (child_dentry->d_name.len > NAME_MAX) {
        return -ENAMETOOLONG;
    }

    int slot = -1;

    for (size_t i = 0; i < VTFS_NODES_MAX; i++) {
        if (vtfs_nodes[i].ino != 0 && vtfs_nodes[i].parent_ino == root) {
            if (!strcmp(name, vtfs_nodes[i].name)) {
                return -EEXIST;
            }
        } else if (vtfs_nodes[i].ino == 0 && slot == -1) {
            slot = (int)i;
        }
    }

    if (slot == -1) {
        return -ENOSPC;
    }

    struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFREG | mode, ROOT_INODE_NUM + 1 + slot);
    if (inode == NULL) {
        return -ENOMEM;
    }

    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = &vtfs_file_ops;

    vtfs_nodes[slot].ino = inode->i_ino;
    vtfs_nodes[slot].parent_ino = root;
    vtfs_nodes[slot].mode = inode->i_mode;
    strscpy(vtfs_nodes[slot].name, name, sizeof(vtfs_nodes[slot].name));

    d_instantiate(child_dentry, inode);

    LOG("created file %s", name);
    return 0;
}

static int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry) {
    const char *name = child_dentry->d_name.name;
    ino_t root = parent_inode->i_ino;

    for (size_t i = 0; i < VTFS_NODES_MAX; i++) {
        if (vtfs_nodes[i].parent_ino == root && !strcmp(vtfs_nodes[i].name, name)) {
            memset(vtfs_nodes[i].name, 0, sizeof(vtfs_nodes[i].name));
            vtfs_nodes[i].ino = 0;
            vtfs_nodes[i].parent_ino = 0;
            vtfs_nodes[i].mode = 0;

            d_drop(child_dentry);

            LOG("deleted file %s", name);
            return 0;
        }
    }

    return -ENOENT;
}

static int vtfs_iterate(struct file *filp, struct dir_context *ctx) {
    struct inode *inode = filp->f_path.dentry->d_inode;
    ino_t root = inode->i_ino;

    if (!dir_emit_dots(filp, ctx)) {
        return 0;
    }

    for (size_t i = 0; i < VTFS_NODES_MAX; i++) {
        struct vtfs_node *node = &vtfs_nodes[i];
        if (node->ino == 0 || node->parent_ino != root) {
            continue;
        }

        unsigned char ftype = S_ISDIR(node->mode) ? DT_DIR : DT_REG;

        if (!dir_emit(ctx, node->name, strlen(node->name), node->ino, ftype)) {
            return 0;
        }
        ctx->pos++;
    }

    return 0;
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
