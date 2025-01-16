#define FUSE_USE_VERSION 30

#include "stdio.h"
#include "unistd.h"
#include "stdlib.h"
#include "string.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "sys/types.h"
#include "sys/mman.h"
#include "time.h"
#include "getopt.h"
#include "fuse.h"
#include "errno.h"
#include "hfs.h"
#include "stdbool.h"

#define MAX_PATH_NAME 264
#define MAX_DISKS 16
#define SUCCESS 0
#define FAIL -1

// Global vars
static char **disks;
static struct hfs_sb *superblock;
static int num_disks;
static int *fileDescs;
size_t diskSize;

void split_path(const char *path, char *parent_path, char *new_name) {
    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) {
        strcpy(parent_path, "/");
        strcpy(new_name, last_slash ? last_slash + 1 : path);
    }
    else{
        size_t parent_len = last_slash - path;
        strncpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        strcpy(new_name, last_slash + 1);
    }
}

char* get_inode_block_addr(int disk_idx, int inode_idx) {
    return (char *) (disks[disk_idx]) + (superblock->i_blocks_ptr) + (inode_idx * BLOCK_SIZE);
}

char* get_data_block_addr(int disk_idx, int inode_idx, off_t block_idx) {
    return (char *) (disks[disk_idx]) + (superblock->d_blocks_ptr) + (inode_idx * (superblock->num_data_blocks / superblock->num_inodes) * BLOCK_SIZE) + (block_idx * BLOCK_SIZE);
}

int raid_read_block(int disk_idx, int inode_idx, off_t block_idx, char *data, size_t size) {
    if (superblock -> mode == 1) {
        for (int i = 0; i < superblock->num_disks; i++) {
            if (block_idx >= 0) {
                char *block_addr = disks[i] + superblock->d_blocks_ptr + block_idx * BLOCK_SIZE;
                memcpy(data, block_addr, size);
                return SUCCESS;
            }
        }
        fprintf(stderr, "raid_read_block: No valid block found for inode %d, block %ld\n", inode_idx, block_idx);
        return FAIL;
    }
    
    fprintf(stderr, "raid_read_block: Unsupported RAID mode %d\n", superblock -> mode);
    return FAIL;
}

struct hfs_inode* get_inode(off_t index) {
    printf("Entering get_inode\n");
    if (index < 0 || index >= superblock->num_inodes) {
        printf("get_inode: failed\n");
        return NULL;
    }

    char* inode_offset = (char*) disks[0] + superblock->i_blocks_ptr; 

    printf("Returing from get_inode\n");
    return (struct hfs_inode*)((char*)inode_offset + index * BLOCK_SIZE);
}

static int allocate_data_block() {
    for (int i = 0; i < superblock->num_data_blocks; i++) {
        if (superblock->mode == 0) {
            int disk_idx = i % superblock->num_disks;
            char *data_bitmap = (char*)disks[disk_idx] + superblock->d_bitmap_ptr;
            if (!(data_bitmap[i / 8] & (1 << (i % 8)))) {
                data_bitmap[i / 8] |= (1 << (i % 8));
                return i;
            }
        } else {
            int is_free = 1;
            for (int j = 0; j < superblock->num_disks; j++) {
                char *data_bitmap = (char*)disks[j] + superblock->d_bitmap_ptr;
                if (data_bitmap[i / 8] & (1 << (i % 8))) {
                    is_free = 0;
                    break;
                }
            }
            
            if (is_free) {
                for (int disk = 0; disk < superblock->num_disks; disk++) {
                    char *data_bitmap = (char*)disks[disk] + superblock->d_bitmap_ptr;
                    data_bitmap[i / 8] |= (1 << (i % 8));
                }
                return i;
            }
        }
    }
    return -ENOSPC;
}

static int allocate_inode() {    
    for (int i = 0; i < superblock->num_inodes; i++) {
        // Check inode free on ALL disks
        int is_free = 1;
        for (int j = 0; j < superblock->num_disks; j++) {
            char *disk_bitmap = (char*)disks[j] + superblock->i_bitmap_ptr;
            if (disk_bitmap[i / 8] & (1 << (i % 8))) {
                is_free = 0;
                break;
            }
        }
        
        if (is_free) {
            // Mark used on ALL disks
            for (int disk = 0; disk < superblock->num_disks; disk++) {
                char *disk_bitmap = (char*)disks[disk] + superblock->i_bitmap_ptr;
                disk_bitmap[i / 8] |= (1 << (i % 8));
            }
            return i;
        }
    }
    
    return -ENOSPC;
}

