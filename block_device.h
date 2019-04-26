//By Monica Moniot
#ifdef __cplusplus
extern "C" {
#endif

#ifndef BLOCK_DEVICE__H_INCLUDE
#define BLOCK_DEVICE__H_INCLUDE

#include "basic.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>


/*
    The BlockDevice is the API that a file system is built on.
    Any block device supports block-level reads and writes.
	Support for block allocation is also integrated here.

    In this system, we simulate a block device using an OS file.
    In theory (an exercise for the motivated?) we could use almost
    this exact code to access an actual raw device (e.g.,
    /dev/rdiskx), but then the program would need to run as
    super-user, and if the wrong device were specified, you could
    overwrite your OS or user data. Beware.
*/

typedef int64 BlockPid;

typedef struct MasterBlock {
	uint64 cookie;
	BlockPid first_unused_block;
	BlockPid last_block;
} MasterBlock;

typedef union BlockDevice {
	struct {
		int device_id;
		MasterBlock master;
		int block_size;
		uint64 blocks_total;
	};
	struct {
		int _;
		struct {
			MasterBlock master;
			int block_size;
			uint64 blocks_total;
		} persistent_data;
	};
} BlockDevice;


#define BLOCK_SIZE_MIN sizeof(BlockDevice)

bool device_create(BlockDevice* device, const char* device_name, int block_size, uint64 blocks_total);
//device is the BlockDevice struct to be intialized and associated with the storage device
//device_name is the name that will be assigned to the storage device, see device_open
//block_size is the size in bytes that all blocks allocated by this device will have, see block_write and block_read,
//this value must be at least BLOCK_SIZE_MIN
//blocks_total is the total number of blocks that the device will store,
//blocks_total*block_size gives the total memory size of the device
bool device_open  (BlockDevice* device, const char* device_name);
//device is the BlockDevice struct to be intialized and associated with the storage device
//device_name is expected to be the name given to a storage device by device_create
//this will restore that storage device to it's previous state, see device_save
bool device_save  (BlockDevice* device);
//device is the BlockDevice struct written to the storage device
//this must be called before device_open can be used to restore the state of a device
bool device_close (BlockDevice* device);
//device is the BlockDevice struct written to and then unassociated with the storage device
//this must be called before program termination or before reusing the BlockDevice struct
//internally calls device_save

bool block_write   (BlockDevice* device, BlockPid pid,             const void* buffer);
//pid is the permanent id of the block on the device being written to
//buffer is memory that is expected to be at least the current device's block size long
//it will be read out and store on the device at the given block
bool block_writes  (BlockDevice* device, BlockPid pid, int offset, const void* buffer, int buffer_size);
//same as block_write, except
//offset is the number of bytes from the beginning of the block to begin writing buffer at
//buffer_size is the number of bytes to be read from buffer to the block
//the contents of the block outside of those written to will remain unchanged, allowing for precise writes
//if
bool block_writes_m(BlockDevice* device, BlockPid pid, int offset, const void* buffer, int buffer_size);
//same as block_writes, except relaxes all safety and debug checks on the validity of passed parameters
//notably will allow the user to write to block 0, the block storing data created by device_save

bool block_read   (BlockDevice* device, BlockPid pid,             void* buffer);
//pid is the permanent id of the block on the device being read from
//buffer is memory that is expected to be at least the current device's block size long
//it will be written to with the entire stored contents of the given block
bool block_reads  (BlockDevice* device, BlockPid pid, int offset, void* buffer, int buffer_size);
//same as block_read, except just like block_writes, allows for a block offset and buffer size to be passed as well
bool block_reads_m(BlockDevice* device, BlockPid pid, int offset, void* buffer, int buffer_size);
//same as block_reads, except just like block_writes_m, relaxes all safety and debug checks on the validity of passed parameters

BlockPid block_alloc(BlockDevice* device);
//returns the pid of an unalloced block, or 0 on failure
bool     block_free (BlockDevice* device, BlockPid pid);//Returns 1 on success and 0 on failure
//pid is the permanent id of an allocated block
//the block gets unallocated, and reads and writes to it become undefined



#endif

#ifdef BLOCK_DEVICE_IMPLEMENTATION
#undef BLOCK_DEVICE_IMPLEMENTATION

bool device_create(BlockDevice* device, const char* device_name, int block_size, uint64 blocks_total) {
	ASSERT(block_size >= BLOCK_SIZE_MIN);
	int device_id = open(device_name, O_RDWR | O_CREAT, 0644);
    // octcal 644 is owner read/write, everyone else read-only - see 'man chmod'

    if(device_id < 0) {
		return 0;
    }
    int succ = ftruncate(device_id, block_size*blocks_total);
    if(succ != 0) {
        return 0;
    }
	// int i = 0;
	// while(i < block_size*blocks_total) {
	// 	char buffer[KILOBYTE] = {0};
	// 	if(!write(device_id, buffer, KILOBYTE)) return 0;
	// 	i += KILOBYTE;
	// }
	device->device_id = device_id;
	device->block_size = block_size;
	device->blocks_total = blocks_total;
	device->master.cookie = 1234567890;
	device->master.first_unused_block = 0;
	device->master.last_block = 1;
	return 1;
}
bool device_open(BlockDevice* device, const char* device_name) {
	int device_id = open(device_name, O_RDWR | O_CREAT, 0644);
	if(device_id < 0) {
		return 0;
	}
	int read_size = sizeof(device->persistent_data);
    int n = read(device_id, &device->persistent_data, read_size);
    if(n != read_size) {
        // raise some heck here - should never happen!
		return 0;
    } else {
		device->device_id = device_id;
		return 1;
	}
}
bool device_save(BlockDevice* device) {
	return block_writes_m(device, 0, 0, &device->persistent_data, sizeof(device->persistent_data));
}
bool device_close(BlockDevice* device) {
	if(device_save(device)) {
		int ok = close(device->device_id);
		if(ok != 0) {
			//file didn't close???
		} else {
			device->device_id = 0;
		}
		return 1;
	} else return 0;
}

bool block_read(BlockDevice* device, BlockPid pid, void* buffer) {
	ASSERT(pid);
	if(!pid) {
		return 0;
	}
	ASSERT(pid < device->blocks_total);
    int off = pid*device->block_size;
    int succ = lseek(device->device_id, off, SEEK_SET);
    if(succ != off) {
		//seek failed
        return 0;
    }
	int read_size = device->block_size;
    int n = read(device->device_id, buffer, read_size);
    if(n != read_size) {
        // raise some heck here - should never happen!
		return 0;
    }
    return 1;
}
bool block_reads_m(BlockDevice* device, BlockPid pid, int offset, void* buffer, int buffer_size) {
	if(!buffer_size) return 1;
    int off = pid*device->block_size + offset;
    int succ = lseek(device->device_id, off, SEEK_SET);
    if(succ != off) {
		//seek failed
        return 0;
    }
	int read_size = buffer_size;
    int n = read(device->device_id, buffer, read_size);
    if(n != read_size) {
        // raise some heck here - should never happen!
		return 0;
    }
    return 1;
}
bool block_reads(BlockDevice* device, BlockPid pid, int offset, void* buffer, int buffer_size) {
	ASSERT(pid);
	ASSERT(pid < device->blocks_total);
	ASSERT(offset >= 0);
	ASSERT(buffer_size >= 0);
	ASSERT(offset + buffer_size >= 0);
	ASSERT(offset + buffer_size <= device->block_size);
	if(pid && (offset + buffer_size <= device->block_size)) {
		return block_reads_m(device, pid, offset, buffer, buffer_size);
	} else {
		return 0;
	}
}

bool block_write(BlockDevice* device, BlockPid pid, const void* buffer) {
	ASSERT(pid);
	if(!pid) {
		return 0;
	}
	ASSERT(pid < device->blocks_total);
	int off = pid*device->block_size;
    int succ = lseek(device->device_id, off, SEEK_SET);
    if(succ != off) {
		//seek failed
        return 0;
    }
	int write_size = device->block_size;
    int n = write(device->device_id, buffer, write_size);
    if(n != write_size) {
        // raise some heck here - should never happen!
		return 0;
    }
    return 1;
}
bool block_writes_m(BlockDevice* device, BlockPid pid, int offset, const void* buffer, int buffer_size) {
	if(!buffer_size) return 1;
	int off = pid*device->block_size + offset;
    int succ = lseek(device->device_id, off, SEEK_SET);
    if(succ != off) {
		//seek failed
        return 0;
    }
	int write_size = buffer_size;
    int n = write(device->device_id, buffer, write_size);
    if(n != write_size) {
        // raise some heck here - should never happen!
		return 0;
    }
    return 1;
}
bool block_writes(BlockDevice* device, BlockPid pid, int offset, const void* buffer, int buffer_size) {
	ASSERT(pid);
	ASSERT(pid < device->blocks_total);
	ASSERT(offset >= 0);
	ASSERT(buffer_size >= 0);
	ASSERT(offset + buffer_size >= 0);
	ASSERT(offset + buffer_size <= device->block_size);
	if(pid && (offset + buffer_size <= device->block_size)) {
		return block_writes_m(device, pid, offset, buffer, buffer_size);
	} else {
		return 0;
	}
}


bool block_free(BlockDevice* device, BlockPid pid) {
	MasterBlock* master = &device->master;
	ASSERT(pid < master->last_block);
	if(block_writes(device, pid, 0, &master->first_unused_block, sizeof(BlockPid))) {
		master->first_unused_block = pid;
		return 1;
	} else {
		return 0;
	}

}
BlockPid block_alloc(BlockDevice* device) {
	MasterBlock* master = &device->master;
	BlockPid block = master->first_unused_block;
	if(block) {
		//TODO: Read less of the block and error check
		BlockPid next_block;
		if(block_reads(device, block, 0, &next_block, sizeof(BlockPid))) {
			ASSERT(next_block < master->last_block);
			master->first_unused_block = next_block;
		} else {
			return 0;
		}
	} else {
		block = master->last_block;
		if(block >= device->blocks_total) {
			return 0;
		}
		master->last_block += 1;
	}

	return block;
}

#endif

#ifdef __cplusplus
}
#endif
