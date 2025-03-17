/*
 *  Copyright (C) 2024 CS416/CS518 Rutgers CS
 *	Tiny File System
 *	File:	rufs.c
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>

#include "block.h"
#include "rufs.h"

char diskfile_path[PATH_MAX];

// Declare your in-memory data structures here
#define REG 0
#define DIR 1

#define ARR_SIZE(arr) (sizeof(arr)/sizeof(arr[0]))
#define DIRENTS_BLOCK (BLOCK_SIZE/sizeof(struct dirent))
#define INODES_BLOCK (BLOCK_SIZE/sizeof(struct inode))
#define size_field(type, member) (sizeof(((type *) 0)->member))

static bitmap_t b_map;
static bitmap_t i_map;

static struct superblock __superblock_defaults = {
	.magic_num = MAGIC_NUM, 
	.max_inum = MAX_INUM,
	.max_dnum = MAX_DNUM,
	.i_bitmap_blk = 1,
	.d_bitmap_blk = 2,
	.i_start_blk = 3,
	.d_start_blk = 3 + (sizeof(struct inode) * MAX_INUM/BLOCK_SIZE)
};

static struct superblock *sblock;

static int w_get_block(int wblk_ptr, struct inode *ino);
static int r_get_block(int rblk_ptr, struct inode *ino);
static void ptr_indir(int *blk_ptr, int *iptr_ind, struct inode *i);
static void init_i(struct inode *new, int o_inode, int type);
static void init_d(struct dirent *ent_blk, int s_ino, int p_ino);
static inline int inode_block(uint16_t ino);


/*
*print log message, usually for error
*msg - message to print
*/
static void rufs_log(const char *msg, ...){
	va_list arg_print;
	va_start(arg_print, msg);
	fprintf(stdout, "---[ (rufs_log) ");
	vfprintf(stdout, msg, arg_print);
	va_end(arg_print);
	fprintf(stdout, " ]---\n");
}

/* 
 * Get available inode number from bitmap
 */
int get_avail_ino() {

	// Step 1: Read inode bitmap from disk
	// Step 2: Traverse inode bitmap to find an available slot
	// Step 3: Update inode bitmap and write to disk 

	int i_num;

	if (!bio_read(sblock->i_bitmap_blk, i_map)) {
		rufs_log("%s: Error reading inode map.", __func__);
		return -1;
	}

	for(i_num = 0; i_num < MAX_INUM; i_num++){
		if(!get_bitmap(i_map, i_num)){
			set_bitmap(i_map, i_num);
			bio_write(sblock->i_bitmap_blk, i_map);
			return i_num;
		}
	}

	return -1;
}

/* 
 * Get available data block number from bitmap
 */
