#pragma once

#include "vtfs.h"

// store group
int vtfs_dirent_find(struct vtfs_inode *parent_vinode, const char *name, struct vtfs_dirent **out);
int vtfs_dirent_alloc(int *slot, struct vtfs_dirent **out);
int vtfs_vinode_alloc(int *slot, struct vtfs_inode **out);
int vtfs_dirent_fill(struct vtfs_inode *parent_vinode, struct vtfs_inode *vinode, struct vtfs_dirent *dirent, const char *name);
int vtfs_vinode_fill(struct inode *inode, struct vtfs_inode *vinode);
int vtfs_dirent_remove(struct vtfs_dirent *dirent, char type);
int vtfs_vinode_remove(struct vtfs_inode *vinode);
void vtfs_dirent_list_init(struct vtfs_list_iter *iter, struct vtfs_inode *parent_vinode);
int vtfs_dirent_list_next(struct vtfs_list_iter *iter, struct vtfs_dirent **out);
