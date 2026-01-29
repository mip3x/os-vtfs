#include <linux/fs.h>

#include "store.h"
#include "vtfs.h"

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

const struct file_operations vtfs_dir_ops = {
    .iterate_shared = vtfs_iterate,
};
