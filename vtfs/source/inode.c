#include <linux/dcache.h>
#include <linux/fs.h>

#include "store.h"
#include "vtfs.h"

struct inode *vtfs_get_inode(
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
        inode = vtfs_get_inode(
            parent_inode->i_sb,
            NULL,
            S_IFDIR | mode,
            ROOT_INODE_NUM + 1 + slot,
            idmap
        );
    } else if (type == FTYPE_FILE) {
        inode = vtfs_get_inode(
            parent_inode->i_sb,
            NULL,
            S_IFREG | mode,
            ROOT_INODE_NUM + 1 + slot,
            idmap
        );
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

static int
vtfs_link(struct dentry *target_dentry, struct inode *parent_dir, struct dentry *link_dentry) {
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

const struct inode_operations vtfs_inode_ops = {
    .lookup = vtfs_lookup,
    .create = vtfs_create,
    .unlink = vtfs_unlink,
    .mkdir = vtfs_mkdir,
    .rmdir = vtfs_rmdir,
    .link = vtfs_link,
    .rename = vtfs_rename
};