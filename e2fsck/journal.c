/*
 * journal.c --- code for handling the "ext3" journal
 *
 * Copyright (C) 2000 Andreas Dilger
 * Copyright (C) 2000 Theodore Ts'o
 *
 * Parts of the code are based on fs/jfs/journal.c by Stephen C. Tweedie
 * Copyright (C) 1999 Red Hat Software
 *
 * This file may be redistributed under the terms of the
 * GNU General Public License version 2 or at your discretion
 * any later version.
 */

#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#define MNT_FL (MS_MGC_VAL | MS_RDONLY)
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#define E2FSCK_INCLUDE_INLINE_FUNCS
#include "jfs_user.h"
#include "problem.h"
#include "uuid/uuid.h"

#ifdef JFS_DEBUG
static int bh_count = 0;
int journal_enable_debug = 0;
#endif

/* Kernel compatibility functions for handling the journal.  These allow us
 * to use the recovery.c file virtually unchanged from the kernel, so we
 * don't have to do much to keep kernel and user recovery in sync.
 */
int bmap(struct inode *inode, int block)
{
	int retval;
	blk_t phys;

	retval = ext2fs_bmap(inode->i_ctx->fs, inode->i_ino, &inode->i_ext2,
			     NULL, 0, block, &phys);

	if (retval)
		com_err(inode->i_ctx->device_name, retval,
			_("bmap journal inode %ld, block %d\n"),
			inode->i_ino, block);

	return phys;
}

struct buffer_head *getblk(e2fsck_t ctx, blk_t blocknr, int blocksize)
{
	struct buffer_head *bh;

	bh = e2fsck_allocate_memory(ctx, sizeof(*bh), "block buffer");
	if (!bh)
		return NULL;

	jfs_debug(4, "getblk for block %lu (%d bytes)(total %d)\n",
		  (unsigned long) blocknr, blocksize, ++bh_count);

	bh->b_ctx = ctx;
	bh->b_size = blocksize;
	bh->b_blocknr = blocknr;

	return bh;
}

void ll_rw_block(int rw, int nr, struct buffer_head *bhp[])
{
	int retval;
	struct buffer_head *bh;

	for (; nr > 0; --nr) {
		bh = *bhp++;
		if (rw == READ && !bh->b_uptodate) {
			jfs_debug(3, "reading block %lu/%p\n", 
				  (unsigned long) bh->b_blocknr, (void *) bh);
			retval = io_channel_read_blk(bh->b_ctx->fs->io, 
						     bh->b_blocknr,
						     1, bh->b_data);
			if (retval) {
				com_err(bh->b_ctx->device_name, retval,
					"while reading block %ld\n", 
					bh->b_blocknr);
				bh->b_err = retval;
				continue;
			}
			bh->b_uptodate = 1;
		} else if (rw == WRITE && bh->b_dirty) {
			jfs_debug(3, "writing block %lu/%p\n", 
				  (unsigned long) bh->b_blocknr, (void *) bh);
			retval = io_channel_write_blk(bh->b_ctx->fs->io, 
						      bh->b_blocknr,
						      1, bh->b_data);
			if (retval) {
				com_err(bh->b_ctx->device_name, retval,
					"while writing block %ld\n", 
					bh->b_blocknr);
				bh->b_err = retval;
				continue;
			}
			bh->b_dirty = 0;
			bh->b_uptodate = 1;
		} else
			jfs_debug(3, "no-op %s for block %lu\n",
				  rw == READ ? "read" : "write", 
				  (unsigned long) bh->b_blocknr);
	}
}

void mark_buffer_dirty(struct buffer_head *bh, int dummy)
{
	bh->b_dirty = dummy | 1; /* use dummy to avoid unused variable */
}

void brelse(struct buffer_head *bh)
{
	if (bh->b_dirty)
		ll_rw_block(WRITE, 1, &bh);
	jfs_debug(3, "freeing block %lu/%p (total %d)\n",
		  (unsigned long) bh->b_blocknr, (void *) bh, --bh_count);
	ext2fs_free_mem((void **) &bh);
}

int buffer_uptodate(struct buffer_head *bh)
{
	return bh->b_uptodate;
}

void wait_on_buffer(struct buffer_head *bh)
{
	if (!bh->b_uptodate)
		ll_rw_block(READ, 1, &bh);
}


