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

static struct vtfs_dirent vtfs_dirents[VTFS_NODES_MAX];
static struct vtfs_inode vtfs_inodes[VTFS_NODES_MAX];

static struct dentry* vtfs_mount(struct file_system_type*, int, const char*, void*);
static void vtfs_kill_sb(struct super_block*);
static int vtfs_fill_super(struct super_block*, void*, int);
static struct inode* vtfs_get_inode(struct super_block*, const struct inode*, umode_t, ino_t, struct mnt_idmap *idmap);

// traverse group
static struct dentry* vtfs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flag);
static int vtfs_iterate(struct file *filp, struct dir_context *ctx);

// create group
static int vtfs_mkobj(struct mnt_idmap *idmap, struct inode *parent_inode, struct dentry *target_dentry, struct dentry *child_dentry, umode_t mode, char type);
static int vtfs_create(struct mnt_idmap *idmap, struct inode *parent_inode, struct dentry *child_dentry, umode_t mode, bool b);
static int vtfs_mkdir(struct mnt_idmap *idmap, struct inode *parent_inode, struct dentry *child_dentry, umode_t mode);
static int vtfs_link(struct dentry *target_dentry, struct inode *parent_dir, struct dentry *link_dentry);

// remove group
static int vtfs_rmobj(struct inode *parent_inode, struct dentry *child_dentry, char type);
static int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry);
static int vtfs_rmdir(struct inode *parent_inode, struct dentry *child_dentry);

// io group
static ssize_t vtfs_read(struct file *filp, char __user *buffer, size_t len, loff_t *offset);
static ssize_t vtfs_write(struct file *filp, const char __user *buffer, size_t len, loff_t *offset);

// rename
static int vtfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry, unsigned int flags);

// fsync
static int vtfs_fsync(struct file *filp, loff_t start, loff_t end, int datasync);

// backend group
static int vtfs_dirent_find(struct vtfs_inode *parent_vinode, const char *name, struct vtfs_dirent **out);
static int vtfs_dirent_alloc(int *slot, struct vtfs_dirent **out);
static int vtfs_vinode_alloc(int *slot, struct vtfs_inode **out);
static int vtfs_dirent_fill(struct vtfs_inode *parent_vinode, struct vtfs_inode *vinode, struct vtfs_dirent *dirent, const char *name);
static int vtfs_vinode_fill(struct inode *inode, struct vtfs_inode *vinode);
static int vtfs_dirent_remove(struct vtfs_dirent *dirent, char type);
static int vtfs_vinode_remove(struct vtfs_inode *vinode);
static void vtfs_dirent_list_init(struct vtfs_list_iter *iter, struct vtfs_inode *parent_vinode);
static int vtfs_dirent_list_next(struct vtfs_list_iter *iter, struct vtfs_dirent **out);

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
    .rmdir = vtfs_rmdir,
    .link = vtfs_link,
    .rename = vtfs_rename
};

struct file_operations vtfs_dir_ops = {
    .iterate_shared = vtfs_iterate,
};

struct file_operations vtfs_file_ops = {
    .read = vtfs_read,
    .write = vtfs_write,
    .fsync = vtfs_fsync
};

static int vtfs_dirent_find(struct vtfs_inode *parent_vinode, const char *name, struct vtfs_dirent **out) {
    for (size_t i = 0; i < VTFS_NODES_MAX; i++) {
        if (vtfs_dirents[i].used == true &&
            vtfs_dirents[i].parent_vinode == parent_vinode &&
            !strcmp(vtfs_dirents[i].name, name)
        ) {
            *out = &vtfs_dirents[i];
            return 0;
        }
    }
    return -ENOENT;
}

static int vtfs_dirent_remove(struct vtfs_dirent *dirent, char type) {
    struct vtfs_inode *vinode = dirent->vinode;

    if (type == FTYPE_DIR && vinode->children != 0) {
        return -ENOTEMPTY;
    }
    if (type == FTYPE_DIR && !S_ISDIR(vinode->mode)) {
        return -ENOTDIR;
    }
    if (type == FTYPE_FILE && S_ISDIR(vinode->mode)) {
        return -EISDIR;
    }

    dirent->used = false;
    memset(dirent->name, 0, sizeof(dirent->name));

    if (dirent->parent_vinode != NULL) {
        dirent->parent_vinode->children--;
    }
    dirent->parent_vinode = NULL;

    if (vinode->nlink != 0) {
        vinode->nlink--;
    }
    dirent->vinode = NULL;
    
    return 0;
}