int get_avail_blkno() {

	// Step 1: Read data block bitmap from disk
	// Step 2: Traverse data block bitmap to find an available slot
	// Step 3: Update data block bitmap and write to disk 


	int b_num;

	if (!bio_read(sblock->d_bitmap_blk, b_map)) {
		rufs_log("%s: Error reading block map.\n", __func__);
		return -1;
	}

	for (b_num = 0; b_num < MAX_DNUM; b_num++){
		if (!get_bitmap(b_map, b_num)) {
			set_bitmap(b_map, b_num);
			bio_write(sblock->d_bitmap_blk, b_map);
			return b_num + sblock->d_start_blk;
		}
	}

	return -1;
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {

  // Step 1: Get the inode's on-disk block number
  // Step 2: Get offset of the inode in the inode on-disk block
  // Step 3: Read the block from disk and then copy into inode structure

	struct inode *blk;
	int i_blk_ind, ofst;

	blk = malloc(BLOCK_SIZE);
	if (!blk) {
		return -ENOMEM;
	}

	i_blk_ind = sblock->i_start_blk + inode_block(ino);

	ofst = ino % INODES_BLOCK;

	bio_read(i_blk_ind, blk);
	*inode = blk[ofst];
	free(blk);

	return 0;
}

int writei(uint16_t ino, struct inode *inode) {

	// Step 1: Get the block number where this inode resides on disk
	// Step 2: Get the offset in the block where this inode resides on disk
	// Step 3: Write inode to disk 

	struct inode *blk;
	int i_blk_ind, ofst;

	blk = malloc(BLOCK_SIZE);

	if(!blk){
		return -ENOMEM;
	}

	i_blk_ind = sblock->i_start_blk + inode_block(ino);

	ofst = ino % INODES_BLOCK;

	bio_read(i_blk_ind, blk);
	blk[ofst] = *inode;
	bio_write(i_blk_ind, blk);
	free(blk);

	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {

  // Step 1: Call readi() to get the inode using ino (inode number of current directory)
  // Step 2: Get data block of current directory from inode
  // Step 3: Read directory's data block and check each directory entry.
  //If the name matches, then copy directory entry to dirent structure

  struct inode dir_n;
  struct dirent *ent;
  int blk_ptr;

  if (readi(ino, &dir_n) < 0) {
	rufs_log("%s: Error finding inode for ino (%d)", __func__, ino);
	return -ENOENT;
  }

  ent = malloc(BLOCK_SIZE);

  if(!ent) {
	return -ENOMEM;
  }

  for (blk_ptr = 0; blk_ptr < ARR_SIZE(dir_n.direct_ptr); blk_ptr++) {
	struct dirent *ent_par = ent;
	int ent_ind;

	if (!dir_n.direct_ptr[blk_ptr]) {
		break;
	}

	if (!bio_read(dir_n.direct_ptr[blk_ptr], ent)) {
		break;
	}

	for (ent_ind = 0; ent_ind < DIRENTS_BLOCK; ent_ind++, ent_par++) {
		if(ent_par->valid && !strcmp(ent_par->name, fname)) {
			*dirent = *ent_par;
			free(ent);
			return 0;
		}
	}
  }

  free(ent);
  return -ENOENT;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	// Step 2: Check if fname (directory name) is already used in other entries
	// Step 3: Add directory entry in dir_inode's data block and write to disk
	// Allocate a new data block for this directory if it does not exist
	// Update directory inode
	// Write directory entry

	struct dirent de, *ent;
	int blk_ptr;

	if(!dir_find(dir_inode.ino, fname, name_len, &de)) {
		rufs_log("%s: file '%s' already exists.", __func__, fname);
		return -EEXIST;
	}

	ent = malloc(BLOCK_SIZE);
	
	if(!ent) {
		return -ENOMEM;
	}

	for(blk_ptr = 0; blk_ptr < ARR_SIZE(dir_inode.direct_ptr); blk_ptr++) {
		struct dirent *ent_par = ent;
		int ent_ind;

		if (!dir_inode.direct_ptr[blk_ptr]) {
			dir_inode.direct_ptr[blk_ptr] = get_avail_blkno();
			dir_inode.vstat.st_blocks++;
		}

		if (!bio_read(dir_inode.direct_ptr[blk_ptr], ent)) {
			break;
		}

		for (ent_ind = 0; ent_ind < DIRENTS_BLOCK; ent_ind++, ent_par++) {
			if (!ent_par->valid) {
				ent_par->valid = 1;
				ent_par->ino = f_ino;
				strcpy(ent_par->name, fname);
				dir_inode.size += sizeof(struct dirent);
				dir_inode.vstat.st_size += sizeof(struct dirent);
				time(&dir_inode.vstat.st_mtime);

				writei(dir_inode.ino, &dir_inode);
				bio_write(dir_inode.direct_ptr[blk_ptr], ent);
				free(ent);
				return 0;
			}
		}

	}

	free(ent);
	return -ENOSPC;
}

// Required for 518
int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
	// Step 2: Check if fname exist
	// Step 3: If exist, then remove it from dir_inode's data block and write to disk

	return 0;
}

/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way

	struct dirent de = {0};
	char *dup, *walker, *tofree;

	if (!strcmp(path, "/")) {
		return readi(0, inode);
	}

	dup = strdup(path);

	if (!dup) {
		return -ENOMEM;
	}

	tofree = dup;

	while ((walker = strsep(&dup, "/")) != NULL) {
		if (*walker && dir_find(de.ino, walker, strlen(walker), &de) < 0) {
			free(tofree);
			return -ENOENT;
		}
	}

	free(tofree);
	return readi(de.ino, inode);
}