static void e2fsck_clear_recover(e2fsck_t ctx, int error)
{
	ctx->fs->super->s_feature_incompat &= ~EXT3_FEATURE_INCOMPAT_RECOVER;

	/* if we had an error doing journal recovery, we need a full fsck */
	if (error)
		ctx->fs->super->s_state &= ~EXT2_VALID_FS;
	ext2fs_mark_super_dirty(ctx->fs);
}

static int e2fsck_journal_init_inode(e2fsck_t ctx,
				     struct ext2_super_block *s,
				     ext2_ino_t journal_inum,
				     journal_t **journal)
{
	struct inode *inode;
	struct buffer_head *bh;
	blk_t start;
	int retval;

	jfs_debug(1, "Using journal inode %u\n", journal_inum);
	*journal = e2fsck_allocate_memory(ctx, sizeof(journal_t), "journal");
	if (!*journal) {
		return EXT2_ET_NO_MEMORY;
	}

	inode = e2fsck_allocate_memory(ctx, sizeof(*inode), "journal inode");
	if (!inode) {
		retval = EXT2_ET_NO_MEMORY;
		goto exit_journal;
	}

	inode->i_ctx = ctx;
	inode->i_ino = journal_inum;
	retval = ext2fs_read_inode(ctx->fs, journal_inum, &inode->i_ext2);
	if (retval)
		goto exit_inode;

	(*journal)->j_dev = ctx;
	(*journal)->j_inode = inode;
	(*journal)->j_blocksize = ctx->fs->blocksize;
	(*journal)->j_maxlen = inode->i_ext2.i_size / (*journal)->j_blocksize;

	if (!inode->i_ext2.i_links_count ||
	    !LINUX_S_ISREG(inode->i_ext2.i_mode) ||
	    (*journal)->j_maxlen < JFS_MIN_JOURNAL_BLOCKS ||
	    (start = bmap(inode, 0)) == 0) {
		retval = EXT2_ET_BAD_INODE_NUM;
		goto exit_inode;
	}

	bh = getblk(ctx, start, (*journal)->j_blocksize);
	if (!bh) {
		retval = EXT2_ET_NO_MEMORY;
		goto exit_inode;
	}
	(*journal)->j_sb_buffer = bh;
	(*journal)->j_superblock = (journal_superblock_t *)bh->b_data;

	return 0;

exit_inode:
	ext2fs_free_mem((void **)&inode);
exit_journal:
	ext2fs_free_mem((void **)journal);

	return retval;
}

static int e2fsck_get_journal(e2fsck_t ctx, journal_t **journal)
{
	char uuid_str[40];
	struct problem_context pctx;
	struct ext2_super_block *sb = ctx->fs->super;

	clear_problem_context(&pctx);

	if (sb->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) {
		if (sb->s_journal_dev) {
			pctx.num = sb->s_journal_dev;
			/* this problem aborts on -y, -p, unsupported on -n */
			if (!fix_problem(ctx, PR_0_JOURNAL_UNSUPP_DEV, &pctx))
				return EXT2_ET_UNSUPP_FEATURE;
			sb->s_journal_dev = 0;
			sb->s_state &= ~EXT2_VALID_FS;
			ext2fs_mark_super_dirty(ctx->fs);
		}
		if (!uuid_is_null(sb->s_journal_uuid)) {
			uuid_unparse(sb->s_journal_uuid, uuid_str);
			pctx.str = uuid_str;
			/* this problem aborts on -y, -p, unsupported on -n */
			if (!fix_problem(ctx, PR_0_JOURNAL_UNSUPP_UUID, &pctx))
				return EXT2_ET_UNSUPP_FEATURE;
			uuid_clear(sb->s_journal_uuid);
			sb->s_state &= ~EXT2_VALID_FS;
			ext2fs_mark_super_dirty(ctx->fs);
		}
		if (!sb->s_journal_inum)
			return EXT2_ET_BAD_INODE_NUM;
	}

	if (sb->s_journal_dev) {
		pctx.num = sb->s_journal_dev;
		if (!fix_problem(ctx, PR_0_JOURNAL_BAD_DEV, &pctx))
			return EXT2_ET_UNSUPP_FEATURE;
		sb->s_journal_dev = 0;
		sb->s_state &= ~EXT2_VALID_FS;
		ext2fs_mark_super_dirty(ctx->fs);
	}
	if (!uuid_is_null(sb->s_journal_uuid)) {
		uuid_unparse(sb->s_journal_uuid, uuid_str);
		pctx.str = uuid_str;
		if (!fix_problem(ctx, PR_0_JOURNAL_BAD_UUID, &pctx))
			return EXT2_ET_UNSUPP_FEATURE;
		uuid_clear(sb->s_journal_uuid);
		sb->s_state &= ~EXT2_VALID_FS;
		ext2fs_mark_super_dirty(ctx->fs);
	}

	return e2fsck_journal_init_inode(ctx, sb, sb->s_journal_inum, journal);
}