static off_t find_inode(const char *path) {
    printf("Entering find_inode: path = %s\n", path);
    if (strcmp(path, "/") == 0) {
        printf("find_inode: path is root\n");
        return 0;
    }

    printf("%s\n", path);
    char temp_path[MAX_PATH_NAME];
    strncpy(temp_path, path, MAX_PATH_NAME-1);
    char *component = temp_path[0] == '/' ? temp_path + 1 : temp_path;
    char *token = strtok(component, "/");
    int current_inode = 0;

    while (token != NULL) {
        struct hfs_inode *dir_inode;
        dir_inode = get_inode(current_inode);

        if (!(dir_inode->mode & S_IFDIR)) {
            printf("Not a directory\n");
            return -ENOTDIR;
        }

        int found = 0;
        for (int block_index = 0; block_index < N_BLOCKS; block_index++) {
            if (dir_inode->blocks[block_index] == -1) continue;

            // Iterate over all entries in the current block
            for (int offset = 0; offset < BLOCK_SIZE; offset += sizeof(struct hfs_dentry)) {
                struct hfs_dentry *entry = (struct hfs_dentry *)(disks[0] + superblock->d_blocks_ptr + dir_inode->blocks[block_index] * BLOCK_SIZE + offset);
                
                if (strlen(entry->name) <= 0) continue;

                if (strcmp(entry->name, token) == 0) {
                    current_inode = entry->num;
                    found = 1;
                    break;
                }
            }
            if (found) break;
        }

        if (!found) {
            printf("Find inode exiting did not find\n");
            return -ENOENT;
        }
        
        token = strtok(NULL, "/");
    }
    printf("find_inode: Successfully returning inode %i\n", current_inode);
    return current_inode;
}

static int hfs_getattr(const char *path, struct stat *stbuf) {
    printf("Entering hfs_getattr: Path = %s\n", path);
    memset(stbuf, 0, sizeof(struct stat));
    int inode_idx = find_inode(path);
    printf("Inode idx: %i\n", inode_idx);
    if (inode_idx < 0) {
        fprintf(stderr, "hfs_getattr: Found invalid inode index\n");
        return inode_idx;
    }
    struct hfs_inode *inode = get_inode(inode_idx);
    if (!inode) {
        fprintf(stderr, "hfs_getattr: Invalid inode index, cannot find inode\n");
        return -ENOENT;
    }
    //printf("hfs_getattr: Nlinks=%i, size=%li, num=%i\n", inode->nlinks, inode->size, inode->num);
    //printf("Going to edit attributes\n");
    stbuf->st_mode = inode->mode;
    stbuf->st_nlink = inode->nlinks;
    stbuf->st_size = inode->size;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_ctime = inode->ctim;
    //printf("edited attributes\n");
    //printf("S_IFDIR: %o\n", S_IFDIR);
    //printf("Mode %o\n", stbuf -> st_mode);
    return SUCCESS;
}