/* 
 * Make file system
 */
int rufs_mkfs() {

	// Call dev_init() to initialize (Create) Diskfile
	// write superblock information
	// initialize inode bitmap
	// initialize data block bitmap
	// update bitmap information for root directory
	// update inode for root directory

	struct inode *iroot;
	struct stat *sroot;
	struct dirent *droot;
	int i;

	dev_init(diskfile_path);

	sblock = malloc(BLOCK_SIZE);

	if(!sblock) {
		rufs_log("%s: Error allocating memory for superblock.", __func__);
		return -ENOMEM;
	}

	*sblock = __superblock_defaults;

	bio_write(0, sblock);

	i_map = calloc(1, BLOCK_SIZE);
	b_map = calloc(1, BLOCK_SIZE);

	if(!i_map || !b_map) {
		rufs_log("%s: Error allocating memory for bitmaps.", __func__);
		return -ENOMEM;
	}

	set_bitmap(i_map, 0);
	set_bitmap(b_map, 0);
	bio_write(sblock->i_bitmap_blk, i_map);
	bio_write(sblock->d_bitmap_blk, b_map);

	iroot = malloc(BLOCK_SIZE);
	droot = malloc(BLOCK_SIZE);

	if(!iroot || !droot) {
		rufs_log("%s: Error initializing root inode and entries.", __func__);
		return -ENOMEM;
	}

	iroot->ino = 0;
	iroot->valid = 1;
	iroot->size = sizeof(struct dirent) * 2;
	iroot->type = DIR;
	iroot->link = 2;
	iroot->direct_ptr[0] = sblock->d_start_blk;

	for (i = 1; i < ARR_SIZE(iroot->direct_ptr); i++) {
		iroot->direct_ptr[i] = 0;
	}

	for (i = 0; i < ARR_SIZE(iroot->indirect_ptr); i++) {
		iroot->indirect_ptr[i] = 0;
	}

	sroot = &iroot->vstat;
	sroot->st_mode = S_IFDIR | 0755;
	sroot->st_nlink = 2;
	sroot->st_blocks = 1;
	sroot->st_blksize = BLOCK_SIZE;

	init_d(droot, 0, 0);

	bio_write(sblock->i_start_blk, iroot);
	bio_write(sblock->d_start_blk, droot);
	
	free(iroot);
	free(droot);

	return 0;
}