static int e2fsck_journal_fix_bad_inode(e2fsck_t ctx,
					struct problem_context *pctx)
{
	struct ext2_super_block *sb = ctx->fs->super;
	int recover = ctx->fs->super->s_feature_incompat &
		EXT3_FEATURE_INCOMPAT_RECOVER;
	int has_journal = ctx->fs->super->s_feature_compat &
		EXT3_FEATURE_COMPAT_HAS_JOURNAL;

	if (has_journal || sb->s_journal_inum) {
		/* The journal inode is bogus, remove and force full fsck */
		if (fix_problem(ctx, PR_0_JOURNAL_BAD_INODE, pctx)) {
			if (has_journal && sb->s_journal_inum)
				printf("*** ext3 journal has been deleted - "
				       "filesystem is now ext2 only ***\n\n");
			sb->s_feature_compat &= ~EXT3_FEATURE_COMPAT_HAS_JOURNAL;
			sb->s_journal_inum = 0;
			e2fsck_clear_recover(ctx, 1);
			return 0;
		}
		return EXT2_ET_BAD_INODE_NUM;
	} else if (recover) {
		if (fix_problem(ctx, PR_0_JOURNAL_RECOVER_SET, pctx)) {
			e2fsck_clear_recover(ctx, 1);
			return 0;
		}
		return EXT2_ET_UNSUPP_FEATURE;
	}
	return 0;
}

static int e2fsck_journal_fix_unsupported_super(e2fsck_t ctx,
						struct problem_context *pctx)
{
	struct ext2_super_block *sb = ctx->fs->super;

	/* Unsupported journal superblock - first choice is abort.
	 * Declining that gives the option to reset the superblock.
	 *
	 * Otherwise we get the chance to delete the journal, and
	 * failing that we abort because we can't handle this.
	 */
	if (sb->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL &&
	    fix_problem(ctx, PR_0_JOURNAL_UNSUPP_SUPER, pctx))
		return EXT2_ET_CORRUPT_SUPERBLOCK;

	if (e2fsck_journal_fix_bad_inode(ctx, pctx))
		return EXT2_ET_UNSUPP_FEATURE;

	return 0;
}