static int hfs_mknod(const char *path, mode_t mode, dev_t dev) {
    printf("Entering hfs_mknod\n");
    printf("hfs_mknod: path = %s\n", path);
    char parentPath[MAX_PATH_NAME];
    char origPath[MAX_PATH_NAME];
    char childPath[MAX_PATH_NAME];
    strcpy(origPath, path);
    split_path(origPath, parentPath, childPath);
    int parentInodeIdx = find_inode(parentPath);
    printf("hfs_mknod: Parent inode index: %i\n", parentInodeIdx);

    if (parentInodeIdx < 0) return -ENOENT;

    if (find_inode(childPath) >= 0) return -EEXIST;

    int childInodeIdx = allocate_inode();
    printf("hfs_mknod: Inode index: %i\n", childInodeIdx);
    if (childInodeIdx < 0) return -ENOSPC;

    struct hfs_inode childInode = {0};
    childInode.mode = mode | S_IFREG;
    childInode.num = childInodeIdx;
    childInode.nlinks = 1;
    childInode.uid = getuid();
    childInode.gid = getgid();
    childInode.atim = childInode.mtim = childInode.ctim = time(NULL);
    childInode.size = 0;
    for (int i = 0; i < N_BLOCKS; i++) {
        childInode.blocks[i] = -1;
    }

    printf("hfs_mknod: Starting directory entry logic!\n");
    struct hfs_inode* parentInode = get_inode(parentInodeIdx);

    // Find Block and dEntry
    int blockIdx = -1;
    int dataBlockIdx = -1;
    int dentryIdx = -1;
    int allocd = -1;
    // Find first empty block (allocate block and use first dentry) or first empty dentry within alloced block
    for (int i = 0; i < N_BLOCKS; i++) {
        if (parentInode->blocks[i] == -1) {
            //printf("directory logic: first parent block empty allocing\n");
            // Allocate a new block for directory entries
            dataBlockIdx = allocate_data_block();
            if (dataBlockIdx < 0){
                //printf("directory logic: BAD ALLOCATED BLOCK\n");
                return -ENOSPC;
            }
            blockIdx = i;
            dentryIdx = 0;
            allocd = 1;
            //printf("directory logic: location - parentInode->blocks[%d]=%d at dentryIdx=%d\n", blockIdx, dataBlockIdx, dentryIdx);
            break;
        } else {
            struct hfs_dentry *dentry = (struct hfs_dentry*)(disks[0] + superblock->d_blocks_ptr + parentInode->blocks[i] * BLOCK_SIZE);
            for (int j = 0; j * sizeof(struct hfs_dentry) < BLOCK_SIZE; j++) {
                if (dentry[j].num <= 0) {
                    //printf("directory logic: found room in alloced dBlock\n");
                    blockIdx = i;
                    dataBlockIdx = parentInode->blocks[i];
                    dentryIdx = j;
                    //printf("directory logic: location - parentInode->blocks[%d]=%d at dentryIdx=%d\n", blockIdx, dataBlockIdx, dentryIdx);
                    break;
                }
            }
            if (blockIdx != -1 && dentryIdx != -1) break;
        }
    }

    if (blockIdx == -1 && dentryIdx == -1) return -ENOSPC;

    // create
    struct hfs_dentry entry = {0};
    strncpy(entry.name, childPath, MAX_NAME - 1);
    entry.num = childInodeIdx;

    // put in
    // printf("directory logic: updating disk info\n");
    for(int i = 0; i < superblock->num_disks; i++) {
        // place childInode
        // printf("directory logic: placing childInode into disk=%d childInodeIdx=%d\n", i, childInodeIdx);
        memcpy(disks[i] + superblock->i_blocks_ptr + childInodeIdx * BLOCK_SIZE, &childInode, BLOCK_SIZE);

        struct hfs_inode *parentInodePtr = (struct hfs_inode*)(disks[i] + superblock->i_blocks_ptr + parentInodeIdx * BLOCK_SIZE);
        // update blocks if we alloced
        if (allocd == 1) {
            // printf("directory logic: alloc -> updating alloc disk info\n");
            parentInodePtr->blocks[blockIdx] = dataBlockIdx;
            memset((disks[i] + superblock->d_blocks_ptr + dataBlockIdx * BLOCK_SIZE), 0, BLOCK_SIZE);
            // printf("directory logic: set parentInodePtr->blocks[%d]=%d\n", blockIdx, dataBlockIdx);
            // printf("directory logic: cleared dataBlockIdx=%d\n", dataBlockIdx);
        } else {
            // only clear entry
            // printf("directory logic: no alloc -> clearing parentInode->blocks[%d]\n", blockIdx);
            memset(disks[i] + superblock->d_blocks_ptr + parentInode->blocks[blockIdx] * BLOCK_SIZE + dentryIdx * sizeof(struct hfs_dentry), 0, sizeof(struct hfs_dentry));
        }

        // copy in dentry
        // printf("directory logic: copying in dentry \n", blockIdx);
        memcpy((disks[i] + superblock->d_blocks_ptr + dataBlockIdx * BLOCK_SIZE + dentryIdx * sizeof(struct hfs_dentry)), &entry, sizeof(struct hfs_dentry));
        // printf("directory logic: copied entry into dataBlockIdx=%d dentryIdx=%d\n", dataBlockIdx, dentryIdx);

        // edit parentInode
        parentInodePtr->nlinks++;
        parentInodePtr->size += sizeof(struct hfs_dentry);
    }

    printf("Returning from mkdir\n");
    return SUCCESS;
}

