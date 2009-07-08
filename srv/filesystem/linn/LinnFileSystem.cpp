/*
 * Copyright (C) 2009 Niek Linnenbank
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <Types.h>
#include <BootModule.h>
#include <Storage.h>
#include <LogMessage.h>
#include "LinnFileSystem.h"
#include "LinnInode.h"
#include "LinnFile.h"
#include "LinnDirectory.h"
#include <stdlib.h>

int main(int argc, char **argv)
{
    BootModule module("/boot/boot.linn");
    
    if (module.load())
    {
        LinnFileSystem server("/mnt", &module);
        return server.run();
    }
    return EXIT_FAILURE;
}

LinnFileSystem::LinnFileSystem(const char *p, Storage *s)
    : FileSystem(p), storage(s), groups(ZERO)
{
    FileSystemPath slash("/");
    LinnInode *rootInode;
    LinnGroup *group;
    Size offset;
    Error e;

    /* Read out the superblock. */
    if ((e = s->read(LINN_SUPER_OFFSET, (u8 *) &super,
		     sizeof(super))) <= 0)
    {
	log("LinnFS: reading superblock failed: %s",
	     strerror(e));
	exit(EXIT_FAILURE);
    }
    /* Verify magic. */
    if (super.magic0 != LINN_SUPER_MAGIC0 ||
        super.magic1 != LINN_SUPER_MAGIC1)
    {
	log("LinnFS: magic mismatch");
	exit(EXIT_FAILURE);
    }
    /* Create groups vector. */
    Size cnt = LINN_GROUP_COUNT(&super);
    groups   = new Array<LinnGroup>(cnt);

    /* Read out group descriptors. */
    for (le64 i = 0; i < LINN_GROUP_COUNT(&super); i++)
    {
	/* Allocate buffer. */
	group  = new LinnGroup;
	offset = (super.groupsTable * super.blockSize) +
		 (sizeof(LinnGroup)  * i);

	/* Read from storage. */
	if ((e = s->read(offset, (u8 *) group, sizeof(LinnGroup))) <= 0)
	{
	    log("LinnFS: reading group descriptor failed: %s",
		 strerror(e));
	    exit(EXIT_FAILURE);
	}
	/* Insert in the groups vector. */
	groups->insert(i, group);
    }
    log("LinnFS: %d group descriptors",
	 LINN_GROUP_COUNT(&super));
    
    /* Debug out superblock information. */
    log("LinnFS: %d inodes, %d blocks",
	 super.inodesCount - super.freeInodesCount,
	 super.blocksCount - super.freeBlocksCount);

    /* Read out the root directory. */
    rootInode = getInode(LINN_INODE_ROOT);
    root = new FileCache(&slash, new LinnDirectory(this, rootInode), ZERO);
    
    /* Done. */
    log("LinnFS: mounted '%s'", p);
}

Error LinnFileSystem::createFile(FileSystemMessage *msg,
				 FileSystemPath *path)
{
    return ENOTSUP;
}

LinnInode * LinnFileSystem::getInode(u64 inodeNum)
{
    LinnGroup *group;
    LinnInode *inode;
    Size offset;
    Error e;
    Integer<u64> inodeInt = inodeNum;
    
    /* Validate the inode number. */
    if (inodeNum >= super.inodesCount)
    {
	return ZERO;
    }
    /* Do we have this Inode cached already? */
    if ((inode = inodes[&inodeInt]))
    {
	return inode;
    }
    /* Get the group descriptor. */
    if (!(group = getGroupByInode(inodeNum)))
    {
	return ZERO;
    }
    /* Allocate inode buffer. */
    inode  = new LinnInode;
    offset = (group->inodeTable * super.blockSize) +
    	     (inodeNum % super.inodesPerGroup);
	     
    /* Read inode from storage. */
    if ((e = storage->read(offset, (u8 *) inode, sizeof(LinnInode))) <= 0)
    {
        log("LinnFS: reading inode failed: %s",
	     strerror(e));
	return ZERO;
    }
    /* Insert into the cache. */
    inodes.insert(new Integer<u64>(inodeNum), inode);
    return inode;
}