static int vtfs_vinode_remove(struct vtfs_inode *vinode) {
    vinode->used = false;
    vinode->ino = 0;
    vinode->mode = 0;
    vinode->nlink = 0;
    vinode->size = 0;
    vinode->capacity = 0;
    if (vinode->data != NULL) {
        kfree(vinode->data);
    }
    vinode->data = NULL;
    vinode->children = 0;

    return 0;
}

static int vtfs_dirent_alloc(int *slot, struct vtfs_dirent **out) {
    *slot = -1;
    for (size_t i = 0; i < VTFS_NODES_MAX; i++) {
        if (vtfs_dirents[i].used == false && *slot == -1) {
            *slot = (int)i;
            break;
        }
    }

    if (*slot == -1) {
        return -ENOSPC;
    }

    *out = &vtfs_dirents[*slot];
    (*out)->used = true;

    return 0;
}

static int vtfs_vinode_alloc(int *slot, struct vtfs_inode **out) {
    *slot = -1;
    for (size_t i = 0; i < VTFS_NODES_MAX; i++) {
        if (vtfs_inodes[i].used == false && *slot == -1) {
            *slot = (int)i;
            break;
        }
    }

    if (*slot == -1) {
        return -ENOSPC;
    }

    *out = &vtfs_inodes[*slot];
    (*out)->used = true;

    return 0;
}

static int vtfs_dirent_fill(
    struct vtfs_inode *parent_vinode,
    struct vtfs_inode *vinode,
    struct vtfs_dirent *dirent,
    const char *name
) {
    dirent->used = true;
    strscpy(dirent->name, name, sizeof(dirent->name));
    dirent->parent_vinode = parent_vinode;
    dirent->vinode = vinode;

    if (dirent->parent_vinode != NULL) {
        dirent->parent_vinode->children++;
    }

    return 0;
}

static int vtfs_vinode_fill(struct inode *inode, struct vtfs_inode *vinode) {
    vinode->used = true;
    vinode->ino = inode->i_ino;
    vinode->mode = inode->i_mode;
    vinode->nlink = inode->i_nlink;
    vinode->size = 0;
    vinode->capacity = 0;
    vinode->data = NULL;
    vinode->children = 0;

    return 0;
}

static void vtfs_dirent_list_init(struct vtfs_list_iter *iter, struct vtfs_inode *parent_vinode) {
    iter->parent_vinode = parent_vinode;
    iter->idx = 0;
}

static int vtfs_dirent_list_next(struct vtfs_list_iter *iter, struct vtfs_dirent **out) {
    for (; iter->idx < VTFS_NODES_MAX; iter->idx++) {
        struct vtfs_dirent *dirent = &vtfs_dirents[iter->idx];
        if (dirent->used == false || dirent->parent_vinode != iter->parent_vinode) {
            continue;
        }

        *out = dirent;
        iter->idx++;
        return 0;
    }
    return -ENOENT;
}

