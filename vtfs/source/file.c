#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "vtfs.h"

static ssize_t vtfs_read(
    struct file *filp,      // file descriptor
    char __user *buffer,    // buffer in user-space for reading (and writing for vtfs_write)
    size_t len,             // length of data to read
    loff_t *offset          // offset
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

// do nothing because of ramfs
static int vtfs_fsync(struct file *filp, loff_t start, loff_t end, int datasync) {
    return 0;
}

const struct file_operations vtfs_file_ops = {
    .read = vtfs_read,
    .write = vtfs_write,
    .fsync = vtfs_fsync
};