/* 
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn) {

	// Step 1a: If disk file is not found, call mkfs
	// Step 1b: If disk file is found, just initialize in-memory data structures
	// and read superblock from disk

	if (dev_open(diskfile_path) < 0) {
		rufs_mkfs();
		return NULL;
	}

	sblock = malloc(BLOCK_SIZE);
	i_map = malloc(BLOCK_SIZE);
	b_map = malloc(BLOCK_SIZE);

	if(!sblock || !i_map || !b_map) {
		rufs_log("%s: Error initializing in memory structures.", __func__);
	}

	if(!bio_read(0, sblock) || !bio_read(sblock->i_bitmap_blk, i_map) || !bio_read(sblock->d_bitmap_blk, b_map)) {
		rufs_log("%s: Error reading block structures.", __func__);
	}

	return NULL;
}

static void rufs_destroy(void *userdata) {

	// Step 1: De-allocate in-memory data structures
	// Step 2: Close diskfile

	if (sblock) {
		free(sblock);
	}

	if(i_map) {
		free(i_map);
	}

	if(b_map) {
		free(b_map);
	}

	dev_close();
}

static int rufs_getattr(const char *path, struct stat *stbuf) {

	// Step 1: call get_node_by_path() to get inode from path
	// Step 2: fill attribute of file into stbuf from inode

	struct inode n;

	if (get_node_by_path(path, 0, &n)) {
		return -ENOENT;
	}

	*stbuf = n.vstat;

	return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
	// Step 2: If not find, return -1

	struct inode dnode;

	return get_node_by_path(path, 0, &dnode) ? -1 : 0;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
	// Step 2: Read directory entries from its data blocks, and copy them to filler

	struct inode dnode;
	struct dirent *ent;
	int i;

	if (get_node_by_path(path, 0, &dnode) < 0) {
		return -ENOENT;
	}

	ent = malloc(BLOCK_SIZE);
	if (!ent) {
		return -ENOMEM;
	}

	for (i = 0; i < ARR_SIZE(dnode.direct_ptr); i++) {
		struct dirent *ent_par = ent;
		int ent_ind;

		if (!dnode.direct_ptr[i]) {
			break;
		}

		if(!bio_read(dnode.direct_ptr[i], ent)) {
			break;
		}

		for (ent_ind = 0; ent_ind < DIRENTS_BLOCK; ent_ind++, ent_par++) {
			if (ent_par->valid) {
				struct inode read;
				readi(ent_par->ino, &read);
				filler(buffer, ent_par->name, &read.vstat, 0);
			}
		}
	}

	free(ent);

	return 0;
}


static int rufs_mkdir(const char *path, mode_t mode) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	// Step 2: Call get_node_by_path() to get inode of parent directory
	// Step 3: Call get_avail_ino() to get an available inode number
	// Step 4: Call dir_add() to add directory entry of target directory to parent directory
	// Step 5: Update inode for target directory
	// Step 6: Call writei() to write inode to disk
	
	struct inode pdir_n, ndir_n;
	struct dirent *ndir;
	struct stat *ndir_st = &ndir_n.vstat;
	time_t ctime;
	char *tar, *p, *dirc, *basec;
	int open, err;

	dirc = strdup(path);
	basec = strdup(path);
	ndir = calloc(1, BLOCK_SIZE);

	if(!basec || !dirc || !ndir) {
		return -ENOMEM;
	}

	tar = basename(basec);
	p = dirname(dirc);

	if(strlen(tar) >= size_field(struct dirent, name)) {
		err = -ENAMETOOLONG;
		goto out;
	}

	err = get_node_by_path(p, 0, &pdir_n);

	if (err) {
		goto out;
	}

	open = get_avail_ino();

	if (open < 0) {
		err = -ENOSPC;
		goto out;
	}

	err = dir_add(pdir_n, open, tar, strlen(tar));

	if (err) {
		goto out;
	}

	init_i(&ndir_n, open, DIR);

	ndir_n.size = sizeof(struct dirent)*2;
	ndir_st->st_mode = S_IFDIR | mode;
	ndir_st->st_nlink = 2;
	ndir_st->st_ino = open;
	ndir_st->st_blocks = 1;
	ndir_st->st_blksize = BLOCK_SIZE;
	ndir_st->st_size = ndir_n.size;

	time(&ctime);

	ndir_st->st_atime = ctime;
	ndir_st->st_mtime = ctime;
	ndir_st->st_uid = getuid();
	ndir_st->st_gid = getgid();

	init_d(ndir, open, pdir_n.ino);
	writei(open, &ndir_n);
	bio_write(ndir_n.direct_ptr[0], ndir);

	out:
		free(dirc);
		free(basec);
		free(ndir);
		return err;
}

// Required for 518
static int rufs_rmdir(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	// Step 2: Call get_node_by_path() to get inode of target directory
	// Step 3: Clear data block bitmap of target directory
	// Step 4: Clear inode bitmap and its data block
	// Step 5: Call get_node_by_path() to get inode of parent directory
	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory

	return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name
	// Step 2: Call get_node_by_path() to get inode of parent directory
	// Step 3: Call get_avail_ino() to get an available inode number
	// Step 4: Call dir_add() to add directory entry of target file to parent directory
	// Step 5: Update inode for target file
	// Step 6: Call writei() to write inode to disk

	struct inode p, tar_i;
	struct stat *tar_st = &tar_i.vstat;
	char *dir, *tar, *dirc, *basec;
	time_t ctime;
	int open, err;

	basec = strdup(path);
	dirc = strdup(path);

	if (!basec || !dirc) {
		return -ENOMEM;
	}

	dir = dirname(dirc);
	tar = basename(basec);

	if (strlen(tar) >= size_field(struct dirent, name)) {
		err = ENAMETOOLONG;
		goto out;
	}

	err = get_node_by_path(dir, 0, &p);

	if (err) {
		goto out;
	}

	open = get_avail_ino();

	err = dir_add(p, open, tar, strlen(tar));

	if (err) {
		rufs_log("%s: Failed to add %s to %s.", __func__, tar, dir);
		goto out;
	}

	init_i(&tar_i, open, REG);

	tar_i.size = 0;
	tar_st->st_mode = S_IFREG | mode;
	tar_st->st_nlink = 1;
	tar_st->st_ino = open;
	tar_st->st_size = 0;
	tar_st->st_blocks = 1;

	time(&ctime);

	tar_st->st_atime = ctime;
	tar_st->st_mtime = ctime;
	tar_st->st_gid = getgid();
	tar_st->st_uid = getuid();

	writei(open, &tar_i);

	out: 
		free(dirc);
		free(basec);
		return err;
}

static int rufs_open(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
	// Step 2: If not find, return -1

	struct inode fnode;

	return get_node_by_path(path, 0, &fnode) ? -1 : 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	// Step 1: You could call get_node_by_path() to get inode from path
	// Step 2: Based on size and offset, read its data blocks from disk
	// Step 3: copy the correct amount of data from offset to buffer
	// Note: this function should return the amount of bytes you copied to buffer

	struct inode fnode;
	char *buf;
	int b_read, to_end;

	if (get_node_by_path(path, 0, &fnode) < 0) {
		return -ENOENT;
	}

	buf = malloc(BLOCK_SIZE);

	if (!buf) {
		return -ENOMEM;
	}

	to_end = fnode.vstat.st_size - offset;

	for (b_read = 0; b_read < size && b_read < to_end;) {
		int rblock, count = 0, rblock_ptr = offset/BLOCK_SIZE;
		char *r = buf + (offset % BLOCK_SIZE);

		rblock = r_get_block(rblock_ptr, &fnode);

		if (rblock < 0) {
			break;
		}

		if (!bio_read(rblock, buf)) {
			break;
		}

		while (b_read < size && b_read < to_end && count < BLOCK_SIZE) {
			*buffer++ = *r++;
			count++;
			b_read++;
			offset++;
		}
	}

	free(buf);

	return b_read;
}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	// Step 1: You could call get_node_by_path() to get inode from path
	// Step 2: Based on size and offset, read its data blocks from disk
	// Step 3: Write the correct amount of data from offset to disk
	// Step 4: Update the inode info and write it to disk
	// Note: this function should return the amount of bytes you write to disk

	struct inode finode;
	char *buf;
	int i, max, written = 0;

	if (get_node_by_path(path, 0, &finode) < 0) {
		return -ENOENT;
	}

	buf = malloc(BLOCK_SIZE);

	if (!buf) {
		return -ENOMEM;
	}

	max = offset + size;

	for (i = offset; i < max; i++) {
		int wblk_ptr, wblk, count;
		char *writer = buf + (i % BLOCK_SIZE);

		wblk_ptr = i/BLOCK_SIZE;
		wblk = w_get_block(wblk_ptr, &finode);

		if (wblk < 0) {
			break;
		}

		bio_read(wblk, buf);

		for (count = 0; i < max && count < BLOCK_SIZE; i++, count++) {
			*writer++ = *buffer++;
			written++;
		}

		bio_write(wblk, buf);
	}

	free(buf);

	if (i > finode.size) {
		finode.size = i - 1;
		finode.vstat.st_size = i - 1;
	}

	if (written) {
		time(&finode.vstat.st_mtime);
	}

	writei(finode.ino, &finode);

	return written;
}

//convert inode to block
static inline int inode_block(uint16_t ino) {
	return (ino * sizeof(struct inode)) / BLOCK_SIZE;
}

/*
 * initializes the entries of entry block
 * creates "." and ".." files and links to inodes.
 */
