//By Monica Moniot
#ifdef __cplusplus
extern "C" {
#endif

#ifndef INODE__H_INCLUDE
#define INODE__H_INCLUDE

#include "basic.h"
#include "block_device.h"
#include <alloca.h>
// #define alloca _alloca


typedef int64 INodePid;
#define INODE_MASK 0xFF
#define INODE_SHIFT 8

enum INodeStatus {
	INODE_INVALID,
	INODE_BUFFER,
	INODE_DIR,
	INODE_FILE,
};
#define BLOCKS_PER_INODE 13
typedef struct INode {
	INodePid pid;
	uint16 level;
	uint16 status;
	uint32 _padding;
	uint64 mem_size;
	BlockPid blocks[BLOCKS_PER_INODE];
} INode;

typedef struct INodeAllocator {
	INodePid next_inode;
	int inodes_per_block;
} INodeAllocator;

//API subject to change, see shell.c for an example of it's usage

#define INODE_BLOCK_SIZE_MAX ((INODE_MASK + 1)*sizeof(INode))
#define INODE_BLOCK_SIZE_MIN sizeof(INode)
void inode_initfs   (INodeAllocator* allocator, int block_size);
bool inode_mountfs  (BlockDevice* device, INodeAllocator* allocator);
bool inode_unmountfs(BlockDevice* device, INodeAllocator* allocator);

bool inode_create (BlockDevice* device, INodeAllocator* allocator, uint64 mem_size, INode* inode);//calls save
bool inode_destroy(BlockDevice* device, INodeAllocator* allocator, INode* inode);//calls save
bool inode_save   (BlockDevice* device,         const INode* inode);
bool inode_restore(BlockDevice* device, INodePid pid, INode* inode);

bool inode_set_size(BlockDevice* device, INode* inode, uint64 mem_size);

bool inode_write   (BlockDevice* device, INode* inode, uint64 mem_offset, const void* mem, uint64 mem_size);
bool inode_read    (BlockDevice* device, INode* inode, uint64 mem_offset,       void* mem, uint64 mem_size);

#endif

#ifdef INODE_IMPLEMENTATION
#undef INODE_IMPLEMENTATION

static INodePid inode_alloc(BlockDevice* device, INodeAllocator* allocator) {
	INodePid ret_inode = allocator->next_inode;
	if(ret_inode) {
		uint inode_i = ret_inode&INODE_MASK;
		BlockPid block = ret_inode>>INODE_SHIFT;
		INodePid next;
		if(block_reads(device, block, inode_i*sizeof(INode), &next, sizeof(INodePid))) {
			allocator->next_inode = next;
		} else return 0;
	} else {
		BlockPid block = block_alloc(device);
		if(!block) return 0;
		INodePid base_pid = block<<INODE_SHIFT;
		int i = 1;
		while(i < allocator->inodes_per_block - 1) {
			INodePid next = base_pid|(i + 1);
			if(!block_writes(device, block, i*sizeof(INode), &next, sizeof(INodePid))) {
				block_free(device, block);
				return 0;
			}
			i += 1;
		}
		INodePid next = 0;
		if(!block_writes(device, block, i*sizeof(INode), &next, sizeof(INodePid))) {
			block_free(device, block);
			return 0;
		}
		ret_inode = base_pid;
		allocator->next_inode = base_pid|1;
	}
	return ret_inode;
}
static bool     inode_free (BlockDevice* device, INodeAllocator* allocator, INodePid pid) {
	uint inode_i = pid&INODE_MASK;
	BlockPid block = pid>>INODE_SHIFT;
	INodePid next = allocator->next_inode;
	if(block_writes(device, block, inode_i*sizeof(INode), &next, sizeof(INodePid))) {
		allocator->next_inode = pid;
		return 1;
	} else {
		return 0;
	}
}

void inode_initfs(INodeAllocator* allocator, int block_size) {
	ASSERT(block_size >= INODE_BLOCK_SIZE_MIN);
	ASSERT(block_size <= INODE_BLOCK_SIZE_MAX);
	allocator->next_inode = 0;
	allocator->inodes_per_block = block_size/sizeof(INode);
}
bool inode_mountfs(BlockDevice* device, INodeAllocator* allocator) {
	return block_reads_m(device, 0, sizeof(device->persistent_data), allocator, sizeof(INodeAllocator));
}
bool inode_unmountfs(BlockDevice* device, INodeAllocator* allocator) {
	return block_writes_m(device, 0, sizeof(device->persistent_data), allocator, sizeof(INodeAllocator));
}


static int inode_get_required_level(int64 mem_size, int block_size) {
	uint64 capacity = BLOCKS_PER_INODE*block_size;
	int level = 0;
	int block_base = block_size/sizeof(BlockPid);
	while(capacity < mem_size) {
		capacity *= block_base;
		level += 1;
	}
	return level;
}

bool inode_save(BlockDevice* device, const INode* inode) {
	return block_writes(device, inode->pid>>INODE_SHIFT, (inode->pid&INODE_MASK)*sizeof(INode), inode, sizeof(INode));
}
bool inode_restore(BlockDevice* device, INodePid pid, INode* inode) {
	return block_reads(device, pid>>INODE_SHIFT, (pid&INODE_MASK)*sizeof(INode), inode, sizeof(INode));
}

bool inode_create(BlockDevice* device, INodeAllocator* allocator, uint64 mem_size, INode* inode) {
	INodePid pid = inode_alloc(device, allocator);
	if(!pid) return 0;
	inode->pid = pid;
	inode->level = inode_get_required_level(mem_size, device->block_size);
	inode->status = INODE_BUFFER;
	inode->mem_size = mem_size;
	for_each_lt(i, BLOCKS_PER_INODE) {
		inode->blocks[i] = 0;
	}
	inode_save(device, inode);
	return 1;
}

static bool free_all(BlockDevice* device, BlockPid pid, int level) {
	if(pid) {
		if(level > 0) {
			for_each_lt(i, device->block_size/sizeof(BlockPid)) {
				BlockPid next_pid;
				if(!block_reads(device, pid, i*sizeof(BlockPid), &next_pid, sizeof(BlockPid))) return 0;
				if(!free_all(device, next_pid, level - 1)) return 0;
			}
		}
		if(!block_free(device, pid)) return 0;
	}
	return 1;
}
bool inode_destroy(BlockDevice* device, INodeAllocator* allocator, INode* inode) {
	for_each_lt(i, BLOCKS_PER_INODE) {
		if(!free_all(device, inode->blocks[i], inode->level)) return 0;
		inode->blocks[i] = 0;
	}
	inode->level = 0;
	inode->status = INODE_INVALID;
	inode->mem_size = 0;
	inode_save(device, inode);
	if(!inode_free(device, allocator, inode->pid)) return 0;
	return 1;
}


static int64 powi(int64 base, int p) {
	int64 ret = 1;
	for_each_lt(_, p) {
		ret *= base;
	}
	return ret;
}

static BlockPid alloc_block_pids(BlockDevice* device, BlockPid above_pid, int pid_index) {
	BlockPid ret = block_alloc(device);
	if(!ret) return 0;
	void* empty_mem = alloca(device->block_size);
	memzero(empty_mem, device->block_size);
	if(!block_write(device, ret, empty_mem)) return 0;
	if(above_pid) {
		if(!block_writes(device, above_pid, pid_index*sizeof(BlockPid), &ret, sizeof(BlockPid))) return 0;
	}
	return ret;
}
static bool get_block_pid(BlockDevice* device, BlockPid above_pid, int pid_index, bool alloc_if_missing, BlockPid* ret) {
	BlockPid ret0 = 0;
	if(above_pid) {
		if(!block_reads(device, above_pid, pid_index*sizeof(BlockPid), &ret0, sizeof(BlockPid))) return 0;
		if(alloc_if_missing && !ret0) {
			ret0 = alloc_block_pids(device, above_pid, pid_index);
			if(!ret0) return 0;
		}
	}
	*ret = ret0;
	return 1;
}

enum IncrementPathMode {
	INC_PATH_WRITE,
	INC_PATH_READ,
	INC_PATH_FREE,
};
static int64 increment_block_path(BlockDevice* device, INode* inode, int* block_path, BlockPid* block_path_pids, enum IncrementPathMode mode, BlockPid* ret, int64 blocks_stepped_max) {
	//NOTE: DO NOT CALL OUTSIDE OF THE READ, WRITE AND FREE FUNCTIONS
	//this function is basically a fancy macro for the path increment algorithm
	int level = inode->level;
	int block_size = device->block_size;
	int block_base = block_size/sizeof(BlockPid);
	//get next block's pid from the path, we do this by performing an increment on the path's numeric value
	int64 blocks_stepped = 1;
	if(blocks_stepped > blocks_stepped_max) {
		*ret = 0;
		return blocks_stepped_max;
	}
	while(1) {
		BlockPid bottom_most_unchanged_pid;
		int i = 0;
		while(1) {
			//perform addition with overflow on the block path; this decreases the number of reads from disk we have to do from linear to logarithmic in the inode level per block
			if(i >= level) {
				if(block_path[i] + 1 >= BLOCKS_PER_INODE) {
					//we've reached the absolute largest path possible
					for_each_in_range_bw(j, 0, i) {
						if(mode == INC_PATH_FREE) {
							if(!block_free(device, block_path_pids[j])) return 0;
						}
						block_path_pids[j] = 0;
					}
					*ret = 0;
					return blocks_stepped_max;
				} else {
					block_path[level] += 1;
					bottom_most_unchanged_pid = inode->blocks[block_path[level]];
					if(mode == INC_PATH_WRITE && !bottom_most_unchanged_pid) {
						bottom_most_unchanged_pid = alloc_block_pids(device, 0, 0);
						inode->blocks[block_path[level]] = bottom_most_unchanged_pid;
					}
					if(mode == INC_PATH_FREE && block_path_pids[level]) {
						if(!block_free(device, block_path_pids[level])) return 0;
					}
					block_path_pids[level] = bottom_most_unchanged_pid;
					i = level - 1;
					//increment can't go further
					break;
				}
			} else if(block_path[i] + 1 >= block_base) {
				//path overflow
				block_path[i] = 0;
				i += 1;
			} else {
				bottom_most_unchanged_pid = block_path_pids[i + 1];
				block_path[i] += 1;
				if(mode == INC_PATH_WRITE || bottom_most_unchanged_pid) {
					//increment is done
					break;
				} else {
					//skip the empty blocks by forcing an overflow and continuing increment
					blocks_stepped += (block_base - block_path[i])*powi(block_base, i);
					if(blocks_stepped > blocks_stepped_max) {
						*ret = 0;
						return blocks_stepped_max;
					}
					block_path[i] = 0;
					i += 1;
				}
			}
		}
		//walk down the path again to the next block pid
		for_each_in_range_bw(j, 0, i) {
			if(!get_block_pid(device, bottom_most_unchanged_pid, block_path[j], mode == INC_PATH_WRITE, &bottom_most_unchanged_pid)) return 0;
			if(mode == INC_PATH_FREE && block_path_pids[j]) {
				//Kind of hacky, but this was the easiest way to implement freeing, and this is a private helper function anyways
				if(!block_free(device, block_path_pids[j])) return 0;
			}
			block_path_pids[j] = bottom_most_unchanged_pid;
		}
		if(mode == INC_PATH_WRITE || block_path_pids[0]) {
			*ret = block_path_pids[0];
			return blocks_stepped;
		} else {
			blocks_stepped += 1;
			if(blocks_stepped > blocks_stepped_max) {
				*ret = 0;
				return blocks_stepped_max;
			}
		}
	}
}
static bool get_block_path(BlockDevice* device, INode* inode, int* block_path, BlockPid* block_path_pids, int64 block_offset, enum IncrementPathMode mode) {
	//helper function to go along with increment_block_path
	//initializes the block_path and block_path_pids arrays
	int level = inode->level;
	int block_size = device->block_size;
	int block_base = block_size/sizeof(BlockPid);
	//find the initial block, and it's path in the inode
	for_each_lt(i, level) {
		block_path[i] = block_offset%block_base;
		block_offset /= block_base;
	}
	ASSERT(block_offset < BLOCKS_PER_INODE);
	BlockPid top_pid = inode->blocks[block_offset];
	if(mode == INC_PATH_WRITE && !top_pid) {
		top_pid = alloc_block_pids(device, 0, 0);
		inode->blocks[block_offset] = top_pid;
	}
	block_path[level] = block_offset;//index in block struct array
	block_path_pids[level] = top_pid;//value at that index
	for_each_lt_bw(i, level) {
		if(!get_block_pid(device, top_pid, block_path[i], mode == INC_PATH_WRITE, &top_pid)) return 0;
		block_path_pids[i] = top_pid;
	}
	return 1;
}

bool inode_set_size(BlockDevice* device, INode* inode, uint64 mem_size) {
	int block_size = device->block_size;
	int block_base = block_size/sizeof(BlockPid);
	if(mem_size > inode->mem_size) {
		//find the new level
		int new_level = inode_get_required_level(mem_size, block_size);
		if(new_level > inode->level) {
			//push the inode struct array down to a block array
			BlockPid bottom_pid = alloc_block_pids(device, 0, 0);
			if(!bottom_pid) return 0;
			//the block will always have enough space bc of INODE_BLOCK_SIZE_MIN
			if(!block_writes(device, bottom_pid, 0, &inode->blocks[0], BLOCKS_PER_INODE*sizeof(BlockPid))) return 0;
			for_each_lt(_, new_level - inode->level - 1) {
				BlockPid cur_pid = alloc_block_pids(device, 0, 0);
				if(!cur_pid) return 0;
				if(!block_writes(device, cur_pid, 0, &bottom_pid, sizeof(BlockPid))) return 0;
				bottom_pid = cur_pid;
			}
			//finish filling out the enlargened tree
			inode->blocks[0] = bottom_pid;
			for_each_in_range(i, 1, BLOCKS_PER_INODE - 1) {
				inode->blocks[i] = 0;
			}
			inode->level = new_level;
		}
		//we need to do nothing extra! technically the tree is already big enough for the size
		inode->mem_size = mem_size;
	} else if(mem_size < inode->mem_size) {
		/*Truncate is unimplemented
		//the file is getting smaller, we need to free everthing no longer in use, but keep the rest
		int64 data_blocks_to_preserve = (mem_size + block_size - 1)/block_size;
		int64 data_blocks_to_free = (inode->mem_size + block_size - 1)/block_size - data_blocks_to_preserve;
		//get the path to the end of the preserved section
		int* block_path = cast(int*, alloca((inode->level + 1)*sizeof(int)));//big endian
		BlockPid* block_path_pids = cast(BlockPid*, alloca((inode->level + 1)*sizeof(BlockPid)));
		if(!get_block_path(device, inode, block_path, block_path_pids, data_blocks_to_preserve, INC_PATH_FREE)) return 0;
		//find the new root of the shortened tree, and it's new level
		//we assume that it is 0 until proven otherwise
		int new_last_root_index = block_path[0];
		int new_root_block = (inode->level > 0) ? block_path_pids[1] : 0;
		int new_level = 0;
		for_each_in_range_bw(i, 0, inode->level) {
			//find the first non-zero entry in the path, this is the new root
			if(block_path[i] > 0) {
				if(block_path[i] >= BLOCKS_PER_INODE) {
					ASSERT(i < inode->level);
					i += 1;
				}
				new_level = i;
				new_last_root_index = block_path[i];
				if(new_level != inode->level) {
					new_root_block = block_path_pids[i + 1];
				} else {
					//there is no new root block, we don't need to overwrite the struct array
					new_root_block = 0;
				}
				break;
			}
		}
		ASSERT(new_last_root_index < BLOCKS_PER_INODE);
		ASSERT(new_level <= inode->level);

		//free everything after the preserved section
		BlockPid cur_pid;
		if(!increment_block_path(device, inode, block_path, block_path_pids, INC_PATH_READ, &cur_pid, 1)) return 0;
		while(data_blocks_to_free > 0) {
			//increment_block_path has a mode that frees blocks it increments over
			//so we increment over all remaining blocks
			int64 steps = increment_block_path(device, inode, block_path, block_path_pids, INC_PATH_FREE, &cur_pid, data_blocks_to_free);
			ASSERT(cur_pid < 100000);

			if(!steps) return 0;
			data_blocks_to_free -= steps;
		}
		//free the last set of blocks increment leaves behind
		for_each_lt(i, inode->level + 1) {
			if(block_path_pids[i]) {
				if(!block_free(device, block_path_pids[i])) return 0;
			}
		}
		//truncate tree
		if(inode->level != new_level) {
			if(!block_reads(device, new_root_block, 0, &inode->blocks[0], (new_last_root_index + 1)*sizeof(BlockPid))) return 0;
			for_each_in_range(i, new_last_root_index + 1, BLOCKS_PER_INODE - 1) {
				inode->blocks[i] = 0;
			}
			inode->level = new_level;
		}
		//bookkeep
		*/
		inode->mem_size = mem_size;
	}
	return 1;
}

bool inode_write(BlockDevice* device, INode* inode, uint64 mem_offset, const void* mem, uint64 mem_size) {
	if(mem_offset + mem_size < mem_size) {ASSERT(0); return 0;}//catch overflows
	else if(mem_offset + mem_size > inode->mem_size) {
		//catch mem too large
		inode_set_size(device, inode, mem_offset + mem_size);
	} else if(mem_size == 0) {return 0;}
	if(!mem_size) return 1;

	int block_size = device->block_size;
	int level = inode->level;
	int block_base = block_size/sizeof(BlockPid);

	//find the initial block for writing, and it's path in the inode
	int internal_offset = mem_offset%block_size;
	int64 block_offset = mem_offset/block_size;
	int* block_path = cast(int*, alloca((level + 1)*sizeof(int)));
	BlockPid* block_path_pids = cast(BlockPid*, alloca((level + 1)*sizeof(BlockPid)));
	if(!get_block_path(device, inode, block_path, block_path_pids, block_offset, INC_PATH_WRITE)) return 0;
	//write data to disk
	BlockPid data_pid0 = block_path_pids[0];
	int write_size0 = block_size - internal_offset;
	if(mem_size <= write_size0) {
		if(!block_writes(device, data_pid0, internal_offset, mem, mem_size)) return 0;
	} else {
		if(!block_writes(device, data_pid0, internal_offset, mem, write_size0)) return 0;
		//data is too big for one block
		const byte* mem_left = ptr_add(const byte, mem, write_size0);
		int64 mem_left_size = mem_size - write_size0;
		while(mem_left_size > 0) {
			BlockPid cur_data_pid;
			if(!increment_block_path(device, inode, block_path, block_path_pids, INC_PATH_WRITE, &cur_data_pid, (mem_left_size + block_size - 1)/block_size)) return 0;
			ASSERT(cur_data_pid);
			if(mem_left_size >= block_size) {
				if(!block_write(device, cur_data_pid, mem_left)) return 0;
				mem_left += block_size;
				mem_left_size -= block_size;
			} else {
				if(!block_writes(device, cur_data_pid, 0, mem_left, mem_left_size)) return 0;
				break;
			}
		}
	}
	return 1;
}

bool inode_read(BlockDevice* device, INode* inode, uint64 mem_offset, void* mem, uint64 mem_size) {
	if(mem_offset + mem_size < mem_size) {ASSERT(0); return 0;}//catch overflows
	else if(mem_offset + mem_size > inode->mem_size) {ASSERT(0); return 0;}//catch mem too large
	if(!mem_size) return 1;

	int block_size = device->block_size;
	int level = inode->level;
	int block_base = block_size/sizeof(BlockPid);

	//find the initial block for reading, and it's path in the inode
	int internal_offset = mem_offset%block_size;
	int64 block_offset = mem_offset/block_size;
	int* block_path = cast(int*, alloca((level + 1)*sizeof(int)));
	BlockPid* block_path_pids = cast(BlockPid*, alloca((level + 1)*sizeof(BlockPid)));
	if(!get_block_path(device, inode, block_path, block_path_pids, block_offset, INC_PATH_READ)) return 0;
	//write data to disk
	BlockPid data_pid0 = block_path_pids[0];
	int read_size0 = block_size - internal_offset;
	if(mem_size <= read_size0) {
		if(!block_reads(device, data_pid0, internal_offset, mem, mem_size)) return 0;
	} else {
		if(!block_reads(device, data_pid0, internal_offset, mem, read_size0)) return 0;
		//data is too big for one block
		byte* mem_left = ptr_add(byte, mem, read_size0);
		int64 mem_left_size = mem_size - read_size0;
		while(mem_left_size > 0) {
			BlockPid cur_data_pid;
			int64 steps = increment_block_path(device, inode, block_path, block_path_pids, INC_PATH_READ, &cur_data_pid, (mem_left_size + block_size - 1)/block_size);
			if(!steps) return 0;
			//handle if increment skipped blocks, or is returning 0 as the final block
			if(!cur_data_pid) {
				ASSERT(mem_left_size <= steps*block_size);
				memzero(mem, mem_left_size);
				break;
			} else if(steps > 1) {
				int64 skipped_mem = (steps - 1)*block_size;
				memzero(mem, skipped_mem);
				mem_left += skipped_mem;
				mem_left_size -= skipped_mem;
			}
			if(mem_left_size >= block_size) {
				if(!block_read(device, cur_data_pid, mem_left)) return 0;
				mem_left += block_size;
				mem_left_size -= block_size;
			} else {
				if(mem_left_size > 0) {
					if(!block_reads(device, cur_data_pid, 0, mem_left, mem_left_size)) return 0;
				}
				break;
			}
		}
	}
	return 1;
}

#endif

#ifdef __cplusplus
}
#endif
