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
#define FTYPE_FILE 0
#define FTYPE_DIR 1

struct vtfs_node {
    char name[NAME_MAX];
    ino_t parent_ino;
    ino_t ino;
    umode_t mode;
    size_t children;
};

static struct vtfs_node vtfs_nodes[VTFS_NODES_MAX];

static struct dentry* vtfs_mount(struct file_system_type*, int, const char*, void*);
static void vtfs_kill_sb(struct super_block*);
static int vtfs_fill_super(struct super_block*, void*, int);
static struct inode* vtfs_get_inode(struct super_block*, const struct inode*, umode_t, int, struct mnt_idmap *idmap);

// traverse group
static struct dentry* vtfs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flag);
static int vtfs_iterate(struct file *filp, struct dir_context *ctx);

// create group
static int vtfs_mkobj(struct mnt_idmap *idmap, struct inode *parent_inode, struct dentry *child_dentry, umode_t mode, bool type);
static int vtfs_create(struct mnt_idmap *idmap, struct inode *parent_inode, struct dentry *child_dentry, umode_t mode, bool b);
static int vtfs_mkdir(struct mnt_idmap *idmap, struct inode *parent_inode, struct dentry *child_dentry, umode_t mode);

// remove group
static int vtfs_rmobj(struct inode *parent_inode, struct dentry *child_dentry, bool type);
static int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry);
static int vtfs_rmdir(struct inode *parent_inode, struct dentry *child_dentry);

// io group
static ssize_t vtfs_read(struct file *filp, char __user *buffer, size_t len, loff_t *offset);
static ssize_t vtfs_write(struct file *filp, const char __user *buffer, size_t len, loff_t *offset);

// backend group
static int vtfs_store_find(ino_t parent_ino, const char *name, struct vtfs_node **out);
static int vtfs_store_add(ino_t parent_ino, const char *name, umode_t mode, bool type, struct vtfs_node **out);
static int vtfs_store_remove(struct inode *parent_inode, struct vtfs_node *node, bool type);
static int vtfs_store_list(ino_t parent_ino, struct vtfs_node **list, size_t *count);

struct file_system_type vtfs_fs_type = {
    .name = "vtfs",
    .mount = vtfs_mount,
    .kill_sb = vtfs_kill_sb
};

struct inode_operations vtfs_inode_ops = {
    .lookup = vtfs_lookup,
    .create = vtfs_create,
    .unlink = vtfs_unlink,
    .mkdir = vtfs_mkdir,
    .rmdir = vtfs_rmdir
};

struct file_operations vtfs_dir_ops = {
    .iterate_shared = vtfs_iterate,
};

struct file_operations vtfs_file_ops = {
    .read = vtfs_read,
    .write = vtfs_write,
};

static int vtfs_store_find(ino_t parent_ino, const char *name, struct vtfs_node **out) {
    for (size_t i = 0; i < VTFS_NODES_MAX; i++) {
        if (vtfs_nodes[i].parent_ino == parent_ino && !strcmp(vtfs_nodes[i].name, name)) {
            *out = &vtfs_nodes[i];
            return 0;
        }
    }
    return -ENOENT;
}

static int vtfs_store_remove(struct inode *parent_inode, struct vtfs_node *node, bool type) {
    if (type == FTYPE_DIR && node->children != 0) {
        return -ENOTEMPTY;
    }
    if (type == FTYPE_DIR && !S_ISDIR(node->mode)) {
        return -ENOTDIR;
    }
    if (type == FTYPE_FILE && S_ISDIR(node->mode)) {
        return -EISDIR;
    }

    memset(node->name, 0, sizeof(node->name));
    node->ino = 0;
    node->parent_ino = 0;
    node->mode = 0;
    node->children = 0;

    struct vtfs_node *parent_node = (struct vtfs_node *)parent_inode->i_private;
    if (parent_node != NULL) {
        parent_node->children--;
    }

    return 0;
}

static struct inode *vtfs_get_inode(
    struct super_block *sb,
    const struct inode *dir,
    umode_t mode,
    int i_ino,
    struct mnt_idmap *idmap
) {
    struct inode *inode = new_inode(sb);
    if (inode == NULL) {
        return NULL;
    }

    if (idmap == NULL) {
        idmap = &nop_mnt_idmap;
    }
    inode_init_owner(idmap, inode, dir, mode);

    inode->i_op = &vtfs_inode_ops;
    if (S_ISDIR(mode)) {
        inode->i_fop = &vtfs_dir_ops;
        inc_nlink(inode);
    } else {
        inode->i_fop = &vtfs_file_ops;
    }

    inode->i_ino = i_ino;
    return inode;
}

static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
    struct inode *inode = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, ROOT_INODE_NUM, NULL);
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
        ERR("Can't mount file system\n");
    } else {
        LOG("Mounted successfully\n");
    }

    return ret;
}

static ssize_t vtfs_read(
    struct file *filp,      // файловый дескриптор
    char __user *buffer,    // буфер в user-space для чтения и записи соответственно
    size_t len,             // длина данных для записи
    loff_t *offset          // смещение
) {
    return 0;
}