static int e2fsck_journal_load(journal_t *journal)
{
	e2fsck_t ctx = journal->j_dev;
	journal_superblock_t *jsb;
	struct buffer_head *jbh = journal->j_sb_buffer;
	struct problem_context pctx;

	clear_problem_context(&pctx);

	ll_rw_block(READ, 1, &jbh);
	if (jbh->b_err) {
		com_err(ctx->device_name, jbh->b_err,
			_("reading journal superblock\n"));
		return jbh->b_err;
	}

	jsb = journal->j_superblock;
	/* If we don't even have JFS_MAGIC, we probably have a wrong inode */
	if (jsb->s_header.h_magic != htonl(JFS_MAGIC_NUMBER))
		return e2fsck_journal_fix_bad_inode(ctx, &pctx);

	switch (ntohl(jsb->s_header.h_blocktype)) {
	case JFS_SUPERBLOCK_V1:
		journal->j_format_version = 1;
		break;
		
	case JFS_SUPERBLOCK_V2:
		journal->j_format_version = 2;
		break;
		
	/* If we don't understand the superblock major type, but there
	 * is a magic number, then it is likely to be a new format we
	 * just don't understand, so leave it alone. */
	default:
		com_err(ctx->program_name, EXT2_ET_UNSUPP_FEATURE,
			_("%s: journal has unrecognised format\n"),
			ctx->device_name);
		return EXT2_ET_UNSUPP_FEATURE;
	}

	if (JFS_HAS_INCOMPAT_FEATURE(journal, ~JFS_KNOWN_INCOMPAT_FEATURES)) {
		com_err(ctx->program_name, EXT2_ET_UNSUPP_FEATURE,
			_("%s: journal has incompatible features\n"),
			ctx->device_name);
		return EXT2_ET_UNSUPP_FEATURE;
	}
		
	if (JFS_HAS_RO_COMPAT_FEATURE(journal, ~JFS_KNOWN_ROCOMPAT_FEATURES)) {
		com_err(ctx->program_name, EXT2_ET_UNSUPP_FEATURE,
			_("%s: journal has readonly-incompatible features\n"),
			ctx->device_name);
		return EXT2_ET_RO_UNSUPP_FEATURE;
	}

	/* We have now checked whether we know enough about the journal
	 * format to be able to proceed safely, so any other checks that
	 * fail we should attempt to recover from. */
	if (jsb->s_blocksize != htonl(journal->j_blocksize)) {
		com_err(ctx->program_name, EXT2_ET_CORRUPT_SUPERBLOCK,
			_("%s: no valid journal superblock found\n"),
			ctx->device_name);
		return EXT2_ET_CORRUPT_SUPERBLOCK;
	}

	if (ntohl(jsb->s_maxlen) < journal->j_maxlen)
		journal->j_maxlen = ntohl(jsb->s_maxlen);
	else if (ntohl(jsb->s_maxlen) > journal->j_maxlen) {
		com_err(ctx->program_name, EXT2_ET_CORRUPT_SUPERBLOCK,
			_("%s: journal too short\n"),
			ctx->device_name);
		return EXT2_ET_CORRUPT_SUPERBLOCK;
	}

	journal->j_tail_sequence = ntohl(jsb->s_sequence);
	journal->j_transaction_sequence = journal->j_tail_sequence;
	journal->j_tail = ntohl(jsb->s_start);
	journal->j_first = ntohl(jsb->s_first);
	journal->j_last = ntohl(jsb->s_maxlen);

	return 0;
}

static void e2fsck_journal_reset_super(e2fsck_t ctx, journal_superblock_t *jsb,
				journal_t *journal)
{
	char *p;
	
	/* Leave a valid existing V1 superblock signature alone.
	 * Anything unrecognisable we overwrite with a new V2
	 * signature. */
	
	if (jsb->s_header.h_magic != htonl(JFS_MAGIC_NUMBER) ||
	    jsb->s_header.h_blocktype != htonl(JFS_SUPERBLOCK_V1)) {
		jsb->s_header.h_magic = htonl(JFS_MAGIC_NUMBER);
		jsb->s_header.h_blocktype = htonl(JFS_SUPERBLOCK_V2);
	}

	/* Zero out everything else beyond the superblock header */
	
	p = ((char *) jsb) + sizeof(journal_header_t);
	memset (p, 0, ctx->fs->blocksize-sizeof(journal_header_t));

	jsb->s_blocksize = htonl(ctx->fs->blocksize);
	jsb->s_maxlen = htonl(journal->j_maxlen);
	jsb->s_first = htonl(1);
	jsb->s_sequence = htonl(1);

	/* In theory we should also re-zero the entire journal here.
	 * Initialising s_sequence to a random value would be a
	 * reasonable compromise. */

	ll_rw_block(WRITE, 1, &journal->j_sb_buffer);
}

static int e2fsck_journal_fix_corrupt_super(e2fsck_t ctx, journal_t *journal,
					    struct problem_context *pctx)
{
	struct ext2_super_block *sb = ctx->fs->super;
	int recover = ctx->fs->super->s_feature_incompat &
		EXT3_FEATURE_INCOMPAT_RECOVER;

	pctx->num = journal->j_inode->i_ino;

	if (sb->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) {
		if (fix_problem(ctx, PR_0_JOURNAL_BAD_SUPER, pctx)) {
			e2fsck_journal_reset_super(ctx, journal->j_superblock,
						   journal);
			journal->j_transaction_sequence = 1;
			e2fsck_clear_recover(ctx, recover);
			return 0;
		}
		return EXT2_ET_CORRUPT_SUPERBLOCK;
	} else if (e2fsck_journal_fix_bad_inode(ctx, pctx))
		return EXT2_ET_CORRUPT_SUPERBLOCK;

	return 0;
}

static void e2fsck_journal_release(e2fsck_t ctx, journal_t *journal, int reset)
{
	journal_superblock_t *jsb;

	if (!(ctx->options & E2F_OPT_READONLY)) {
		jsb = journal->j_superblock;
		jsb->s_sequence = htonl(journal->j_transaction_sequence);
		if (reset)
			jsb->s_start = 0; /* this marks the journal as empty */
		mark_buffer_dirty(journal->j_sb_buffer, 1);
	}
	brelse(journal->j_sb_buffer);

	if (journal->j_inode)
		free(journal->j_inode);
	ext2fs_free_mem((void **)&journal);
}