static struct inode *vtfs_get_inode(
    struct super_block *sb,
    const struct inode *dir,
    umode_t mode,
    ino_t i_ino,
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

static int vtfs_fsync(struct file *filp, loff_t start, loff_t end, int datasync) {
    return 0;
}

static int vtfs_rename(
    struct mnt_idmap *idmap,
    struct inode *old_dir,
    struct dentry *old_dentry,
    struct inode *new_dir,
    struct dentry *new_dentry,
    unsigned int flags
) {
    if (flags) {
        LOG("rename flags: %d\n", flags);
        return -EINVAL;
    }

    struct vtfs_inode *old_parent_vinode = (struct vtfs_inode *)old_dir->i_private;
    struct vtfs_inode *new_parent_vinode = (struct vtfs_inode *)new_dir->i_private;
    const char *old_name = (const char *)old_dentry->d_name.name;
    const char *new_name = (const char *)new_dentry->d_name.name;

    if (old_dentry->d_name.len > NAME_MAX || new_dentry->d_name.len > NAME_MAX) {
        return -ENAMETOOLONG;
    }

    struct vtfs_dirent *src_dirent = NULL;
    int ret = vtfs_dirent_find(old_parent_vinode, old_name, &src_dirent);
    if (ret != 0) {
        return ret;
    } 

    // main vinode; trying to move it
    struct vtfs_inode *src_vinode = src_dirent->vinode;

    struct vtfs_dirent *dst_dirent = NULL;
    ret = vtfs_dirent_find(new_parent_vinode, new_name, &dst_dirent);
    if (ret == 0) {
        struct vtfs_inode *dst_vinode = dst_dirent->vinode;
        if (S_ISDIR(src_vinode->mode) && !S_ISDIR(dst_vinode->mode)) {
            return -ENOTDIR;
        }

        if (!S_ISDIR(src_vinode->mode) && S_ISDIR(dst_vinode->mode)) {
            return -EISDIR;
        }

        if (S_ISDIR(dst_vinode->mode) && dst_vinode->children != 0) {
            ERR("dir %s already exists & not empty\n", new_name);
            return -ENOTEMPTY;
        } 

        char type = S_ISDIR(dst_dirent->vinode->mode) ? FTYPE_DIR : FTYPE_FILE;
        // trying to clear fields of dirent
        ret = vtfs_dirent_remove(dst_dirent, type);
        if (ret != 0) {
            return ret;
        }
        if (d_inode(new_dentry)) {
            set_nlink(d_inode(new_dentry), dst_vinode->nlink);
        }

        // if vinode->nlink == 0 it means
        // that removed dirent was the only pointed to this vinode 
        if (dst_vinode->nlink == 0) {
            int ret = vtfs_vinode_remove(dst_vinode);
            if (ret != 0) {
                return ret;
            }
        }
    }

    // do nothing if src & dst are equal
    if (old_parent_vinode == new_parent_vinode) {
        goto change_name;
    }

    // deleting from old
    old_parent_vinode->children--;
    // moving to new
    new_parent_vinode->children++;

    if (S_ISDIR(src_vinode->mode)) {
        // deleting from old
        old_parent_vinode->nlink--;
        set_nlink(old_dir, old_parent_vinode->nlink);

        // moving to new
        new_parent_vinode->nlink++;
        set_nlink(new_dir, new_parent_vinode->nlink);
    }

    src_dirent->parent_vinode = new_parent_vinode;

change_name:
    strscpy(src_dirent->name, new_name, sizeof(src_dirent->name));

    d_move(old_dentry, new_dentry);

    return 0;
}

static ssize_t vtfs_read(
    struct file *filp,      // файловый дескриптор
    char __user *buffer,    // буфер в user-space для чтения и записи соответственно
    size_t len,             // длина данных для записи
    loff_t *offset          // смещение
) {
    struct inode *inode = file_inode(filp);
    ssize_t bytes_read = 0;
    loff_t pos = *offset;

    if (pos >= inode->i_size || pos >= MAXFILE_SIZE) {
        return 0;
    }

    ssize_t bytes_to_read = min_t(size_t, len, inode->i_size - pos);
    struct vtfs_inode *vinode = (struct vtfs_inode *)inode->i_private;
    if (vinode->data == NULL) {
        return 0;
    }

    while (bytes_to_read > 0) {
        ssize_t ret = (ssize_t)copy_to_user(
            buffer + bytes_read,
            vinode->data + pos,
            bytes_to_read
        );
        ssize_t read = bytes_to_read - ret;
        if (read == 0) {
            break;
        }

        bytes_read += read;
        pos += read;
        bytes_to_read = ret;
    }

    *offset = pos;
    if (bytes_read > 0) {
        return bytes_read;
    }
    return -EFAULT;
}

static ssize_t vtfs_write(
    struct file *filp, 
    const char __user *buffer, 
    size_t len, 
    loff_t *offset
) {
    struct inode *inode = file_inode(filp);
    ssize_t bytes_wrote = 0;
    loff_t pos = *offset;

    if (pos > inode->i_size) {
        return -EINVAL;
    }
    if (pos >= MAXFILE_SIZE) {
        return -ENOSPC;
    }

    ssize_t bytes_to_write = min_t(size_t, len, MAXFILE_SIZE - pos);

    struct vtfs_inode *vinode = (struct vtfs_inode *)inode->i_private;
    if (vinode->data == NULL) {
        vinode->data = kmalloc(pos + bytes_to_write, GFP_KERNEL);
        if (vinode->data == NULL) {
            return -ENOMEM;
        }
        vinode->capacity = pos + bytes_to_write; // allocated size
    }

    if ((*offset + bytes_to_write) > vinode->capacity) {
        size_t need_to_alloc = (*offset + bytes_to_write) - vinode->capacity;
        vinode->data = krealloc(vinode->data, vinode->capacity + need_to_alloc, GFP_KERNEL);
        if (vinode->data == NULL) {
            return -ENOMEM;
        }
        vinode->capacity += (loff_t)need_to_alloc;
    } 

    while (bytes_to_write > 0) {
        ssize_t ret = (ssize_t)copy_from_user(
            vinode->data + pos,
            buffer + bytes_wrote,
            bytes_to_write
        );
        ssize_t written = bytes_to_write - ret;
        if (written == 0) {
            break;
        }

        bytes_wrote += written;
        pos += written;
        bytes_to_write = ret;
    }

    vinode->size = max(pos, inode->i_size);
    inode->i_size = vinode->size;
    *offset = pos;

    if (bytes_wrote > 0) {
        return bytes_wrote;
    }
    return -EFAULT;
}

static struct dentry *vtfs_lookup(
    struct inode *parent_inode,  // parent inode
    struct dentry *child_dentry, // object we try to access
    unsigned int flag            // unused value
) {
    struct vtfs_inode *parent_vinode = (struct vtfs_inode *)parent_inode->i_private;
    const char *name = (const char *)child_dentry->d_name.name;
    struct inode *inode = NULL;

    if (child_dentry->d_name.len > NAME_MAX) {
        return ERR_PTR(-ENAMETOOLONG);
    }

    struct vtfs_dirent *dirent = NULL;
    int ret = vtfs_dirent_find(parent_vinode, name, &dirent);
    if (ret == -ENOENT) {
        goto link_dentry_inode;
    } else if (ret != 0) {
        return ERR_PTR(ret);
    }

    inode = vtfs_get_inode(
        parent_inode->i_sb,
        parent_inode,
        dirent->vinode->mode,
        dirent->vinode->ino,
        NULL
    );
    if (inode == NULL) {
        return ERR_PTR(-ENOMEM);
    }

    inode->i_private = dirent->vinode;
    inode->i_size = dirent->vinode->size;
    set_nlink(inode, dirent->vinode->nlink);

link_dentry_inode:
    d_add(child_dentry, inode);
    return NULL;
}

// creates file or dir
// depends on type
static int vtfs_mkobj(
    struct mnt_idmap *idmap,
    struct inode *parent_inode,
    struct dentry *target_dentry,
    struct dentry *child_dentry,
    umode_t mode,
    char type // 0 - file; 1 - dir; 2 - link
) {
    struct vtfs_inode *parent_vinode = (struct vtfs_inode *)parent_inode->i_private;
    const char *name = (const char *)child_dentry->d_name.name;

    if (child_dentry->d_name.len > NAME_MAX) {
        return -ENAMETOOLONG;
    }

    // trying to find dirent by parent_vinode & name
    //      found -- file with this name already exists -> FAIL
    //      not found  -- file with this name doesn't exist -> OK
    struct vtfs_dirent *dirent = NULL;
    int ret = vtfs_dirent_find(parent_vinode, name, &dirent);
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

    // dirent not found -> trying to allocate it
    int slot = -1;
    ret = vtfs_dirent_alloc(&slot, &dirent);
    if (ret != 0) {
        return ret;
    }

    struct inode *inode = NULL;
    if (type == FTYPE_DIR) {
        inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFDIR | mode, ROOT_INODE_NUM + 1 + slot, idmap);
    } else if (type == FTYPE_FILE) {
        inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFREG | mode, ROOT_INODE_NUM + 1 + slot, idmap);
    } else if (type == FTYPE_HLINK) {
        // inode for hardlink already allocated
        inode = target_dentry->d_inode;
        // increases i_count
        ihold(inode);
    }
    if (inode == NULL) {
        return -ENOMEM;
    }

    // trying to get vinode
    struct vtfs_inode *vinode = NULL;
    if (type == FTYPE_HLINK) {
        // no need to allocate vinode
        // because it was previously allocated
        // when target dirent was created
        // we can get this vinode from linked inode
        vinode = (struct vtfs_inode *)inode->i_private;
    } else {
        // need to allocate vinode and link to inode
        ret = vtfs_vinode_alloc(&slot, &vinode);
        if (ret != 0) {
            dirent->used = false;
            dirent->parent_vinode = NULL;
            dirent->vinode = NULL;
            return ret;
        }
        // fill vinode fields with data from inode
        ret = vtfs_vinode_fill(inode, vinode);
        // double link: now we have ability to get struct vtfs_inode from inode
        inode->i_private = vinode;
    }

    if (type == FTYPE_FILE) {
        vinode->nlink = 1;
        set_nlink(inode, vinode->nlink);
    } else if (type == FTYPE_DIR) {
        // increment hard links count for parent_inode
        vinode->nlink = 2;
        set_nlink(inode, vinode->nlink);
        parent_vinode->nlink++;
        set_nlink(parent_inode, parent_vinode->nlink);
    } else if (type == FTYPE_HLINK) {
        // increment hard links count for inode
        vinode->nlink++;
        set_nlink(inode, vinode->nlink);
    }

    // fill dirent fields
    ret = vtfs_dirent_fill(parent_vinode, vinode, dirent, name);
    if (ret != 0) {
        return ret;
    }

    d_instantiate(child_dentry, inode);

    if (type == FTYPE_DIR) {
        LOG("created dir %s\n", name);
    } else if (type == FTYPE_FILE) {
        LOG("created file %s\n", name);
    } else if (type == FTYPE_HLINK) {
        LOG("created hard link %s\n", name);
    }
    return 0;
}