static int hfs_mkdir(const char *path, mode_t mode) {
    printf("Entering hfs_mkdir\n");
    printf("hfs_mkdir: path = %s\n", path);
    char parentPath[MAX_PATH_NAME];
    char origPath[MAX_PATH_NAME];
    char childPath[MAX_PATH_NAME];
    strcpy(origPath, path);
    split_path(origPath, parentPath, childPath);
    int parentInodeIdx = find_inode(parentPath);
    printf("hfs_mkdir: Parent inode index: %i\n", parentInodeIdx);

    if (parentInodeIdx < 0) return -ENOENT;

    if (find_inode(childPath) >= 0) return -EEXIST;

    int childInodeIdx = allocate_inode();
    printf("hfs_mkdir: Inode index: %i\n", childInodeIdx);
    if (childInodeIdx < 0) return -ENOSPC;

    struct hfs_inode childInode = {0};
    childInode.mode = (mode & 0777) | S_IFDIR;
    childInode.num = childInodeIdx;
    childInode.nlinks = 2;
    childInode.uid = getuid();
    childInode.gid = getgid();
    childInode.atim = childInode.mtim = childInode.ctim = time(NULL);
    childInode.size = 0;
    for (int i = 0; i < N_BLOCKS; i++) {
        childInode.blocks[i] = -1;
    }

    printf("hfs_mkdir: Starting directory entry logic!\n");
    struct hfs_inode* parentInode = get_inode(parentInodeIdx);

    // Find Block and dEntry
    int blockIdx = -1;
    int dataBlockIdx = -1;
    int dentryIdx = -1;
    int allocd = -1;
    // Find first empty block (allocate block use first dentry) or first empty dentry within alloced block
    for (int i = 0; i < IND_BLOCK; i++) {
        if (parentInode->blocks[i] == -1) {
            printf("directory logic: first parent block empty allocing\n");
            dataBlockIdx = allocate_data_block();
            if (dataBlockIdx < 0){
                printf("directory logic: BAD ALLOCATED BLOCK\n");
                return -ENOSPC;
            }
            blockIdx = i;
            dentryIdx = 0;
            allocd = 1;
            printf("directory logic: location - parentInode->blocks[%d]=%d at dentryIdx=%d\n", blockIdx, dataBlockIdx, dentryIdx);
            break;
        } else {
            struct hfs_dentry *dentry = (struct hfs_dentry*)(disks[0] + superblock->d_blocks_ptr + parentInode->blocks[i] * BLOCK_SIZE);
            for (int j = 0; j * sizeof(struct hfs_dentry) < BLOCK_SIZE; j++) {
                if (dentry[j].num <= 0) {
                    printf("directory logic: found room in alloced dBlock\n");
                    blockIdx = i;
                    dataBlockIdx = parentInode->blocks[i];
                    dentryIdx = j;
                    printf("directory logic: location - parentInode->blocks[%d]=%d at dentryIdx=%d\n", blockIdx, dataBlockIdx, dentryIdx);
                    break;
                }
            }
            if (blockIdx != -1 && dentryIdx != -1) break;
        }
    }

    if (blockIdx == -1 && dentryIdx == -1) return -ENOSPC;

    // create
    struct hfs_dentry entry = {0};
    strncpy(entry.name, childPath, MAX_NAME - 1);
    entry.num = childInodeIdx;

    // put in
    printf("directory logic: updating disk info\n");
    for(int i = 0; i < superblock->num_disks; i++) {
        // place childInode
        printf("directory logic: placing childInode into disk=%d childInodeIdx=%d\n", i, childInodeIdx);
        memcpy(disks[i] + superblock->i_blocks_ptr + childInodeIdx * BLOCK_SIZE, &childInode, BLOCK_SIZE);

        struct hfs_inode *parentInodePtr = (struct hfs_inode*)(disks[i] + superblock->i_blocks_ptr + parentInodeIdx * BLOCK_SIZE);
        // update blocks if we alloced
        if (allocd == 1) {
            printf("directory logic: alloc -> updating alloc disk info\n");
            parentInodePtr->blocks[blockIdx] = dataBlockIdx;
            memset((disks[i] + superblock->d_blocks_ptr + dataBlockIdx * BLOCK_SIZE), 0, BLOCK_SIZE);
            printf("directory logic: set parentInodePtr->blocks[%d]=%d\n", blockIdx, dataBlockIdx);
            printf("directory logic: cleared dataBlockIdx=%d\n", dataBlockIdx);
        } else {
            // only clear entry
            printf("directory logic: no alloc -> clearing parentInode->blocks[%d]\n", blockIdx);
            memset(disks[i] + superblock->d_blocks_ptr + parentInode->blocks[blockIdx] * BLOCK_SIZE + dentryIdx * sizeof(struct hfs_dentry), 0, sizeof(struct hfs_dentry));
        }

        // copy in dentry
        // printf("directory logic: copying in dentry \n", blockIdx);
        memcpy((disks[i] + superblock->d_blocks_ptr + dataBlockIdx * BLOCK_SIZE + dentryIdx * sizeof(struct hfs_dentry)), &entry, sizeof(struct hfs_dentry));
        printf("directory logic: copied entry into dataBlockIdx=%d dentryIdx=%d\n", dataBlockIdx, dentryIdx);

        // edit parentInode
        parentInodePtr->nlinks++;
        parentInodePtr->size += sizeof(struct hfs_dentry);
    }

    printf("Returning from mkdir\n");
    return SUCCESS;
}