/*
 * This function makes sure that the superblock fields regarding the
 * journal are consistent.
 */
int e2fsck_check_ext3_journal(e2fsck_t ctx)
{
	struct ext2_super_block *sb = ctx->fs->super;
	journal_t *journal;
	int recover = ctx->fs->super->s_feature_incompat &
		EXT3_FEATURE_INCOMPAT_RECOVER;
	struct problem_context pctx;
	int reset = 0, force_fsck = 0;
	int retval;

	/* If we don't have any journal features, don't do anything more */
	if (!(sb->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) &&
	    !recover && sb->s_journal_inum == 0 && sb->s_journal_dev == 0 &&
	    uuid_is_null(sb->s_journal_uuid))
 		return 0;

#ifdef JFS_DEBUG		/* Enabled by configure --enable-jfs-debug */
	journal_enable_debug = 2;
#endif
	clear_problem_context(&pctx);
	pctx.num = sb->s_journal_inum;

	retval = e2fsck_get_journal(ctx, &journal);
	if (retval) {
		if (retval == EXT2_ET_BAD_INODE_NUM)
			return e2fsck_journal_fix_bad_inode(ctx, &pctx);
		return retval;
	}

	retval = e2fsck_journal_load(journal);
	if (retval) {
		if (retval == EXT2_ET_CORRUPT_SUPERBLOCK)
			return e2fsck_journal_fix_corrupt_super(ctx, journal,
								&pctx);
		return retval;
	}

	/*
	 * We want to make the flags consistent here.  We will not leave with
	 * needs_recovery set but has_journal clear.  We can't get in a loop
	 * with -y, -n, or -p, only if a user isn't making up their mind.
	 */
no_has_journal:
	if (!(sb->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)) {
		recover = sb->s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER;
		pctx.str = "inode";
		if (fix_problem(ctx, PR_0_JOURNAL_HAS_JOURNAL, &pctx)) {
			if (recover &&
			    !fix_problem(ctx, PR_0_JOURNAL_RECOVER_SET, &pctx))
				goto no_has_journal;
			/*
			 * Need a full fsck if we are releasing a
			 * journal stored on a reserved inode.
			 */
			force_fsck = recover ||
				(sb->s_journal_inum < EXT2_FIRST_INODE(sb));
			/* Clear all of the journal fields */
			sb->s_journal_inum = 0;
			sb->s_journal_dev = 0;
			memset(sb->s_journal_uuid, 0,
			       sizeof(sb->s_journal_uuid));
			e2fsck_clear_recover(ctx, force_fsck);
		} else if (!(ctx->options & E2F_OPT_READONLY)) {
			sb->s_feature_compat |= EXT3_FEATURE_COMPAT_HAS_JOURNAL;
			ext2fs_mark_super_dirty(ctx->fs);
		}
	}

	if (sb->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL &&
	    !(sb->s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) &&
	    journal->j_superblock->s_start != 0) {
		if (fix_problem(ctx, PR_0_JOURNAL_RESET_JOURNAL, &pctx)) {
			reset = 1;
			sb->s_state &= ~EXT2_VALID_FS;
			ext2fs_mark_super_dirty(ctx->fs);
		}
		/*
		 * If the user answers no to the above question, we
		 * ignore the fact that journal apparently has data;
		 * accidentally replaying over valid data would be far
		 * worse than skipping a questionable recovery.
		 * 
		 * XXX should we abort with a fatal error here?  What
		 * will the ext3 kernel code do if a filesystem with
		 * !NEEDS_RECOVERY but with a non-zero
		 * journal->j_superblock->s_start is mounted?
		 */
	}

	e2fsck_journal_release(ctx, journal, reset);
	return retval;
}

static int recover_ext3_journal(e2fsck_t ctx)
{
	journal_t *journal;
	int retval;

	retval = e2fsck_get_journal(ctx, &journal);
	if (retval)
		return retval;

	retval = e2fsck_journal_load(journal);
	if (retval)
		return retval;

	retval = journal_init_revoke(journal, 1024);
	if (retval)
		return retval;
	
	retval = -journal_recover(journal);
	e2fsck_journal_release(ctx, journal, 1);
	return retval;
}