static ssize_t vtfs_write(
    struct file *filp, 
    const char __user *buffer, 
    size_t len, 
    loff_t *offset
) {
    return 0;
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

    struct vtfs_node *node = NULL;
    int ret = vtfs_store_find(root, name, &node);
    if (ret == 0) {
        inode = vtfs_get_inode(parent_inode->i_sb, NULL, node->mode, (int)node->ino, NULL);
        if (inode == NULL) {
            return ERR_PTR(-ENOMEM);
        }
    }

    d_add(child_dentry, inode);
    return NULL;
}

// creates file or dir
// depends on type
static int vtfs_mkobj(
    struct mnt_idmap *idmap,
    struct inode *parent_inode,
    struct dentry *child_dentry,
    umode_t mode,
    bool type // 0 - file; 1 - dir
) {
    ino_t root = parent_inode->i_ino;
    const char *name = (const char *)child_dentry->d_name.name;

    if (child_dentry->d_name.len > NAME_MAX) {
        return -ENAMETOOLONG;
    }

    int slot = -1;

    struct vtfs_node *node = NULL;
    int ret = vtfs_store_find(root, name, &node);
    if (ret == 0) {
        if (type == FTYPE_DIR) {
            ERR("dir %s already exists\n", name);
        } else {
            ERR("file %s already exists\n", name);
        }
        return -EEXIST;
    } 
    
    if (ret != -ENOENT) {
        return ret;
    }

    for (size_t i = 0; i < VTFS_NODES_MAX; i++) {
        if (vtfs_nodes[i].ino == 0 && slot == -1) {
            slot = (int)i;
            break;
        }
    }

    if (slot == -1) {
        return -ENOSPC;
    }

    struct inode *inode = NULL;
    if (type == FTYPE_DIR) {
        inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFDIR | mode, ROOT_INODE_NUM + 1 + slot, idmap);
    } else {
        inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFREG | mode, ROOT_INODE_NUM + 1 + slot, idmap);
    }
    if (inode == NULL) {
        return -ENOMEM;
    }

    // double link: now we have ability to get struct vtfs_node from inode and vice-versa
    inode->i_private = &vtfs_nodes[slot];

    struct vtfs_node *parent_node = (struct vtfs_node *)parent_inode->i_private;
    if (parent_node != NULL) {
        parent_node->children++;
    }

    vtfs_nodes[slot].ino = inode->i_ino;
    vtfs_nodes[slot].parent_ino = root;
    vtfs_nodes[slot].mode = inode->i_mode;
    strscpy(vtfs_nodes[slot].name, name, sizeof(vtfs_nodes[slot].name));

    if (type == FTYPE_DIR) {
        // increment hard links count for parent_inode
        inc_nlink(parent_inode);
    }
    d_instantiate(child_dentry, inode);

    if (type == 1) {
        LOG("created dir %s\n", name);
    } else {
        LOG("created file %s\n", name);
    }
    return 0;
}

static int vtfs_mkdir(
    struct mnt_idmap *idmap,
    struct inode *parent_inode,
    struct dentry *child_dentry,
    umode_t mode
) {
    return vtfs_mkobj(idmap, parent_inode, child_dentry, mode, FTYPE_DIR);
}

static int vtfs_create(
    struct mnt_idmap *idmap,
    struct inode *parent_inode, 
    struct dentry *child_dentry, 
    umode_t mode, 
    bool b
) {
    return vtfs_mkobj(idmap, parent_inode, child_dentry, mode, FTYPE_FILE);
}

// removes file or dir
// depends on type
static int vtfs_rmobj(
    struct inode *parent_inode,
    struct dentry *child_dentry,
    bool type // 0 - file; 1 - dir
) {
    const char *name = child_dentry->d_name.name;
    ino_t root = parent_inode->i_ino;

    struct vtfs_node *node = NULL;
    int ret = vtfs_store_find(root, name, &node);
    if (ret != 0) {
        return ret;
    }

    ret = vtfs_store_remove(parent_inode, node, type);
    if (ret != 0) {
        return ret;
    }

    if (type == FTYPE_DIR) {
        drop_nlink(parent_inode);
    }
    d_drop(child_dentry);

    if (type == FTYPE_DIR) {
        LOG("deleted dir %s\n", name);
    } else {
        LOG("deleted file %s\n", name);
    }

    return 0;
}

static int vtfs_rmdir(struct inode *parent_inode, struct dentry *child_dentry) {
    return vtfs_rmobj(parent_inode, child_dentry, FTYPE_DIR);
}

static int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry) {
    return vtfs_rmobj(parent_inode, child_dentry, FTYPE_FILE);
}

static int vtfs_iterate(struct file *filp, struct dir_context *ctx) {
    struct inode *inode = file_inode(filp);
    ino_t root = inode->i_ino;

    // skip '.' & '..'
    if (!dir_emit_dots(filp, ctx)) {
        return 0;
    }

    size_t want_skip = (ctx->pos >= 2) ? (size_t)(ctx->pos - 2) : 0;
    size_t idx = 0;

    for (size_t i = 0; i < VTFS_NODES_MAX; i++) {
        struct vtfs_node *node = &vtfs_nodes[i];
        if (node->ino == 0 || node->parent_ino != root) {
            continue;
        }

        if (idx < want_skip) {
            idx++;
            continue;
        }

        unsigned char ftype = S_ISDIR(node->mode) ? DT_DIR : DT_REG;
        if (!dir_emit(ctx, node->name, strlen(node->name), node->ino, ftype)) {
            return 0;
        }

        ctx->pos++;
        idx++;
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