static void init_d(struct dirent *ent_blk, int s_ino, int p_ino) {
	ent_blk->ino = s_ino;
	ent_blk->valid = 1;
	strcpy(ent_blk->name, ".");
	ent_blk++;
	ent_blk->ino = p_ino;
	ent_blk->valid = 1;
	strcpy(ent_blk->name, "..");
}

/*
 * initalizes an inode by assigning it an open_inode, grabbing an
 * open data block, and assigning it a type.
 */
static void init_i(struct inode *new, int o_inode, int type) {
	int i;

	new->ino = o_inode;
	new->valid = 1;
	new->link = type == REG ? 1 : 2;
	new->direct_ptr[0] = get_avail_blkno();

	for (i = 1; i < ARR_SIZE(new->direct_ptr); i++) {
		new->direct_ptr[i] = 0;
	}
		
	for (i = 0; i < ARR_SIZE(new->indirect_ptr); i++) {
		new->indirect_ptr[i] = 0;
	}
		
	new->type = type;
}

/*
 *converts block pointer to indirect pointers
 */
static void ptr_indir(int *blk_ptr, int *iptr_ind, struct inode *i) {
	int ptr = *blk_ptr, ind = *iptr_ind;
	ptr -= ARR_SIZE(i->direct_ptr);
	ind = ptr % (BLOCK_SIZE / sizeof(int));
	ptr /= BLOCK_SIZE / sizeof(int);
	*blk_ptr = ptr;
	*iptr_ind = ind;
}

