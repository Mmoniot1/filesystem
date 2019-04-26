//By Monica Moniot
#ifdef __cplusplus
extern "C" {
#endif

#ifndef FS__H_INCLUDE
#define FS__H_INCLUDE

#include "basic.h"

typedef struct File File;
typedef struct FS FS;

#define DIR_CACHE_SIZE 32*MEGABYTE

bool fs_init   (FS* fs, const char* device_name, uint64 device_capacity);
bool fs_mount  (FS* fs, const char* device_name);
bool fs_unmount(FS* fs);
bool fs_save   (FS* fs);

File* fs_get_root(FS* fs);

bool fs_get_file (FS* fs, File* dir, const char* name, uint16 name_size, File** ret_file);
bool fs_open_file(FS* fs, File* dir, const char* name, uint16 name_size, File** ret_file);
bool fs_get_dir  (FS* fs, File* dir, const char* name, uint16 name_size, File** ret_file);
bool fs_open_dir (FS* fs, File* dir, const char* name, uint16 name_size, File** ret_file);
bool fs_get_any  (FS* fs, File* dir, const char* name, uint16 name_size, File** ret_file);

bool fs_is_dir(File* file);
bool fs_get_first_child(FS* fs, File* dir, File** ret_file);
File* fs_get_next_child(FS* fs, File* dir, File* child);

uint fs_filename_size(File* file);
void fs_get_filename(File* file, char* ret_name);
void fs_set_filename(FS* fs, File* file, const char* name, uint16 name_size);
int  fs_cmp_filename(File* file, const char* name, uint16 name_size);

uint64 fs_get_size(File* file);
bool fs_set_size(FS* fs, File* file, uint64 mem_size);
bool fs_read    (FS* fs, File* file, uint64 mem_offset,       void* mem, uint64 mem_size);
bool fs_write   (FS* fs, File* file, uint64 mem_offset, const void* mem, uint64 mem_size);



#include "block_device.h"
#include "inode.h"
#include "mam_alloc.h"


#define DIR_IS_CACHED 0b1
#define FILE_IS_DIRTY  0b10
// #define FILE_IN_USE  0b100
struct File {
	struct Filename* name;
	struct File* next;
	struct File* head_child;
	uint16 name_size;
	uint16 flags;
	INode inode;
};
#define FILENAME_TEXT_SIZE (sizeof(File) - sizeof(struct Filename*))
typedef struct Filename {
	struct Filename* next;
	char text[FILENAME_TEXT_SIZE];
} Filename;

//TODO: create a proper directory cache with an eviction policy
struct FS {
	BlockDevice device;
	INodeAllocator inode_a;
	File root;
	MamPool* dir_cache;
};


typedef struct DirEntryHeader {
	INodePid pid;
	uint16 name_size;
} DirEntryHeader;
#define SIZEOF_HEADER (sizeof(INodePid) + sizeof(uint16))

#define FS_BLOCK_SIZE 512


#endif

#ifdef FS_IMPLEMENTATION
#undef FS_IMPLEMENTATION

#define BLOCK_DEVICE_IMPLEMENTATION
#include "block_device.h"
#define INODE_IMPLEMENTATION
#include "inode.h"
#define MAM_ALLOC_IMPLEMENTATION
#include "mam_alloc.h"

static bool fs_init_file(FS* fs, File* file, const char* name, uint16 name_size, uint16 status) {
	file->head_child = 0;
	file->next = 0;
	file->flags = (status == INODE_DIR) ? DIR_IS_CACHED : 0;

	//TODO: eliminate this call so this function never fails
	if(!inode_create(&fs->device, &fs->inode_a, 0, &file->inode)) return 0;
	file->inode.status = status;

	file->name = 0;
	fs_set_filename(fs, file, name, name_size);
	return 1;
}

static File* fs_create_file(FS* fs, File* parent, const char* name, uint16 name_size, uint16 status) {
	//TODO: properly handle these asserts
	ASSERT(name_size > 0);
	if(!name_size) return 0;
	if(parent->inode.status != INODE_DIR || (name_size == 0)) return 0;
	//TODO: consider rounding the filename size?
	// name_size = sizeof(INodePid)*((name_size + sizeof(INodePid) - 1)/sizeof(INodePid));
	//TODO: handle eviction policy
	File* new_child = mam_pool_alloc(File, fs->dir_cache);
	if(!fs_init_file(fs, new_child, name, name_size, status)) return 0;
	//add new child to the directory's children list
	//TODO: handle children with the same name
	new_child->next = parent->head_child;
	parent->head_child = new_child;
	parent->flags |= FILE_IS_DIRTY;
	return new_child;
}
static bool fs_save_dir(FS* fs, File* dir) {
	ASSERT(dir->inode.status == INODE_DIR);
	File* cur_child = dir->head_child;
	uint64 cur_offset = 0;
	while(cur_child) {
		DirEntryHeader header = {cur_child->inode.pid, cur_child->name_size};
		if(!inode_write(&fs->device, &dir->inode, cur_offset, &header, SIZEOF_HEADER)) return 0;
		cur_offset += SIZEOF_HEADER;
		//write the name immediately after
		Filename* cur_filename = cur_child->name;
		uint cur_left = cur_child->name_size;
		while(cur_left > FILENAME_TEXT_SIZE) {
			if(!inode_write(&fs->device, &dir->inode, cur_offset, &cur_filename->text, FILENAME_TEXT_SIZE)) return 0;
			cur_offset += FILENAME_TEXT_SIZE;
			cur_left -= FILENAME_TEXT_SIZE;
			cur_filename = cur_filename->next;
		}
		if(!inode_write(&fs->device, &dir->inode, cur_offset, &cur_filename->text, cur_left)) return 0;
		cur_offset += cur_left;
		//move on
		cur_child = cur_child->next;
	}
	if(!inode_set_size(&fs->device, &dir->inode, cur_offset)) return 0;
	dir->flags &= ~FILE_IS_DIRTY;
	if(!inode_save(&fs->device, &dir->inode)) return 0;
	return 1;
}

static bool fs_save_all_(FS* fs, File* dir) {
	//TODO: inline save_dir to only visit each node once?
	if(dir->flags & FILE_IS_DIRTY) {
		if(!fs_save_dir(fs, dir)) return 0;
	}
	File* cur_child = dir->head_child;
	while(cur_child) {
		if(cur_child->inode.status == INODE_DIR) {
			if(!fs_save_all_(fs, cur_child)) return 0;
		} else if(cur_child->flags & FILE_IS_DIRTY) {
			cur_child->flags &= ~FILE_IS_DIRTY;
			if(!inode_save(&fs->device, &cur_child->inode)) return 0;
		}
		//move on
		cur_child = cur_child->next;
	}
	return 1;
}

static bool fs_restore_dir(FS* fs, File* dir) {
	ASSERT(dir->inode.status == INODE_DIR);
	if(dir->flags & DIR_IS_CACHED) return 1;

	uint64 cur_offset = 0;
	uint64 cur_left = dir->inode.mem_size;
	//NOTE: this function will fail horribly if the dir is corrupted, maybe fix?
	dir->head_child = 0;
	while(cur_offset < cur_left) {
		DirEntryHeader header;
		if(!inode_read(&fs->device, &dir->inode, cur_offset, &header, SIZEOF_HEADER)) return 0;
		cur_offset += SIZEOF_HEADER;
		{//cache the new file
			File* new_file = mam_pool_alloc(File, fs->dir_cache);
			new_file->head_child = 0;
			//NOTE: the flags here are initialized to 0
			new_file->flags = 0;
			if(!inode_restore(&fs->device, header.pid, &new_file->inode)) return 0;

			{//copy the filename into the dir cache
				Filename* cur_filename = mam_pool_alloc(Filename, fs->dir_cache);
				new_file->name = cur_filename;
				new_file->name_size = header.name_size;

				int cur_text_left = header.name_size;
				while(cur_text_left > FILENAME_TEXT_SIZE) {
					if(!inode_read(&fs->device, &dir->inode, cur_offset, &cur_filename->text, FILENAME_TEXT_SIZE)) return 0;
					cur_offset += FILENAME_TEXT_SIZE;
					cur_text_left -= FILENAME_TEXT_SIZE;
					//create another filename array
					cur_filename->next = mam_pool_alloc(Filename, fs->dir_cache);
					cur_filename = cur_filename->next;
				}
				if(!inode_read(&fs->device, &dir->inode, cur_offset, &cur_filename->text, cur_text_left)) return 0;
				cur_offset += cur_text_left;
				cur_filename->next = 0;
			}
			new_file->next = dir->head_child;
			dir->head_child = new_file;
		}
	}
	dir->flags |= DIR_IS_CACHED;
	return 1;
}


bool fs_init(FS* fs, const char* device_name, uint64 device_capacity) {
	//NOTE: device_capacity may be rounded down
	if(!device_create(&fs->device, device_name, FS_BLOCK_SIZE, device_capacity/FS_BLOCK_SIZE)) return 0;
	inode_initfs(&fs->inode_a, FS_BLOCK_SIZE);
	//TODO: manage the pool as a cache
	fs->dir_cache = mam_pool_init(File, malloc(DIR_CACHE_SIZE), DIR_CACHE_SIZE);
	if(!fs_init_file(fs, &fs->root, "/", 1, INODE_DIR)) return 0;
	return 1;
}
bool fs_mount(FS* fs, const char* device_name) {
	if(!device_open(&fs->device, device_name) || !inode_mountfs(&fs->device, &fs->inode_a)) return 0;
	//TODO: manage the pool as a cache
	fs->dir_cache = mam_pool_init(File, malloc(DIR_CACHE_SIZE), DIR_CACHE_SIZE);
	INodePid root_pid;
	block_reads_m(&fs->device, 0, sizeof(fs->device.persistent_data) + sizeof(INodeAllocator), &root_pid, sizeof(INodePid));
	{// restore root
		if(!root_pid) return 0;
		if(!inode_restore(&fs->device, root_pid, &fs->root.inode)) return 0;
		fs->root.head_child = 0;
		fs->root.flags = 0;
		fs->root.next = 0;
		fs->root.inode.status = INODE_DIR;
		fs->root.name = 0;
		fs_set_filename(fs, &fs->root, "/", 1);
		if(!fs_restore_dir(fs, &fs->root)) return 0;
	}
	return 1;
}
bool fs_unmount(FS* fs) {
	//write the root inode to the master block
	block_writes_m(&fs->device, 0, sizeof(fs->device.persistent_data) + sizeof(INodeAllocator), &fs->root.inode.pid, sizeof(INodePid));
	return fs_save_all_(fs, &fs->root) && inode_unmountfs(&fs->device, &fs->inode_a) && device_close(&fs->device);
	free(fs->dir_cache);
}
bool fs_save(FS* fs) {
	//write the root inode to the master block
	block_writes_m(&fs->device, 0, sizeof(fs->device.persistent_data) + sizeof(INodeAllocator), &fs->root.inode.pid, sizeof(INodePid));
	return fs_save_all_(fs, &fs->root) && inode_unmountfs(&fs->device, &fs->inode_a) && device_save(&fs->device);
}

File* fs_get_root(FS* fs) {
	return &fs->root;
}

int fs_cmp_filename(File* file, const char* name, uint16 name_size) {
	Filename* filename = file->name;
	uint cur_left = file->name_size;
	while(cur_left > FILENAME_TEXT_SIZE) {
		int c = memcmp(name, &filename->text, FILENAME_TEXT_SIZE);
		if(c != 0) return c;
		cur_left -= FILENAME_TEXT_SIZE;
		name += FILENAME_TEXT_SIZE;
		filename = filename->next;
	}
	return memcmp(name, &filename->text, cur_left);
}


bool fs_get_any(FS* fs, File* dir, const char* name, uint16 name_size, File** ret_file) {
	//NOTE: will cache dir always, failure only occurs then
	ASSERT(fs_is_dir(dir));
	*ret_file = 0;
	if(!fs_restore_dir(fs, dir)) return 0;
	File* cur_child = dir->head_child;
	while(cur_child) {
		//write the name immediately after
		if(fs_cmp_filename(cur_child, name, name_size) == 0) {
			*ret_file = cur_child;
			return 1;
		};
		cur_child = cur_child->next;
	}
	return 1;
}

bool fs_get_file(FS* fs, File* dir, const char* name, uint16 name_size, File** ret_file) {
	if(!fs_get_any(fs, dir, name, name_size, ret_file)) return 0;
	if(*ret_file && fs_is_dir(*ret_file)) {
		//the item is a directory so reject
		*ret_file = 0;
	}
	return 1;
}
bool fs_open_file(FS* fs, File* dir, const char* name, uint16 name_size, File** ret_file) {
	if(!fs_get_any(fs, dir, name, name_size, ret_file)) return 0;
	if(*ret_file) {
		if(fs_is_dir(*ret_file)) {
			//the item is a directory so reject
			*ret_file = 0;
		}
	} else {
		*ret_file = fs_create_file(fs, dir, name, name_size, INODE_FILE);
		return *ret_file != 0;
	}
	return 1;
}
bool fs_get_dir(FS* fs, File* dir, const char* name, uint16 name_size, File** ret_dir) {
	if(!fs_get_any(fs, dir, name, name_size, ret_dir)) return 0;
	if(*ret_dir && !fs_is_dir(*ret_dir)) {
		//the item is a file so reject
		*ret_dir = 0;
	}
	return 1;
}
bool fs_open_dir(FS* fs, File* dir, const char* name, uint16 name_size, File** ret_dir) {
	if(!fs_get_any(fs, dir, name, name_size, ret_dir)) return 0;
	if(*ret_dir) {
		if(!fs_is_dir(*ret_dir)) {
			//the item is a file so reject
			*ret_dir = 0;
		}
	} else {
		*ret_dir = fs_create_file(fs, dir, name, name_size, INODE_DIR);
		return *ret_dir != 0;
	}
	return 1;
}



bool fs_is_dir(File* file) {
	return file->inode.status == INODE_DIR;
}
bool fs_get_first_child(FS* fs, File* dir, File** ret_file) {
	*ret_file = 0;
	if(dir->inode.status == INODE_DIR) {
		if(!fs_restore_dir(fs, dir)) return 0;
		*ret_file = dir->head_child;
	}
	return 1;
}
File* fs_get_next_child(FS* fs, File* dir, File* child) {
	return child->next;
}


uint fs_filename_size(File* file) {
	return file->name_size;
}
void fs_get_filename(File* file, char* ret_name) {
	Filename* cur_filename = file->name;
	uint cur_left = file->name_size;
	while(cur_left > FILENAME_TEXT_SIZE) {
		memcpy(ret_name, &cur_filename->text, FILENAME_TEXT_SIZE);
		cur_left -= FILENAME_TEXT_SIZE;
		ret_name += FILENAME_TEXT_SIZE;
		cur_filename = cur_filename->next;
	}
	memcpy(ret_name, &cur_filename->text, cur_left);
}
void fs_set_filename(FS* fs, File* file, const char* name, uint16 name_size) {
	if(!name_size) return;
	Filename* cur_filename = file->name;
	uint cur_left = name_size;
	if(!cur_filename) {
		cur_filename = mam_pool_alloc(Filename, fs->dir_cache);
		cur_filename->next = 0;
		file->name = cur_filename;
	}
	//copy the filename into the dir cache
	while(cur_left > FILENAME_TEXT_SIZE) {
		memcpy(&cur_filename->text, name, FILENAME_TEXT_SIZE);
		cur_left -= FILENAME_TEXT_SIZE;
		name += FILENAME_TEXT_SIZE;
		cur_filename = cur_filename->next;
		if(!cur_filename) {
			cur_filename = mam_pool_alloc(Filename, fs->dir_cache);
			cur_filename->next = 0;
		}
	}
	memcpy(&cur_filename->text, name, cur_left);
	file->name_size = name_size;
}


uint64 fs_get_size(File* file) {
	return file->inode.mem_size;
}
bool fs_set_size(FS* fs, File* file, uint64 mem_size) {
	file->flags |= FILE_IS_DIRTY;
	return inode_set_size(&fs->device, &file->inode, mem_size);
}
bool fs_read (FS* fs, File* file, uint64 mem_offset,       void* mem, uint64 mem_size) {
	return inode_read(&fs->device, &file->inode, mem_offset, mem, mem_size);
}
bool fs_write(FS* fs, File* file, uint64 mem_offset, const void* mem, uint64 mem_size) {
	file->flags |= FILE_IS_DIRTY;
	return inode_write(&fs->device, &file->inode, mem_offset, mem, mem_size);
}


#endif

#ifdef __cplusplus
}
#endif