static int hfs_unlink(const char *path) {
    printf("Entering hfs_unlink: path = %s\n", path);
    char parentPath[MAX_PATH_NAME];
    char origPath[MAX_PATH_NAME];
    char fileName[MAX_PATH_NAME];
    strcpy(origPath, path);
    split_path(origPath, parentPath, fileName);

    int parentInodeIdx = find_inode(parentPath);
    if (parentInodeIdx < 0) {
        fprintf(stderr, "hfs_unlink: Parent directory not found\n");
        return parentInodeIdx;
    }
    struct hfs_inode *parentInode = get_inode(parentInodeIdx);
    if (!parentInode) {
        fprintf(stderr, "hfs_unlink: Invalid parent inode\n");
        return -ENOENT;
    }
    int inode_idx = find_inode(path);
    if (inode_idx < 0) {
        fprintf(stderr, "hfs_unlink: File not found\n");
        return inode_idx;
    }
    struct hfs_inode *inode = get_inode(inode_idx);
    if (!inode) {
        fprintf(stderr, "hfs_unlink: Invalid inode index\n");
        return -ENOENT;
    }

    if (S_ISDIR(inode->mode)) {
        fprintf(stderr, "hfs_unlink: Cannot unlink a directory\n");
        return -EISDIR;
    }

    int entry_removed = 0;
    for (int disk = 0; disk < superblock->num_disks; disk++) {
        for (int block_index = 0; block_index < N_BLOCKS; block_index++) {
            if (parentInode->blocks[block_index] == -1) {
                continue;
            }
            struct hfs_dentry *entries = (struct hfs_dentry*)(disks[disk] + superblock->d_blocks_ptr + parentInode->blocks[block_index] * BLOCK_SIZE);
            
            for (int entry_idx = 0; entry_idx * sizeof(struct hfs_dentry) < BLOCK_SIZE; entry_idx++) {
                if (entries[entry_idx].num == inode_idx) {
                    memset(&entries[entry_idx], 0, sizeof(struct hfs_dentry));
                    struct hfs_inode *parentInodePtr = (struct hfs_inode*)(disks[disk] + superblock->i_blocks_ptr + parentInodeIdx * BLOCK_SIZE);
                    parentInodePtr->size -= sizeof(struct hfs_dentry);
                    parentInodePtr->nlinks--;
                    entry_removed = 1;
                    break;
                }
            }
            if (entry_removed) break;
        }
    }

    inode->nlinks--;

    // no more links, free inode and data blocks
    if (inode->nlinks == 0) {
        for (int disk = 0; disk < superblock->num_disks; disk++) {
            char *inode_bitmap = (char*)disks[disk] + superblock->i_bitmap_ptr;
            inode_bitmap[inode_idx / 8] &= ~(1 << (inode_idx % 8));
            for (int block_idx = 0; block_idx < N_BLOCKS; block_idx++) {
                if (inode->blocks[block_idx] != -1) {
                    for (int d = 0; d < superblock->num_disks; d++) {
                        char *curr_data_bitmap = (char*)disks[d] + superblock->d_bitmap_ptr;
                        curr_data_bitmap[inode->blocks[block_idx] / 8] &= ~(1 << (inode->blocks[block_idx] % 8));
                    }
                    memset(disks[disk] + superblock->d_blocks_ptr + inode->blocks[block_idx] * BLOCK_SIZE, 0, BLOCK_SIZE);
                    inode->blocks[block_idx] = -1;
                }
            }

            memset(disks[disk] + superblock->i_blocks_ptr + inode_idx * BLOCK_SIZE, 0, BLOCK_SIZE);
        }
    }

    printf("Exiting hfs_unlink successfully\n");
    return SUCCESS;
}