static int vtfs_mkdir(
    struct mnt_idmap *idmap,
    struct inode *parent_inode,
    struct dentry *child_dentry,
    umode_t mode
) {
    return vtfs_mkobj(idmap, parent_inode, NULL, child_dentry, mode, FTYPE_DIR);
}

static int vtfs_create(
    struct mnt_idmap *idmap,
    struct inode *parent_inode, 
    struct dentry *child_dentry, 
    umode_t mode, 
    bool b
) {
    return vtfs_mkobj(idmap, parent_inode, NULL, child_dentry, mode, FTYPE_FILE);
}

static int vtfs_link(
    struct dentry *target_dentry,
    struct inode *parent_dir,
    struct dentry *link_dentry
) {
    umode_t mode = target_dentry->d_inode->i_mode;
    // restricted to create hard link to directory; only to files
    if (S_ISDIR(mode)) {
        return -EISDIR;
    }

    return vtfs_mkobj(NULL, parent_dir, target_dentry, link_dentry, 0, FTYPE_HLINK);
}

// removes file or dir
// depends on type
static int vtfs_rmobj(
    struct inode *parent_inode,
    struct dentry *child_dentry,
    char type // 0 - file; 1 - dir; 2 - hardlink
) {
    struct vtfs_inode *parent_vinode = (struct vtfs_inode *)parent_inode->i_private;
    const char *name = (const char *)child_dentry->d_name.name;

    // trying to find dirent by parent_vinode & name
    struct vtfs_dirent *dirent = NULL;
    int ret = vtfs_dirent_find(parent_vinode, name, &dirent);
    if (ret != 0) {
        return ret;
    }

    struct vtfs_inode *vinode = dirent->vinode;
    // trying to clear fields of dirent
    ret = vtfs_dirent_remove(dirent, type);
    if (ret != 0) {
        return ret;
    }
    set_nlink(d_inode(child_dentry), vinode->nlink);

    // if vinode->nlink == 0 it means
    // that removed dirent was the only pointed to this vinode 
    if (vinode->nlink == 0) {
        int ret = vtfs_vinode_remove(vinode);
        if (ret != 0) {
            return ret;
        }
    }

    if (type == FTYPE_DIR) {
        if (parent_inode != 0) {
            parent_vinode->nlink--;
        }
        set_nlink(parent_inode, parent_vinode->nlink);
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
    struct vtfs_inode *parent_vinode = (struct vtfs_inode *)inode->i_private;
    if (parent_vinode == NULL) {
        return -EINVAL;
    }

    // skip '.' & '..'
    if (!dir_emit_dots(filp, ctx)) {
        return 0;
    }

    size_t want_skip = (ctx->pos >= 2) ? (size_t)(ctx->pos - 2) : 0;
    struct vtfs_dirent *dirent = NULL;
    struct vtfs_list_iter iter;

    vtfs_dirent_list_init(&iter, parent_vinode);
    while (want_skip > 0) {
        if (vtfs_dirent_list_next(&iter, &dirent) != 0) {
            return 0;
        }
        want_skip--;
    }

    while (vtfs_dirent_list_next(&iter, &dirent) == 0) {
        unsigned char ftype = S_ISDIR(dirent->vinode->mode) ? DT_DIR : DT_REG;
        if (!dir_emit(ctx, dirent->name, strlen(dirent->name), dirent->vinode->ino, ftype)) {
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