#if 0
#define TEMPLATE "/tmp/ext3.XXXXXX"

/*
 * This function attempts to mount and unmount an ext3 filesystem,
 * which is a cheap way to force the kernel to run the journal and
 * handle the recovery for us.
 */
static int recover_ext3_journal_via_mount(e2fsck_t ctx)
{
	ext2_filsys fs = ctx->fs;
	char	*dirlist[] = {"/mnt","/lost+found","/tmp","/root","/boot",0};
	errcode_t	 retval, retval2;
	int	 count = 0;
	char	 template[] = TEMPLATE;
	struct stat buf;
	char	*tmpdir;

	if (ctx->options & E2F_OPT_READONLY) {
		printf("%s: won't do journal recovery while read-only\n",
		       ctx->device_name);
		return EXT2_ET_FILE_RO;
	}

	printf(_("%s: trying for ext3 kernel journal recovery\n"),
	       ctx->device_name);
	/*
	 * First try to make a temporary directory.  This may fail if
	 * the root partition is still mounted read-only.
	 */
newtemp:
	tmpdir = mktemp(template);
	if (tmpdir) {
		jfs_debug(2, "trying %s as ext3 temp mount point\n", tmpdir);
		if (mkdir(template, 0700)) {
			if (errno == EROFS) {
				tmpdir = NULL;
				template[0] = '\0';
			} else if (errno == EEXIST && count++ < 10) {
				strcpy(template, TEMPLATE);
				goto newtemp;
			}
			return errno;
		}
	}

	/*
	 * OK, creating a temporary directory didn't work.
	 * Let's try a list of possible temporary mountpoints.
	 */
	if (!tmpdir) {
		dev_t	rootdev;
		char	**cpp, *dir;

		if (stat("/", &buf))
			return errno;

		rootdev = buf.st_dev;

		/*
		 * Check that dir is on the same device as root (no other
		 * filesystem is mounted there), and it's a directory.
		 */
		for (cpp = dirlist; (dir = *cpp); cpp++)
			if (stat(dir, &buf) == 0 && buf.st_dev == rootdev &&
			    S_ISDIR(buf.st_mode)) {
				tmpdir = dir;
				break;
			}
	}

	if (tmpdir) {
		io_manager	io_ptr = fs->io->manager;
		int		blocksize = fs->blocksize;

		jfs_debug(2, "using %s for ext3 mount\n", tmpdir);
		/* FIXME - need to handle loop devices here */
		if (mount(ctx->device_name, tmpdir, "ext3", MNT_FL, NULL)) {
			retval = errno;
			com_err(ctx->program_name, errno,
				"when mounting %s", ctx->device_name);
			if (template[0])
				rmdir(tmpdir);
			return retval;
		}
		/*
		 * Now that it mounted cleanly, the filesystem will have been
		 * recovered, so we can now unmount it.
		 */
		if (umount(tmpdir))
			return errno;

		/*
		 * Remove the temporary directory, if it was created.
		 */
		if (template[0])
			rmdir(tmpdir);
		return 0;
	}
}
#endif

int e2fsck_run_ext3_journal(e2fsck_t ctx)
{
	io_manager io_ptr = ctx->fs->io->manager;
	int blocksize = ctx->fs->blocksize;
	errcode_t	retval, recover_retval;

	printf(_("%s: recovering journal\n"), ctx->device_name);
	if (ctx->options & E2F_OPT_READONLY) {
		printf(_("%s: won't do journal recovery while read-only\n"),
		       ctx->device_name);
		return EXT2_ET_FILE_RO;
	}

	recover_retval = recover_ext3_journal(ctx);
	
	/*
	 * Reload the filesystem context to get up-to-date data from disk
	 * because journal recovery will change the filesystem under us.
	 */
	ext2fs_close(ctx->fs);
	retval = ext2fs_open(ctx->filesystem_name, EXT2_FLAG_RW,
			     ctx->superblock, blocksize, io_ptr,
			     &ctx->fs);

	if (retval) {
		com_err(ctx->program_name, retval,
			_("while trying to re-open %s"),
			ctx->device_name);
		fatal_error(ctx, 0);
	}
	ctx->fs->priv_data = ctx;

	/* Set the superblock flags */
	e2fsck_clear_recover(ctx, recover_retval);
	return recover_retval;
}