static int hfs_rmdir(const char *path) {
    printf("Entering hfs_rmdir: path = %s\n", path);
    int inode_idx = find_inode(path);
    if (inode_idx < 0) {
        fprintf(stderr, "hfs_rmdir: Directory not found\n");
        return inode_idx;
    }
    struct hfs_inode *inode = get_inode(inode_idx);
    if (!inode) {
        fprintf(stderr, "hfs_rmdir: Invalid inode index\n");
        return -ENOENT;
    }

    if (!S_ISDIR(inode->mode)) {
        fprintf(stderr, "hfs_rmdir: Not a directory\n");
        return -ENOTDIR;
    }

    int is_empty = 1;
    for (int block_index = 0; block_index < N_BLOCKS; block_index++) {
        if (inode->blocks[block_index] == -1) continue;

        struct hfs_dentry *entries = (struct hfs_dentry*)(disks[0] + superblock->d_blocks_ptr + inode->blocks[block_index] * BLOCK_SIZE);
        
        for (int entry_idx = 0; entry_idx * sizeof(struct hfs_dentry) < BLOCK_SIZE; entry_idx++) {
            if (entries[entry_idx].num > 0 && 
                strcmp(entries[entry_idx].name, ".") != 0 && 
                strcmp(entries[entry_idx].name, "..") != 0) {
                is_empty = 0;
                break;
            }
        }

        if (!is_empty) break;
    }

    if (!is_empty) {
        fprintf(stderr, "hfs_rmdir: Directory is not empty\n");
        return -ENOTEMPTY;
    }

    char parentPath[MAX_PATH_NAME];
    char origPath[MAX_PATH_NAME];
    char dirName[MAX_PATH_NAME];
    strcpy(origPath, path);
    split_path(origPath, parentPath, dirName);

    int parentInodeIdx = find_inode(parentPath);
    if (parentInodeIdx < 0) {
        fprintf(stderr, "hfs_rmdir: Parent directory not found\n");
        return parentInodeIdx;
    }
    struct hfs_inode *parentInode = get_inode(parentInodeIdx);
    if (!parentInode) {
        fprintf(stderr, "hfs_rmdir: Invalid parent inode\n");
        return -ENOENT;
    }

    // rmdir entry from all disks
    for (int disk = 0; disk < superblock->num_disks; disk++) {
        for (int block_index = 0; block_index < N_BLOCKS; block_index++) {
            if (parentInode->blocks[block_index] == -1) continue;

            struct hfs_dentry *entries = (struct hfs_dentry*)(disks[disk] + superblock->d_blocks_ptr + parentInode->blocks[block_index] * BLOCK_SIZE);
            
            for (int entry_idx = 0; entry_idx * sizeof(struct hfs_dentry) < BLOCK_SIZE; entry_idx++) {
                if (entries[entry_idx].num == inode_idx) {
                    memset(&entries[entry_idx], 0, sizeof(struct hfs_dentry));
                    struct hfs_inode *parentInodePtr = (struct hfs_inode*)(disks[disk] + superblock->i_blocks_ptr + parentInodeIdx * BLOCK_SIZE);
                    parentInodePtr->size -= sizeof(struct hfs_dentry);
                    parentInodePtr->nlinks--;
                    break;
                }
            }
        }

        char *inode_bitmap = (char*)disks[disk] + superblock->i_bitmap_ptr;
        inode_bitmap[inode_idx / 8] &= ~(1 << (inode_idx % 8));

        char *data_bitmap = (char*)disks[disk] + superblock->d_bitmap_ptr;
        for (int block_idx = 0; block_idx < N_BLOCKS; block_idx++) {
            if (inode->blocks[block_idx] != -1) {
                data_bitmap[inode->blocks[block_idx] / 8] &= ~(1 << (inode->blocks[block_idx] % 8));
                memset(disks[disk] + superblock->d_blocks_ptr + inode->blocks[block_idx] * BLOCK_SIZE, 0, BLOCK_SIZE);
            }
        }

        memset(disks[disk] + superblock->i_blocks_ptr + inode_idx * BLOCK_SIZE, 0, BLOCK_SIZE);
    }

    printf("Exiting hfs_rmdir successfully\n");
    return SUCCESS;
}

static int hfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    printf("Entering hfs_read: path=%s, size=%zu, offset=%ld\n", path, size, offset);
    int inode_idx = find_inode(path);
    if (inode_idx < 0) return -ENOENT;
    
    struct hfs_inode *inode = get_inode(inode_idx);
    if (!inode) return -ENOENT;

    if (offset >= inode->size) return 0;

    if (offset + size > inode->size) {
        size = inode->size - offset;
    }

    size_t bytes_read = 0;
    while (bytes_read < size) {
        off_t current_offset = offset + bytes_read;
        int block_index = current_offset / BLOCK_SIZE;
        int block_offset = current_offset % BLOCK_SIZE;
        
        // direct or indirect
        off_t block_num;
        if (block_index < D_BLOCK) {
            block_num = inode->blocks[block_index];
        } else if (block_index < D_BLOCK + (BLOCK_SIZE / sizeof(off_t))) {
            if (inode->blocks[IND_BLOCK] == -1) {
                return bytes_read;
            }
            struct hfs_ind_block *ind_block = (struct hfs_ind_block *)(disks[0] + 
                superblock->d_blocks_ptr + inode->blocks[IND_BLOCK] * BLOCK_SIZE);
            block_num = ind_block->blocks[block_index - D_BLOCK];
        } else {
            return bytes_read;
        }

        if (block_num == -1) {
            return bytes_read;
        }

        size_t block_bytes = BLOCK_SIZE - block_offset;
        if (block_bytes > size - bytes_read) {
            block_bytes = size - bytes_read;
        }

        // RAID
        if (superblock->mode == 0) {
            int disk_index = block_num % superblock->num_disks;
            off_t local_block_num = block_num / superblock->num_disks;
            char *block_addr = disks[disk_index] + superblock->d_blocks_ptr + local_block_num * BLOCK_SIZE;
            memcpy(buf + bytes_read, block_addr + block_offset, block_bytes);
        } else {
            char *block_addr = disks[0] + superblock->d_blocks_ptr + block_num * BLOCK_SIZE;
            memcpy(buf + bytes_read, block_addr + block_offset, block_bytes);
        }
        
        bytes_read += block_bytes;
    }

    inode->atim = time(NULL);
    return bytes_read;  
}

