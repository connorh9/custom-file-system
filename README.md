# Custom File System

This is a custom file system written fully in C. The file system is intended to support RAID modes 0, 1, and 1v. Currently, only RAID 0 and 1 work, and the implementation for 1v is in development. 
This file system is enabled by FUSE, which is a framework that allows the creation of file systems in standard programming languages. FUSE works by defining callback functions for FUSE to use as handlers.

## File System Implementation Details
The file system is modeled after common FFS (Fast File System) implementations. The file system uses a super block, inode bitmap and data bitmap as the metadata. We also have inodes and data blocks. The layout can be seen below.
![image](https://github.com/user-attachments/assets/27d7802a-1d71-4835-b5e3-e7a1ad70bdbd)
Each inode will contain a single indirect block in order to increase how much information that inode can store. Each inode will be of size 512 bytes, and every inode will also start at a location divisible by 512, this 
file system does not pack inodes close together. Every data block is also of size 512 bytes.

## Working the File System
The file system is split into two parts; mkfs.c and hfs.c. mkfs.c is the file system initialization and it works by being passed in a minimum of two disks, the raid mode, and the number of inodes and data blocks. Usage for it would look like
```
./mkfs -r 1 -d myDisk1 -d myDisk2 -i 64 -b 256
```
which would create a file system with 2 disks, 64 inodes and 256 data blocks.
To create a disk you can run the create_disk script.
You then should create a folder where you want to mount the file system via mkdir.
After running mkfs, you can then run hfs like so:
```
./hfs myDisk1 myDisk2 [options] [mount folder]
```
The options are intended for FUSE, this code assumes -s will be passed every time to disable multi-threading. -f can be passed to run the file system in the foreground, doing this would require you to open a second terminal to use the system.

## Supported features
Create empty files/directories  
Read/Write files up to max size of indirect block  
Read directory  
Remove an entry  
Get stats of a file/folder  