/*
 * internal function for rufs_read to get next data block
 */
static int r_get_block(int rblk_ptr, struct inode *ino) {
	int blk_ptr;

	if (rblk_ptr >= ARR_SIZE(ino->direct_ptr)) {
		int ind, *buf;

		ptr_indir(&rblk_ptr, &ind, ino);
		buf = malloc(BLOCK_SIZE);

		if (!buf) {
			return -ENOMEM;
		}

		bio_read(ino->indirect_ptr[rblk_ptr], buf);
		blk_ptr = buf[ind];
		free(buf);

	} 
	
	else {
		blk_ptr = ino->direct_ptr[rblk_ptr];
	}

	return blk_ptr;
}

/*
 *internal function to rufs_write to get next data block
 */
static int w_get_block(int wblk_ptr, struct inode *ino) {
	int blk_ptr;

	if (wblk_ptr >= ARR_SIZE(ino->direct_ptr)) {
		int iptr_ind, *iptr, *buf;

		ptr_indir(&wblk_ptr, &iptr_ind, ino);
		buf = malloc(BLOCK_SIZE);

		if (!buf) {
			return -ENOMEM;
		}
		
		if (!ino->indirect_ptr[wblk_ptr]) {
			ino->indirect_ptr[wblk_ptr] = get_avail_blkno();
			memset(buf, 0, BLOCK_SIZE);
		} 
		
		else {
			bio_read(ino->indirect_ptr[wblk_ptr], buf);
		}

		iptr = &buf[iptr_ind];
		if (!*iptr) {
			*iptr = get_avail_blkno();
			ino->vstat.st_blocks++;
			bio_write(ino->indirect_ptr[wblk_ptr], buf);
		}

		blk_ptr = *iptr;
		free(buf);
	} 
	
	else {
		if (!ino->direct_ptr[wblk_ptr]) {
			ino->direct_ptr[wblk_ptr] = get_avail_blkno();
			ino->vstat.st_blocks++;
		}

		blk_ptr = ino->direct_ptr[wblk_ptr];
	}

	return blk_ptr;
}

// Required for 518
static int rufs_unlink(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name
	// Step 2: Call get_node_by_path() to get inode of target file
	// Step 3: Clear data block bitmap of target file
	// Step 4: Clear inode bitmap and its data block
	// Step 5: Call get_node_by_path() to get inode of parent directory
	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory

	return 0;
}

static int rufs_truncate(const char *path, off_t size) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_flush(const char * path, struct fuse_file_info * fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2]) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}


static struct fuse_operations rufs_ope = {
	.init		= rufs_init,
	.destroy	= rufs_destroy,

	.getattr	= rufs_getattr,
	.readdir	= rufs_readdir,
	.opendir	= rufs_opendir,
	.releasedir	= rufs_releasedir,
	.mkdir		= rufs_mkdir,
	.rmdir		= rufs_rmdir,

	.create		= rufs_create,
	.open		= rufs_open,
	.read 		= rufs_read,
	.write		= rufs_write,
	.unlink		= rufs_unlink,

	.truncate   = rufs_truncate,
	.flush      = rufs_flush,
	.utimens    = rufs_utimens,
	.release	= rufs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

	return fuse_stat;
}