LinnGroup * LinnFileSystem::getGroup(u64 groupNum)
{
    return (*groups)[groupNum];
}

LinnGroup * LinnFileSystem::getGroupByInode(u64 inodeNum)
{
    return getGroup((inodeNum - 1) / super.inodesPerGroup);
}

u64 LinnFileSystem::getOffset(LinnInode *inode, u64 blk)
{
    u64 numPerBlock = LINN_SUPER_NUM_PTRS(&super), offset;
    u64 *block = ZERO;
    Size depth = ZERO;

    /* Direct blocks. */
    if (blk < LINN_INODE_DIR_BLOCKS)
    {
	return inode->block[blk] * super.blockSize;
    }
    /* Indirect blocks. */
    if (blk - LINN_INODE_DIR_BLOCKS < numPerBlock)
    {
	depth = 1;
    }
    /* Double indirect blocks. */
    else if (blk - LINN_INODE_DIR_BLOCKS < numPerBlock * numPerBlock)
    {
	depth = 2;
    }
    /* Tripple indirect blocks. */
    else
	depth = 3;
    
    /* Allocate temporary block. */
    block   = new u64[LINN_SUPER_NUM_PTRS(&super)];
    offset  = inode->block[(LINN_INODE_DIR_BLOCKS + depth - 1)];
    offset *= super.blockSize;
    
    /* Lookup the block number. */
    while (depth > 0)
    {
	/* Fetch block. */
	if (storage->read(offset, (u8 *) block, super.blockSize) < 0)
	{
	    delete block;
	    return 0;
	}
	/* Calculate the number of blocks remaining per entry. */
	Size remain = LINN_SUPER_NUM_PTRS(&super);
	
	/* Effectively the pow() function. */
	for (Size i = 0; i < depth - 1; i++)
	{
	    remain *= remain;
	}
	/* Calculate the next offset. */
	offset  = block[ remain / (blk - LINN_INODE_DIR_BLOCKS + 1) ];
	offset *= super.blockSize;
	depth--;
    }
    /* Calculate the final offset. */
    offset  = block[ (blk - LINN_INODE_DIR_BLOCKS) %
		      LINN_SUPER_NUM_PTRS(&super) ];
    offset *= super.blockSize;
    
    /* All done. */
    delete block;
    return offset;	
}

FileCache * LinnFileSystem::lookupFile(FileSystemPath *path)
{
    List<String> *entries = path->split();
    FileCache *c = root;
    LinnInode *inode;
    LinnDirectoryEntry entry;
    LinnDirectory *dir;

    /* Loop the entire path. */
    for (ListIterator<String> i(entries); i.hasNext(); i++)
    {
	/* Do we have this entry? */
        if (!c->entries[i.current()])
	{
	    /* If this isn't a directory, we cannot perform a lookup. */
	    if (c->file->getType() != DirectoryFile)
	    {
		return ZERO;
	    }
	    dir = (LinnDirectory *) c->file;
	    
	    /* Then retrieve it, if possible. */	
	    if (dir->getEntry(&entry, **i.current()) != ESUCCESS)
	    {
		return ZERO;
	    }
	    /* Lookup corresponding inode. */
	    if (!(inode = getInode(entry.inode)))
	    {
		return ZERO;
	    }
	    /* Create the appropriate in-memory file. */
	    switch ((FileType)inode->type)
	    {
	        case DirectoryFile:
		    c = insertFileCache(new LinnDirectory(this, inode),
		    			**i.current());
		    break;

		case RegularFile:
		    c = insertFileCache(new LinnFile(this, inode),
					**i.current());
		    break;

		default:
		    return ZERO;
	    }
	}
	/* Move to the next entry. */
	else
	    c = c->entries[i.current()];
    }
    return c;
}