static int hfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    printf("Entering hfs_write\n");
    int inode_idx = find_inode(path);
    if (inode_idx < 0) return -ENOENT;
    
    struct hfs_inode *inode = get_inode(inode_idx);
    if (!inode) return -ENOENT;

    size_t bytes_written = 0;
    while (bytes_written < size) {
        off_t current_offset = offset + bytes_written;
        int block_index = current_offset / BLOCK_SIZE;
        int block_offset = current_offset % BLOCK_SIZE;
        
        off_t *block_num_ptr = NULL;
        struct hfs_ind_block *ind_block = NULL;
        // direct or indirect
        if (block_index < D_BLOCK) {
            block_num_ptr = &inode->blocks[block_index];
        } else if (block_index < D_BLOCK + (BLOCK_SIZE / sizeof(off_t))) {
            if (inode->blocks[IND_BLOCK] == -1) {
                int new_block = allocate_data_block();
                if (new_block < 0) return -ENOSPC;
                inode->blocks[IND_BLOCK] = new_block;
                
                if (superblock->mode == 0) {
                    int disk_index = new_block % superblock->num_disks;
                    off_t local_block_num = new_block / superblock->num_disks;
                    ind_block = (struct hfs_ind_block *)(disks[disk_index] + superblock->d_blocks_ptr + local_block_num * BLOCK_SIZE);
                    memset(ind_block, -1, BLOCK_SIZE);
                } else {  
                    for (int i = 0; i < superblock->num_disks; i++) {
                        ind_block = (struct hfs_ind_block *)(disks[i] + superblock->d_blocks_ptr + new_block * BLOCK_SIZE);
                        memset(ind_block, -1, BLOCK_SIZE);
                    }
                }
            }
            
            // Get ind block
            if (superblock->mode == 0) {
                int disk_index = inode->blocks[IND_BLOCK] % superblock->num_disks;
                off_t local_block_num = inode->blocks[IND_BLOCK] / superblock->num_disks;
                ind_block = (struct hfs_ind_block *)(disks[disk_index] + superblock->d_blocks_ptr + local_block_num * BLOCK_SIZE);
            } else {
                ind_block = (struct hfs_ind_block *)(disks[0] + superblock->d_blocks_ptr + inode->blocks[IND_BLOCK] * BLOCK_SIZE);
            }
            block_num_ptr = &ind_block->blocks[block_index - D_BLOCK];
        } else {
            return -EFBIG;
        }

        // Alloc if needed
        if (*block_num_ptr == -1) {
            int new_block = allocate_data_block();
            if (new_block < 0) {
                return -ENOSPC;
            }
            *block_num_ptr = new_block;
            
            if (superblock->mode == 0) {
                int disk_index = new_block % superblock->num_disks;
                off_t local_block_num = new_block / superblock->num_disks;
                memset(disks[disk_index] + superblock->d_blocks_ptr + local_block_num * BLOCK_SIZE, 0, BLOCK_SIZE);
            } else { 
                for (int i = 0; i < superblock->num_disks; i++) {
                    memset(disks[i] + superblock->d_blocks_ptr + new_block * BLOCK_SIZE, 0, BLOCK_SIZE);
                }
            }
        }

        // Calc size
        size_t block_bytes = BLOCK_SIZE - block_offset;
        if (block_bytes > size - bytes_written) {
            block_bytes = size - bytes_written;
        }

        // RAID write
        if (superblock->mode == 0) {
            int disk_index = *block_num_ptr % superblock->num_disks;
            off_t local_block_num = *block_num_ptr / superblock->num_disks;
            
            char *block_addr = disks[disk_index] + superblock->d_blocks_ptr + local_block_num * BLOCK_SIZE;
            memcpy(block_addr + block_offset, buf + bytes_written, block_bytes);
            
            if (ind_block) {
                ind_block->blocks[block_index - D_BLOCK] = *block_num_ptr;
            }
        } else {
            for (int i = 0; i < superblock->num_disks; i++) {
                char *block_addr = disks[i] + superblock->d_blocks_ptr + *block_num_ptr * BLOCK_SIZE;
                memcpy(block_addr + block_offset, buf + bytes_written, block_bytes);
                
                if (ind_block && i > 0) {
                    ind_block = (struct hfs_ind_block *)(disks[i] + superblock->d_blocks_ptr + inode->blocks[IND_BLOCK] * BLOCK_SIZE);
                    ind_block->blocks[block_index - D_BLOCK] = *block_num_ptr;
                }
            }
        }

        bytes_written += block_bytes;
    }

    // Update inode
    if (offset + bytes_written > inode->size) {
        inode->size = offset + bytes_written;
    }
    inode->mtim = time(NULL);

    for (int i = 0; i < superblock->num_disks; i++) {
        struct hfs_inode *mirror_inode = (struct hfs_inode *)(disks[i] + superblock->i_blocks_ptr + inode_idx * BLOCK_SIZE);
        memcpy(mirror_inode, inode, sizeof(struct hfs_inode));
    }

    return bytes_written;
}


