/*
 *  * a demo of file_system
 *   * usage: mount -t pikaqfs none /mnt(dir)
 *    */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/spinlock.h>
#include <linux/vfs.h>
#include <linux/buffer_head.h>
#include <linux/random.h>
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/namei.h>
#include <linux/quotaops.h>
#include <asm/uaccess.h>
#include <linux/exportfs.h>
#include <linux/pagemap.h>
#include <linux/uidgid.h>

#define PIKAQFS_MAGIC 0x19900616
#define TMPSIZE 64
static kuid_t uid;
static kgid_t gid;

static struct inode *pikaqfs_make_node(struct super_block *sb, int mode)
{
    struct inode *ret = new_inode(sb);
    if (ret) {
        ret->i_mode = mode;
        ret->i_uid = uid;
        ret->i_gid = gid;
        ret->i_blocks = 0;
        ret->i_atime = ret->i_mtime = ret->i_ctime = CURRENT_TIME;
    }
    return ret;
}

static ssize_t pikaqfs_open(struct inode *inode, struct file *filp)
{
    filp->private_data = inode->i_private;
    return 0;
}

static ssize_t pikaqfs_read_file(struct file *filp, const char *buf, size_t count, loff_t *offset)
{
    atomic_t *counter = (atomic_t *)filp->private_data;
    int v, len;
    char tmp[TMPSIZE];
    
    v = atomic_read(counter);
    if (*offset > 0)
        v -= 1;
    else 
        atomic_inc(counter);
    len = snprintf(tmp, TMPSIZE, "%d\n", v);
    if (*offset > len)
        return 0;
    
    if (count > len - *offset)
        count = len - *offset;
    
    if (copy_to_user(buf, tmp + *offset, count))
        return -EFAULT;
    
    *offset += count;
    
    return count;
}

static ssize_t pikaqfs_write_file(struct file *filp, const char *buf, size_t count, loff_t *offset)
{
    atomic_t *counter = (atomic_t *)filp->private_data;
    char tmp[TMPSIZE];
    
    if (*offset != 0)
        return -EINVAL;
    
    if (count >= TMPSIZE)
        return -EINVAL;
    
    memset(tmp, 0, TMPSIZE);
    if (copy_from_user(tmp, buf, count))
        return -EFAULT;
    /*
 *      *  simple_strtol  convert a string to a signed long
 *           */
    atomic_set(counter, simple_strtol(tmp, NULL, 10));
    return count;
}

static struct file_operations pikaqfs_file_ops = {
    .open = pikaqfs_open,
    .read = pikaqfs_read_file,
    .write = pikaqfs_write_file,
};

static struct dentry *pikaqfs_create_file(struct super_block *sb, struct dentry *dir, const char *name, atomic_t *counter)
{
    struct dentry *dentry;
    struct inode *inode;
    struct qstr qname;
    
    qname.name = name;
    qname.len = strlen(name);
    qname.hash = full_name_hash(name, qname.len);
    
    dentry = d_alloc(dir, &qname);
    if (!dentry)
        goto out;
    inode = pikaqfs_make_node(sb, S_IFREG | 0644);
    if (!inode)
        goto out_dput;
    inode->i_fop = &pikaqfs_file_ops;
    inode->i_private = counter;
    
    d_add(dentry, inode);
    return dentry;
    
out_dput:
    dput(dentry);
out:
    return 0;
}

static struct dentry *pikaqfs_create_dir(struct super_block *sb, struct dentry *dir, const char *name)
{
    struct dentry *dentry;
    struct inode *inode;
    struct qstr qname;
    
    qname.name = name;
    qname.len = strlen(name);
    qname.hash = full_name_hash(name, qname.len);
    
    dentry = d_alloc(dir, &qname);
    if (!dentry)
        goto out;
    inode = pikaqfs_make_node(sb, S_IFDIR | 0644);
    if (!inode)
        goto out_dput;
    inode->i_op = &simple_dir_inode_operations;
    inode->i_fop = &simple_dir_operations;
    
    d_add(dentry, inode);
    return dentry;
    
out_dput:
    dput(dentry);
out:
    return 0;
}

static atomic_t counter, subcounter;

/*create file and dir
 *  * note: dentry including file and dir.
 *   */
static void pikaqfs_create_files(struct super_block *sb, struct dentry *root)
{
    struct dentry *subdir;
    
    atomic_set(&counter, 0);
    pikaqfs_create_file(sb, root, "mycounter", &counter);
    
    atomic_set(&subcounter, 0);
    subdir = pikaqfs_create_dir(sb, root, "mysubdir");
    if (subdir)
        pikaqfs_create_file(sb, subdir, "mysubcounter", &subcounter);
}

static struct super_operations pikaqfs_s_ops = {
    .statfs = simple_statfs,
    .drop_inode = generic_drop_inode,
};

/*file super_block*/
static int pikaqfs_fill_super(struct super_block *sb, void *data, int silent)
{
    printk("pikaqfs_fill_super is called..\n");
    
    struct inode *root;
    struct dentry *root_dentry;
    
    sb->s_blocksize = PAGE_CACHE_SIZE;
    sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
    sb->s_magic = PIKAQFS_MAGIC;
    sb->s_op = &pikaqfs_s_ops;
    
    root = pikaqfs_make_node(sb, S_IFDIR | 0755);
    if (!root)
        goto out;
    root->i_op = &simple_dir_inode_operations;
    root->i_fop = &simple_dir_operations;
    
    root_dentry = d_make_root(root);
    if (! root_dentry)
        goto out_iput;
    sb->s_root = root_dentry;
    
    pikaqfs_create_files(sb, root_dentry);
    return 0;
    
out_iput:
    iput(root);
out:
    return -ENOMEM;
}


static struct super_block *pikaqfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
    printk("mount from user!\n");
    /*mount_single -> pikaqfs_fill_super*/
	return mount_single(fs_type, flags, data, pikaqfs_fill_super);
}

static struct file_system_type pikaqfs = {
    .owner = THIS_MODULE,
	.name = "pikaqfs",
	.mount = pikaqfs_mount, /*mount*/
	.kill_sb = kill_litter_super, /*remove super_block*/
};

static int pikqfs_init(void)
{
    int ret;
    struct file_system_type *tmp;
    /*get_fs_type testcode get fs type*/
    tmp = get_fs_type("xfs");
    printk("hello pikqfs kernel module\n");
    printk("file_system name found = %s\n", tmp->name);
    
    /*register file system*/
    ret = register_filesystem(&pikaqfs);
    if (ret < 0)
        printk("register pikaqfs failed...\n");
    printk("register pikaqfs success...\n");
    return 0;
}

static void pikqfs_exit(void)
{
    printk("goodbye pikqfs kernel module\n");
    /*unregister filesystem*/
    unregister_filesystem(&pikaqfs);
    return;
}

module_init(pikqfs_init);
module_exit(pikqfs_exit);
MODULE_LICENSE("GPL");
