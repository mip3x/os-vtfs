#include "store.h"

#include <linux/string.h>

#include "vtfs.h"

struct vtfs_dirent vtfs_dirents[VTFS_NODES_MAX];
struct vtfs_inode vtfs_inodes[VTFS_NODES_MAX];

int vtfs_dirent_find(struct vtfs_inode *parent_vinode, const char *name, struct vtfs_dirent **out) {
    for (size_t i = 0; i < VTFS_NODES_MAX; i++) {
        if (vtfs_dirents[i].used == true && vtfs_dirents[i].parent_vinode == parent_vinode &&
            !strcmp(vtfs_dirents[i].name, name)) {
            *out = &vtfs_dirents[i];
            return 0;
        }
    }
    return -ENOENT;
}

int vtfs_dirent_remove(struct vtfs_dirent *dirent, char type) {
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

int vtfs_vinode_remove(struct vtfs_inode *vinode) {
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

int vtfs_dirent_alloc(int *slot, struct vtfs_dirent **out) {
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

int vtfs_vinode_alloc(int *slot, struct vtfs_inode **out) {
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

int vtfs_dirent_fill(
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

int vtfs_vinode_fill(struct inode *inode, struct vtfs_inode *vinode) {
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

void vtfs_dirent_list_init(struct vtfs_list_iter *iter, struct vtfs_inode *parent_vinode) {
    iter->parent_vinode = parent_vinode;
    iter->idx = 0;
}

int vtfs_dirent_list_next(struct vtfs_list_iter *iter, struct vtfs_dirent **out) {
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