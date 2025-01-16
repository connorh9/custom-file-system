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
#include "wfs.h"

int num_blocks;
int num_inodes;
int disks;
int raid_mode;
char* diskNames[256] = {NULL};


void init_filesystem(){
   // printf("started init");
    int* fileDescs = malloc(sizeof(int) * disks);
    //size_t total_blocks = num_blocks + num_inodes / BLOCK_SIZE + 1;

    void **diskMaps = malloc(disks * sizeof(void *));
    if (!diskMaps) {
        fprintf(stderr, "malloc for diskMaps");
        exit(-1);
    }
    
    off_t i_bitmap_offset = sizeof(struct wfs_sb);
    off_t d_bitmap_offset = i_bitmap_offset + (num_inodes + 7) / 8; 

    off_t i_blocks_start = (((d_bitmap_offset + (num_blocks + 7) / 8) + BLOCK_SIZE-1 ) / BLOCK_SIZE) * BLOCK_SIZE; 
    off_t d_blocks_start = i_blocks_start + (num_inodes * BLOCK_SIZE);
    
    struct wfs_sb superblock = {
        .num_data_blocks = num_blocks,
        .num_inodes = num_inodes,
        .d_bitmap_ptr = d_bitmap_offset,
        .i_bitmap_ptr = i_bitmap_offset,
        .d_blocks_ptr = d_blocks_start,
        .i_blocks_ptr = i_blocks_start,
        .mode = raid_mode,
        .num_disks = disks
    };

    int diskSize = i_blocks_start + (num_inodes * BLOCK_SIZE) + (num_blocks * BLOCK_SIZE);
    
    for (int i = 0; i < disks; i++) {
        // Update disk index
        superblock.disk_index = i;

        struct stat fStat;
        fileDescs[i] = open(diskNames[i], O_RDWR);
        if (fileDescs[i] < 0) {
            fprintf(stderr, "could not open file");
            for(int k = 0; k < i; k++){
                munmap(diskMaps[k], diskSize);
                close(fileDescs[k]);
            }
            free(fileDescs);
            free(diskMaps);
            exit(-1);
        }

        if (fstat(fileDescs[i], &fStat) < 0 || fStat.st_size < diskSize) {
            for (int j = 0; j < i; j++) {
                munmap(diskMaps[j], diskSize);
                close(fileDescs[j]);
            }
            close(fileDescs[i]);
            free(diskMaps);
            free(fileDescs);
            
            exit(-1);  // Runtime error - file too small or stat failed
        }

        diskMaps[i] = mmap(NULL, diskSize, PROT_READ | PROT_WRITE, MAP_SHARED, fileDescs[i], 0);
        if (diskMaps[i] == MAP_FAILED) {
            fprintf(stderr, "mmap");
            for(int k = 0; k < i; k++){
                munmap(diskMaps[k], diskSize);
                close(fileDescs[k]);
            }
            close(fileDescs[i]);
            free(diskMaps);
            free(fileDescs);
            
            exit(-1);
        }

        memset(diskMaps[i], 0, diskSize);
        memcpy(diskMaps[i], &superblock, sizeof(superblock));

        char *i_bitmap =(char *)diskMaps[i] + i_bitmap_offset;
        memset(i_bitmap, 0, (num_inodes + 7) / 8);
        i_bitmap[0] = 1;

        char *d_bitmap = (char *)diskMaps[i] + d_bitmap_offset;
        memset(d_bitmap, 0, (num_blocks + 7) / 8);

        struct wfs_inode root_inode = {0};
        root_inode.mode = S_IFDIR | 0777; 
        root_inode.uid = getuid();
        root_inode.gid = getgid();
        root_inode.num = 0;
        root_inode.nlinks = 2; 
        root_inode.atim = root_inode.mtim = root_inode.ctim = time(NULL);
        for (int i = 0; i < N_BLOCKS; i++) {
            root_inode.blocks[i] = -1;
        }

        memcpy((char*)diskMaps[i] + i_blocks_start, &root_inode, sizeof(root_inode));
        msync(diskMaps[i], diskSize, MS_SYNC);
    }


    for (int i = 0; i < disks; i++) {
        munmap(diskMaps[i], diskSize);
        close(fileDescs[i]);
    }
    free(diskMaps);
    free(fileDescs);
    return;
}
void parse(int argc, char* argv[]){
    int opt;
    while((opt = getopt(argc, argv, "r:d:i:b:")) != -1){
        switch(opt){
            case 'r':
                if(strcmp(optarg, "0") == 0){
                    raid_mode = 0;
                }
                else if(strcmp(optarg, "1") == 0){
                    raid_mode = 1;
                }
                else if(strcmp(optarg, "1v") == 0){
                    raid_mode = 2;
                }
                else{
                    exit(1);
                }
                break;
            case 'd':
                diskNames[disks] = optarg;
                disks++;
                break;
            case 'i':
                if (!optarg) {
                    fprintf(stdout, "Missing argument for -i.\n");
                    exit(1);
                }
                num_inodes = atoi(optarg);
                num_inodes = ((num_inodes + 31) / 32) * 32;
                break;
            case 'b':
                if (!optarg) {
                    fprintf(stdout, "Missing argument for -b.\n");
                    exit(1);
                }
                num_blocks = atoi(optarg);
                num_blocks = ((num_blocks + 31) / 32) * 32;
                break;
        }
    }
    if(disks < 2){
        fprintf(stderr, "Need 2 disks\n");
        exit(1);
    }
    if(num_inodes <= 0){
        fprintf(stderr, "no inodes\n");
        exit(1);
    } 
    if(num_blocks <= 0){
        fprintf(stderr, "no data blocks\n");
        exit(1);
    }

   // printf("%i, %i, %i", num_inodes, num_blocks, disks);
}

int main(int argc, char* argv[]){
    raid_mode = -1;
    num_blocks = -1;
    num_inodes = -1;
    disks = 0;

    parse(argc, argv);
    init_filesystem();
    return 0;
}