static int hfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    printf("Entering hfs_readdir, path is: %s\n", path); 
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    int inode_idx = find_inode(path);
    if (inode_idx < 0) {
        fprintf(stderr, "Found invalid inode index\n");
        return -ENOENT;
    }
    struct hfs_inode *inode = get_inode(inode_idx);
    if (!inode) {
        fprintf(stderr, "Invalid inode index, cannot find inode\n");
        return -ENOENT;
    }

    if (!(inode->mode & S_IFDIR)) return -ENOTDIR;

    for (int offset = 0; offset < inode->size; offset += sizeof(struct hfs_dentry)) {
        struct hfs_dentry *entry = NULL;

        switch(superblock->mode) {
            case 0: {
                int disk_index = (inode->blocks[0] * BLOCK_SIZE + offset) % superblock->num_disks;
                entry = (struct hfs_dentry *)(disks[disk_index] + superblock->d_blocks_ptr + inode->blocks[0] * BLOCK_SIZE + offset);
                break;
            }
            case 1:
            case 2: {
                entry = (struct hfs_dentry *)(disks[0] + superblock->d_blocks_ptr + inode->blocks[0] * BLOCK_SIZE + offset);
                break;
            }
            default:
                return -EINVAL;
        }

        if (entry->name[0] == '\0') continue;

        if (filler(buf, entry->name, NULL, 0) != 0) break;
        
    }
    printf("Exiting readdir\n");
    return 0;
}

static struct fuse_operations ops = {
    .getattr = hfs_getattr,
    .mknod   = hfs_mknod,
    .mkdir   = hfs_mkdir,
    .unlink  = hfs_unlink,
    .rmdir   = hfs_rmdir,
    .read    = hfs_read,
    .write   = hfs_write,
    .readdir = hfs_readdir,
};

int main(int argc, char *argv[]) {
    num_disks = 0;
    while (num_disks + 1 < argc && access(argv[num_disks + 1], F_OK) == 0)
    {
        num_disks++;
    }
    
    if (num_disks < 1) {
        fprintf(stderr, "Need at least 1 disks\n");
        return FAIL;
    }

    disks = malloc(sizeof(void *) * num_disks);
    if (disks == NULL) {
        fprintf(stderr, "Memory allocation failed for disks\n");
        return FAIL;
    }

    fileDescs = malloc(sizeof(int) * num_disks);
    if (fileDescs == NULL) {
        fprintf(stderr, "Memory allocation failed for fileDescs\n");
        return FAIL;
    }

    for (int i = 0; i < num_disks; i++) {
        fileDescs[i] = open(argv[i + 1], O_RDWR);
        if (fileDescs[i] == -1) {
            fprintf(stderr, "Failed to open disk %s\n", argv[i + 1]);
            return FAIL;
        }

        struct stat st;
        if (fstat(fileDescs[i], &st) != 0) {
            fprintf(stderr, "Failed to get disk size for %s\n", argv[i + 1]);
            return FAIL;
        }
        diskSize = st.st_size;

        disks[i] = mmap(NULL, diskSize, PROT_READ | PROT_WRITE, MAP_SHARED, fileDescs[i], 0);
        if (disks[i] == MAP_FAILED) {
            fprintf(stderr, "Failed to mmap disk %s\n", argv[i + 1]);
            return FAIL;
        }
    }

    superblock = (struct hfs_sb *)disks[0];

    if (superblock == NULL) {
        fprintf(stderr, "Failed to access superblock\n");
        return FAIL;
    }

    num_disks = superblock->num_disks;

    int f_argc = argc - num_disks;
    char **f_argv = argv + num_disks;

    int rc = fuse_main(f_argc, f_argv, &ops, NULL);
    printf("Returned from fuse\n");

    for (int i = 0; i < num_disks; i++) {
        if (munmap(disks[i], diskSize) != 0) {
            fprintf(stderr, "Failed to unmap disk %d\n", i);
            return FAIL;
        }
        close(fileDescs[i]);
    }

    free(disks);
    free(fileDescs);

    return rc;
}
