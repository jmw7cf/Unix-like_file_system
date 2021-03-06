#include "dyn_array.h"
#include "bitmap.h"
#include "block_store.h"
#include "F17FS.h"
#include "string.h"
#include "libgen.h"
#include "math.h"

#define BLOCK_STORE_NUM_BLOCKS 65536   // 2^16 blocks.
#define BLOCK_STORE_AVAIL_BLOCKS 65520 // Last 2^16/2^3/2^9 = 16 blocks consumed by the FBM
#define BLOCK_SIZE_BYTES 512           // 2^9 BYTES per block
// direct each: 512, total size: 512 * 6 = 3072
// indirect, index block: 512/2 = 256 addresses, total size: 512 * 256 = 131072
// double indirect, first index block: 256 addresses, second index block total: 256*256 = 65536, total: 65536 * 512 = 33554432  
#define DIRECT_TOTAL_BYTES 3072	
#define SINGLE_INDIRECT_TOTAL_BYTES 131072
#define DOUBLE_INDIRECT_TOTAL_BYTES 33554432
#define DIRECT_BLOCKS 6	
#define INDIRECT_BLOCKS 256
#define DOUBLE_INDIRECT_BLOCKS 65536
#define MAX_FILE_SIZE 33688576

// each inode represents a regular file or a directory file
struct inode {
	uint8_t vacantFile;			// this parameter is only for directory, denotes which place in the array is empty and can hold a new file
	char owner[18];

	char fileType;				// 'r' denotes regular file, 'd' denotes directory file
	
	size_t inodeNumber;			// for F17FS, the range should be 0-255
	size_t fileSize; 			// the unit is in byte	
	size_t linkCount;
	
	// to realize the 16-bit addressing, pointers are acutally block numbers, rather than 'real' pointers.
	uint16_t directPointer[6];
	uint16_t indirectPointer;
	uint16_t doubleIndirectPointer;
		
};


struct fileDescriptor {
	uint8_t inodeNum;	// the inode # of the fd
	uint8_t usage; 		// only the lower 3 digits will be used. 1 for direct, 2 for indirect, 4 for dbindirect
	// locate_block and locate_offset together lcoate the exact byte
	uint16_t locate_order;		// the n-th block in the direct, indirect, or dbindirect pointer
	uint16_t locate_offset; // offset from the first byte (0 byte) in the data block
};


struct directoryFile {
	char filename[64];
	uint8_t inodeNumber;
};

struct directoryBlock {
	directoryFile_t dentries[7];
	char padding[57];
};

struct F17FS {
	block_store_t * BlockStore_whole;
	block_store_t * BlockStore_inode;
	block_store_t * BlockStore_fd;
};

// initialize directoryFile to 0 or "";
directoryFile_t init_dirFile(void){
	directoryFile_t df;
	memset(df.filename,'\0',64);
	df.inodeNumber = 0x00;
	return df;
}

// initialize directoryBlock 
directoryBlock_t init_dirBlock(void){
	directoryBlock_t db;
	int i=0;
	for(; i<7; i++){
		db.dentries[i] = init_dirFile();
	}
	memset(db.padding,'\0',57);
	return db;
}

/// Formats (and mounts) an F17FS file for use
/// \param fname The file to format
/// \return Mounted F17FS object, NULL on error
///
F17FS_t *fs_format(const char *path)
{
	if(path != NULL && strlen(path) != 0)
	{
		F17FS_t * ptr_F17FS = malloc(sizeof(F17FS_t));	// get started
		ptr_F17FS->BlockStore_whole = block_store_create(path);				// pointer to start of a large chunck of memory
		
		// reserve the 1st block for bitmaps (this block is cut half and half, for inode bitmap and fd bitmap)
		size_t bitmap_ID = block_store_allocate(ptr_F17FS->BlockStore_whole);
		//printf("bitmap_ID = %zu\n", bitmap_ID);
		// 2nd - 33th block for inodes, 32 blocks in total
		size_t inode_start_block = block_store_allocate(ptr_F17FS->BlockStore_whole);
		//printf("inode_start_block = %zu\n", inode_start_block);		
		for(int i = 0; i < 31; i++)
		{
			block_store_allocate(ptr_F17FS->BlockStore_whole);
		}
		
		// 34th block for root directory
		size_t root_data_ID = block_store_allocate(ptr_F17FS->BlockStore_whole);
		//printf("root_data_ID = %zu\n\n", root_data_ID);				
		// install inode block store inside the whole block store
		// install inode block store inside the whole block store
		ptr_F17FS->BlockStore_inode = block_store_inode_create(block_store_Data_location(ptr_F17FS->BlockStore_whole) + bitmap_ID * BLOCK_SIZE_BYTES, block_store_Data_location(ptr_F17FS->BlockStore_whole) + inode_start_block * BLOCK_SIZE_BYTES);

		// the first inode is reserved for root dir
		block_store_sub_allocate(ptr_F17FS->BlockStore_inode);
		
		// update the root inode info.
		uint8_t root_inode_ID = 0;	// root inode is the first one in the inode table
		inode_t * root_inode = (inode_t *) calloc(1, sizeof(inode_t));
		root_inode->vacantFile = 0x00;
		root_inode->fileType = 'd';
		root_inode->fileSize = BLOCK_SIZE_BYTES;								
		root_inode->inodeNumber = root_inode_ID;
		root_inode->linkCount = 1;
		root_inode->directPointer[0] = root_data_ID;
		block_store_inode_write(ptr_F17FS->BlockStore_inode, root_inode_ID, root_inode);
		directoryBlock_t rootDataBlock = init_dirBlock();		
		block_store_write(ptr_F17FS->BlockStore_whole, root_data_ID,&rootDataBlock );
		free(root_inode);
		
		// now allocate space for the file descriptors
		ptr_F17FS->BlockStore_fd = block_store_fd_create();

		return ptr_F17FS;
	}	
	return NULL;	
}




///
/// Mounts an F17FS object and prepares it for use
/// \param fname The file to mount

/// \return Mounted F17FS object, NULL on error

///
F17FS_t *fs_mount(const char *path)
{
	if(path != NULL && strlen(path) != 0)
	{
		F17FS_t * ptr_F17FS = (F17FS_t *)calloc(1, sizeof(F17FS_t));	// get started
		ptr_F17FS->BlockStore_whole = block_store_open(path);	// get the chunck of data	
		
		// the bitmap block should be the 1st one
		size_t bitmap_ID = 0;

		// the inode blocks start with the 2nd block, and goes around until the 33th block, 32 in total
		size_t inode_start_block = 1;
		
		// attach the bitmaps to their designated place
		ptr_F17FS->BlockStore_inode = block_store_inode_create(block_store_Data_location(ptr_F17FS->BlockStore_whole) + bitmap_ID * BLOCK_SIZE_BYTES, block_store_Data_location(ptr_F17FS->BlockStore_whole) + inode_start_block * BLOCK_SIZE_BYTES);
		
		// since file descriptors are alloacted outside of the whole blocks, we only can reallocate space for it.
		ptr_F17FS->BlockStore_fd = block_store_fd_create();
		
		return ptr_F17FS;
	}
	
	return NULL;		
}




///
/// Unmounts the given object and frees all related resources
/// \param fs The F17FS object to unmount
/// \return 0 on success, < 0 on failure
///
int fs_unmount(F17FS_t *fs)
{
	if(fs != NULL)
	{	
		block_store_inode_destroy(fs->BlockStore_inode);
		
		block_store_destroy(fs->BlockStore_whole);
		block_store_fd_destroy(fs->BlockStore_fd);
		
		free(fs);
		return 0;
	}
	return -1;
}

// search if the absolute path leading to the directory exists
// \param fs F17FS File system containing the file
// \param dirPath Absolute path of the directory
// return the inode number of the directory,  SIZE_MAX on error
size_t searchPath(F17FS_t *fs, char* dirPath){
	char *fn = strtok(dirPath,"/");
	// search and check if the directory name "fn" along the path are valid
	size_t iNum = 0; // inode number of the searched directory inode
	inode_t dirInode; // inode of the searched directory
	directoryBlock_t dirBlock; // file block of the searched directory
	while(fn != NULL){
		// find the inode
		if(0 == block_store_inode_read(fs->BlockStore_inode,iNum,&dirInode)){
			return SIZE_MAX;
		}
		// the inode must be of directory
		if(dirInode.fileType != 'd'){
			return SIZE_MAX;
		}
		// read the directory file block 	
		if(0 == block_store_read(fs->BlockStore_whole,dirInode.directPointer[0],&dirBlock)){
			return SIZE_MAX;
		}
		// search in the entries of the directory to see if the next directory name is found
		// use bitmap to jump over uninitialzied(unused) entries
		bitmap_t * dirBitmap = bitmap_overlay(8,&(dirInode.vacantFile));
		size_t j=0, found = 0;
		for(; j<7; j++){
			if(!bitmap_test(dirBitmap,j)){continue;}		
			if(strncmp(dirBlock.dentries[j].filename,fn,strlen(fn)) == 0 /* && (0 < dirBlock.dentries[j].inodeNumber)*/){
				inode_t nextInode; // inode whoes filename is fn
				// check if it is found and is dir  
				if((0 != block_store_inode_read(fs->BlockStore_inode,dirBlock.dentries[j].inodeNumber,&nextInode)) && (nextInode.fileType == 'd')){
					iNum = nextInode.inodeNumber;
					found = 1;
				}
			}
		}
		bitmap_destroy(dirBitmap);
		// if not found, exit on error
		if(found == 0){
			return SIZE_MAX;		
		}
		fn = strtok(NULL,"/");
	}
	return iNum;
} 

// check if the file already exists under the designated directory, if so, get the file's inode number
// \param fs F17FS containing the file
// \param dirInodeID The inode number of the directory
// \param filename Name of the file to look for
// return the file's inode number if the file is already created (exists), or 0 otherwise
size_t getFileInodeID(F17FS_t *fs, size_t dirInodeID, char *filename){
	// file aready exists?? use iNum as the inode number of the parent dir to search if this directory already contains the file/dir to be created
	inode_t parentInode; // inode for the parent directory of the destinated file/dir
	directoryBlock_t parentDir; // directory file block of the parent directory
	if((0==block_store_inode_read(fs->BlockStore_inode,dirInodeID,&parentInode)) || (0==block_store_read(fs->BlockStore_whole, parentInode.directPointer[0],&parentDir))){
		return 0;
	}
	// use bitmap to jump over uninitialized(unused) entries
	bitmap_t *parentBitmap = bitmap_overlay(8, &(parentInode.vacantFile)); 	
	int k=0;
	for(; k<7; k++){
		if(bitmap_test(parentBitmap,k)){
			if(0 == strncmp(parentDir.dentries[k].filename,filename,strlen(filename)) /* && 0 < parentDir.dentries[k].inodeNumber*/){
				// DO NOT worry about same name but different file type.
				// files can't have same name, regardless of file type.
				//inode_t tempInode;
				//if((0 != block_store_inode_read(fs->BlockStore_inode,parentDir.dentries[k].inodeNumber,&tempInode)) && (tempInode.fileType == fileType)){
		        	// printf("path: %s\nfilename already exists: %s\n",path,parentDir.dentries[k].filename);	
				bitmap_destroy(parentBitmap);
				return parentDir.dentries[k].inodeNumber;
				//}
			}
		}
	}
	bitmap_destroy(parentBitmap);
	return 0;
}	

///
/// Creates a new file at the specified location
///   Directories along the path that do not exist are not created
/// \param fs The F17FS containing the file
/// \param path Absolute path to file to create
/// \param type Type of file to create (regular/directory)
/// \return 0 on success, < 0 on failure
//
int fs_create(F17FS_t *fs, const char *path, file_t type){
	if(fs == NULL || path == NULL || (type != FS_REGULAR && type != FS_DIRECTORY) || strlen(path) <= 1){
		return -1;
	}
	// check if inode table is full
	if(block_store_get_used_blocks(fs->BlockStore_inode) >= 256){
		return -2;
	}
	// valid path must start with '/'
	char firstChar = *path;
	//printf("path: %s\nfirstChar: %c\n",path,firstChar);
	if(firstChar != '/'){return -3;}
	// path cannot end with "/"
	char lastChar = path[strlen(path)-1];
	//printf("path: %s\nlastChar: %c\n",path,lastChar);
	if(lastChar == '/'){return -4;}
	char dirc[strlen(path)+1]; // p is the non const copy of path
	char basec[strlen(path)+1];
	strcpy(dirc,path);
	strcpy(basec,path);
	char *dirPath = dirname(dirc);
	char *baseFileName = basename(basec);
	//printf("path: %s\ndirPath: %s\nbaseFileName: %s\n",path,dirPath,baseFileName);
	if(strlen(baseFileName)>=64){return -5;}

	// set fileType
	char fileType = 'r';
	if(type == FS_DIRECTORY){fileType = 'd';}

	// search and check if the directory name "fn" along the path are valid
	size_t iNum; // inode number of the searched directory inode
	iNum = searchPath(fs,dirPath);
	if(iNum == SIZE_MAX){return -6;}

	// file aready exists?? use iNum as the inode number of the parent dir to search if this directory already contains the file/dir to be created
	if(0!=getFileInodeID(fs,iNum,baseFileName)){ return -7;}		
	
	inode_t parentInode; // get the inode for the parent directory of the destinated file/dir
	directoryBlock_t parentDir; // get the directory file block of the parent directory
	if((0==block_store_inode_read(fs->BlockStore_inode,iNum,&parentInode)) || (0==block_store_read(fs->BlockStore_whole, parentInode.directPointer[0],&parentDir))){
		return -8;
	}

	// check if the dir block is full of entries using bitmap
	// only 7 entries are allowed in a directory, 0 - 6 bit, not including 7
	size_t available = 7;
	bitmap_t *bmp = bitmap_overlay(8,&(parentInode.vacantFile));
	available = bitmap_ffz(bmp);
	//printf("path: %s\ndirPath: %s\navailable: %lu\n\n",path,dirPath,available);
	if(available == SIZE_MAX || available == 7){
		bitmap_destroy(bmp);
		return -9;
	} 
	bitmap_set(bmp, available);
	bitmap_destroy(bmp);

	// allocate a new inode for the new file and get its inode number
	size_t newInodeID = block_store_sub_allocate(fs->BlockStore_inode);
	// create a new inode for the file
	inode_t newInode;
	newInode.fileType = fileType;
	if(fileType == 'd'){ // If create a directory
		newInode.vacantFile = 0x00;
		// allocate a block for the directory entries
		size_t directoryBlockPointer = block_store_allocate(fs->BlockStore_whole);
		if(SIZE_MAX == directoryBlockPointer){
			return -12;
		} 
		newInode.directPointer[0] = directoryBlockPointer;
		directoryBlock_t newDirBlock = init_dirBlock();
		block_store_write(fs->BlockStore_whole, newInode.directPointer[0],&newDirBlock);
		newInode.fileSize = BLOCK_SIZE_BYTES;	
	} else { // If to create a file
		newInode.fileSize = 0;
		int i=0;
		for(;i<6;i++){
			newInode.directPointer[i] = 0x0000;
		}	
		newInode.indirectPointer = 0x0000;
		newInode.doubleIndirectPointer = 0x0000;
		// Not need to allocate an empty data block for the file.
		// The get_data_block_id will take care of allocation of the data blocks when writing data to the file
		
	}
	newInode.inodeNumber = newInodeID;
	newInode.linkCount = 1;
	// write the created inode to the inode table
	block_store_inode_write(fs->BlockStore_inode,newInodeID,&newInode);
	
	// write it to the block store
	if(64 != block_store_inode_write(fs->BlockStore_inode, parentInode.inodeNumber, &parentInode)){
		return -10;
	}
	// add a new entry of filename and inode number to the parentDir 
	directoryFile_t df;
	strcpy(df.filename, baseFileName);
	df.inodeNumber = newInodeID; 
	//printf("baseFileName: %s inodeNumber: %d\n", baseFileName, newInodeID);
	parentDir.dentries[available] = df;
	if(0==block_store_write(fs->BlockStore_whole,parentInode.directPointer[0],&parentDir)){
		return -11;
	}
	
	// printf("dirPath: %s \n baseName: %s\n",dirPath, baseFileName);
	return 0;   
}

///
/// Opens the specified file for use
///   R/W position is set to the beginning of the file (BOF)
///   Directories cannot be opened
/// \param fs The F17FS containing the file
/// \param path path to the requested file
/// \return file descriptor to the requested file, < 0 on error
///
int fs_open(F17FS_t *fs, const char *path){
	if(fs == NULL || path == NULL || strlen(path) <= 1){
		return -1;
	}
	// valid path must start with '/'
	char firstChar = *path;
	//printf("path: %s\nfirstChar: %c\n",path,firstChar);
	if(firstChar != '/'){return -2;}
	// path cannot end with "/"
	char lastChar = path[strlen(path)-1];
	//printf("path: %s\nlastChar: %c\n",path,lastChar);
	if(lastChar == '/'){return -3;}

	char dirc[strlen(path)+1]; // p is the non const copy of path
	char basec[strlen(path)+1];
	strcpy(dirc,path);
	strcpy(basec,path);
	char *dirPath = dirname(dirc);
	char *baseFileName = basename(basec);
	//printf("path: %s\ndirPath: %s\nbaseFileName: %s\n",path,dirPath,baseFileName);
	if(strlen(baseFileName)>=64){return -4;}

	size_t dirInodeID = searchPath(fs,dirPath);
	if(dirInodeID == SIZE_MAX){return -5;} // No such path for the dir containing the requested file
	size_t fileInodeID = getFileInodeID(fs,dirInodeID,baseFileName);
	if(fileInodeID == 0){return -6;} // No such file is found
	inode_t fileInode;
	if(0 == block_store_inode_read(fs->BlockStore_inode,fileInodeID,&fileInode)){return -7;} // get the inode object of the file
	if('d'==fileInode.fileType){return -8;} // file can't be directory
	size_t fd = block_store_sub_allocate(fs->BlockStore_fd); // file descriptor ID
	fileDescriptor_t fd_t;
	fd_t.inodeNum = fileInodeID;	
	fd_t.usage = 1;
	fd_t.locate_order = 0;
	fd_t.locate_offset = 0;
	if(0 == block_store_fd_write(fs->BlockStore_fd,fd,&fd_t)){return -9;}			
	return fd;
}

///
/// Closes the given file descriptor
/// \param fs The F17FS containing the file
/// \param fd The file to close
/// \return 0 on success, < 0 on failure
///
int fs_close(F17FS_t *fs, int fd){
	if(fs == NULL || fd < 0){
		return -1;
	}
	if(!block_store_sub_test(fs->BlockStore_fd,fd)){return -2;} // error if fd is not allocated
	block_store_sub_release(fs->BlockStore_fd,fd); 
	return 0;
}

///
/// Populates a dyn_array with information about the files in a directory
///   Array contains up to 15 file_record_t structures
/// \param fs The F17FS containing the file
/// \param path Absolute path to the directory to inspect
/// \return dyn_array of file records, NULL on error
///
dyn_array_t *fs_get_dir(F17FS_t *fs, const char *path){
	if(fs == NULL || path == NULL || strlen(path) < 1){
		return NULL;
	}

	// validate the pathname
	// valid path must start with '/'
	char firstChar = *path;
	if(firstChar != '/'){
		return NULL;}
	// parse the pathname
	char dirc[strlen(path)+1]; 
	char basec[strlen(path)+1];
	strcpy(dirc,path);
	strcpy(basec,path);
	char *dirPath = dirname(dirc);
	char *baseFileName = basename(basec);


	size_t dirInodeID;
	// if the directory is the root
	if(strlen(path)==1 && 0==strncmp(path,"/",1)){
		dirInodeID = 0; // set the inode number to 0 for root
	} else{ // other wise, trace down from the root to look for the inode
		// get the inode number of the target directory
		size_t parentInodeID;
		if((parentInodeID=searchPath(fs,dirPath)) == SIZE_MAX) {return NULL;}
		dirInodeID = getFileInodeID(fs,parentInodeID,baseFileName);
		if(dirInodeID == 0){return NULL;} // No such file is found, if it is not root, the inode number cannot be 0
	}
	// get the inode block and data block of the directory
	inode_t dirInode;
	directoryBlock_t dirBlock;
	if((0 == block_store_inode_read(fs->BlockStore_inode, dirInodeID, &dirInode)) || (0 == block_store_read(fs->BlockStore_whole,dirInode.directPointer[0],&dirBlock))){ return NULL;}	
	if('d'!=dirInode.fileType){return NULL;} // Should be directory
	
	// create a dynamic array, data object size is sizeof(file_record_t)
	dyn_array_t *list = dyn_array_create(15,sizeof(file_record_t),NULL);
	if(list == NULL){
		return NULL;
	}
	// loop through all the allocated entries in the data block in form of directoryBlock_t structure
	// use bitmap to skip unused/uninitialized entires
	bitmap_t * bmp = bitmap_overlay(8,&(dirInode.vacantFile));
	int k = 0;
	for(;k<7;k++){
		if(bitmap_test(bmp,k)){
			// add the entry name to the array 
			file_record_t record;
			strncpy(record.name,dirBlock.dentries[k].filename,FS_FNAME_MAX);
			inode_t fileInode;
			if(0==block_store_inode_read(fs->BlockStore_inode,dirBlock.dentries[k].inodeNumber,&fileInode)){
				bitmap_destroy(bmp);
				dyn_array_destroy(list);
				return NULL;
			}else {
				if(fileInode.fileType == 'r'){
					record.type = FS_REGULAR;
				} else {
					record.type = FS_DIRECTORY;
				}
				
			}
			if(!dyn_array_push_back(list,&record)){
				bitmap_destroy(bmp);
				dyn_array_destroy(list);
				return NULL;
			}
			//printf("record name: %s\n",record.name);	
		}	
	}
	bitmap_destroy(bmp);	
	return list;
}

// allocate and get the data block id
// \param fs The F17FS Filesystem
// \param fd_t The fileDescriptor object
// \param fileInode Inode of the file
// return the data block id, or 0 on error
uint16_t get_data_block_id(F17FS_t *fs, fileDescriptor_t *fd_t){
	if(fs==NULL || fd_t==NULL){
		//printf("fs or fd_t == NULL\n");
		return 0;
	} else {
		inode_t ino;
		if(0==block_store_inode_read(fs->BlockStore_inode,fd_t->inodeNum,&ino)){
			//printf("Cannot read inode\n");
			return 0;
		} else {
			size_t order = fd_t->locate_order; 
	
			if(fd_t->usage == 1){ // the block to be used is pointed by directPointer
				if(0x0000 == ino.directPointer[order]){ // if the block hasnt been allocated
					if(1<=block_store_get_free_blocks(fs->BlockStore_whole)){
						ino.directPointer[order] = block_store_allocate(fs->BlockStore_whole);
						if(block_store_inode_write(fs->BlockStore_inode,fd_t->inodeNum,&ino)){
							//printf("new, usage:%d, order:%lu,offset:%d\n",fd_t->usage,order,fd_t->locate_offset);
							return ino.directPointer[order];
						}
					}
				} else if(0x0000 != ino.directPointer[order] && block_store_test(fs->BlockStore_whole,ino.directPointer[order])){
					//printf("existed\n");
					return ino.directPointer[order]; // return the pointer, i.e., the address of the data block to write
				}
				//printf("error,order:%lu,offset:%d,direct addr:%d\n",order,fd_t->locate_offset,ino.directPointer[order]);
				return 0;
			} else if(fd_t->usage == 2){ // the block is pointed by indirectPointer
				uint16_t table[256]; // the index table of the indirectPointer
				memset(table,0x0000,2*256);
				if(0 == order && 0x0000 == ino.indirectPointer){ // the block hasnt been allocated
					// the block is the first indirectPointer pointed block
					// allocate a block for the index table pointed by the indirectPointer in the inode 
					if(2<=block_store_get_free_blocks(fs->BlockStore_whole)){
						ino.indirectPointer = block_store_allocate(fs->BlockStore_whole);
						table[0] = block_store_allocate(fs->BlockStore_whole); // allocate the data block pointed by an entry in the index table
						if(0!=block_store_write(fs->BlockStore_whole,ino.indirectPointer,table) && 0!=block_store_inode_write(fs->BlockStore_inode,fd_t->inodeNum,&ino)){
							return table[0]; 
						}
					} 
				} else if(0x0000 != ino.indirectPointer && block_store_test(fs->BlockStore_whole,ino.indirectPointer)){
				// when indirectPointer is alread allocated: (1) the the data block is not allocated (2) data block is allocated	
					if(block_store_read(fs->BlockStore_whole,ino.indirectPointer,table)){
						if(0x0000 == table[order]){
							if(1<=block_store_get_free_blocks(fs->BlockStore_whole)){
								table[order] = block_store_allocate(fs->BlockStore_whole);
								if(block_store_write(fs->BlockStore_whole,ino.indirectPointer,table)){
									return table[order];
								}
							}
						} else if(0x0000 != table[order] && block_store_test(fs->BlockStore_whole,table[order])){
							return table[order];
						}
					}
					//printf("indirectPointer set but cannot access\n");
				}
				//printf("indirectPointer %d\n",ino.indirectPointer);
				return 0;	
			} else { // the block is pointed by a doubleIndiretPointer
				uint16_t outerIndexTable[256];
				memset(outerIndexTable,0x0000,2*256);
				uint16_t innerIndexTable[256];
				memset(innerIndexTable,0x0000,2*256);
				//printf("order: %lu\n",order);
				if(0x0000 == ino.doubleIndirectPointer){ //the block hasnt been allocated yet
					//printf("outerIndexTable index: %lu,usedBlockCount: %lu\n",order/256,usedBlockCount);
					if(3<=block_store_get_free_blocks(fs->BlockStore_whole)){
						ino.doubleIndirectPointer = block_store_allocate(fs->BlockStore_whole);
						outerIndexTable[0] = block_store_allocate(fs->BlockStore_whole);
						innerIndexTable[0] = block_store_allocate(fs->BlockStore_whole);
						if(block_store_write(fs->BlockStore_whole,ino.doubleIndirectPointer,outerIndexTable) &&	
						   block_store_write(fs->BlockStore_whole,outerIndexTable[0],innerIndexTable) &&
						   block_store_inode_write(fs->BlockStore_inode,fd_t->inodeNum,&ino)){
							//printf("dbIndirect addr: %d, order: %lu\n",innerIndexTable[0],order);
							return innerIndexTable[0];
						}
					} 	
				} else if(0x0000 != ino.doubleIndirectPointer && block_store_test(fs->BlockStore_whole,ino.doubleIndirectPointer)){
					if(block_store_read(fs->BlockStore_whole,ino.doubleIndirectPointer,outerIndexTable)){
						if(0x0000 == outerIndexTable[order/256]){
						// when the new block is the first entry of a new innerIndexTable
							if(2<=block_store_get_free_blocks(fs->BlockStore_whole)){
							//printf("outerIndexTable index: %lu,usedBlockCount: %lu\n",order/256,usedBlockCount);
								outerIndexTable[order/256] = block_store_allocate(fs->BlockStore_whole);
								innerIndexTable[order%256] = block_store_allocate(fs->BlockStore_whole);
								if(0!=block_store_write(fs->BlockStore_whole,ino.doubleIndirectPointer,outerIndexTable) && 0!=block_store_write(fs->BlockStore_whole,outerIndexTable[order/256],innerIndexTable)){
									//printf("? dbIndirect addr: %d, order: %lu\n",innerIndexTable[order%256],order);
 									return innerIndexTable[order%256];
								}
							} 	
						} else if(0x0000 != outerIndexTable[order/256] && block_store_test(fs->BlockStore_whole,outerIndexTable[order/256])){
							if(block_store_read(fs->BlockStore_whole,outerIndexTable[order/256],innerIndexTable)){
								//printf("innerIndex %lu addr: %d\n",order%256,innerIndexTable[order%256]);
								if(0x0000 == innerIndexTable[order%256]){
									if(1<=block_store_get_free_blocks(fs->BlockStore_whole)){
										innerIndexTable[order%256] = block_store_allocate(fs->BlockStore_whole);
										if(block_store_write(fs->BlockStore_whole,outerIndexTable[order/256],innerIndexTable)){
											//printf("dbIndirect addr: %d, order: %lu\n",innerIndexTable[order%256],order);
											return innerIndexTable[order%256];
										}
									}	
								} else if(0x0000 != innerIndexTable[order%256] && block_store_test(fs->BlockStore_whole,innerIndexTable[order%256])){
									//printf("dbIndirect addr: %d, order: %lu\n",innerIndexTable[order%256],order);
									return innerIndexTable[order%256];
								}	
								//printf("Something happened to doubleIndirect 5");
							}
							//printf("Something happened to doubleIndirect 4");
						}
						//printf("Something happened to doubleIndirect 3");
					}
					//printf("Something happened to doubleIndirect 2");
				}
				//printf("Something happened to doubleIndirect");
				return 0;
			}
			//printf("Something happened to doubleIndirect -1");
		}		
		//printf("Something happened to doubleIndirect -2\n");
	} 
	//printf("Something happened to doubleIndirect -3\n");
}

//
// calculate the file size up until the location pointed by fileDescriptor usage, order and offset
// return size of the file
size_t getFileSize(fileDescriptor_t *fd_t){
		if(fd_t->usage == 1){
			return 512 * fd_t->locate_order + fd_t->locate_offset;
		} else if(fd_t->usage == 2){
			return 512 * (6 + fd_t->locate_order) + fd_t->locate_offset;	
		} else {
			return 512 * (6 + 256 + fd_t->locate_order) + fd_t->locate_offset;
		}
} 


/// Writes data from given buffer to the file linked to the descriptor
///   Writing past EOF extends the file
///   Writing inside a file overwrites existing data
///   R/W position is incremented by the number of bytes written
/// \param fs The F17FS containing the file/// \param fd The file to write to
/// \param dst The buffer to read from
/// \param nbyte The number of bytes to write
/// \return number of bytes written (< nbyte IF out of space), < 0 on error
///
ssize_t fs_write(F17FS_t *fs, int fd, const void *src, size_t nbyte){
	// check if fs,fd,src are valid
	if(fs==NULL || fd < 0 || !block_store_sub_test(fs->BlockStore_fd,fd) || src == NULL){
		return -1;
	} else {
		// if 0 byte is needed to write
		if(nbyte==0){
			return 0;
		} else {
			// get fd's corresponding fileDescriptor structure 
			// get inode number, usage, order and offset
			fileDescriptor_t fd_t;
			if(0==block_store_fd_read(fs->BlockStore_fd,fd,&fd_t)){
				return -2;
			} else {
				//printf("Free blocks: %lu\n",block_store_get_free_blocks(fs->BlockStore_whole));
				size_t locSize = getFileSize(&fd_t); 
				size_t writtenBytes = 0;
				//printf("start writing:\n");
				// write data to the first block starting where the current fd pointer is at	
				while(nbyte - writtenBytes > 0){
					uint16_t blockID = get_data_block_id(fs,&fd_t); 
					//printf("Free blocks: %lu\n",block_store_get_free_blocks(fs->BlockStore_whole));
					//printf("blockID: %d\n",blockID);
					if(0 < blockID){
						if(fd_t.locate_offset + nbyte - writtenBytes < BLOCK_SIZE_BYTES){ // the last block to write 
							if(0!=block_store_n_write(fs->BlockStore_whole,blockID,fd_t.locate_offset,src+writtenBytes,nbyte-writtenBytes)){
								fd_t.locate_offset += (nbyte - writtenBytes); // update locate_offset
								writtenBytes = nbyte;		
								break; // finish writing
							}
							//printf("blockID: %d,offset: %d, nbyte: %lu, writtenBytes: %lu\n",blockID,fd_t.locate_offset,nbyte,writtenBytes); 
							return -5;
						} else { 
							if(0!=block_store_n_write(fs->BlockStore_whole,blockID,fd_t.locate_offset,src+writtenBytes,BLOCK_SIZE_BYTES-fd_t.locate_offset)){
								writtenBytes += (BLOCK_SIZE_BYTES - fd_t.locate_offset);
								fd_t.locate_offset = 0;//update locate_offset
								// update locate_order
								if(fd_t.usage==1&&fd_t.locate_order== (DIRECT_BLOCKS-1)){ // directPointer space is full
									fd_t.locate_order = 0;
									fd_t.usage = 2;
									//printf("dip into indirect blocks\n");
								} else if(fd_t.usage==2&&fd_t.locate_order== (INDIRECT_BLOCKS-1)){ // indirectPointer space is full
									fd_t.locate_order = 0;
									fd_t.usage = 4;
									//printf("dip into double indirect blocks\n");
								} else { // as long as not exceeding the double inidirect max, unlikely
									fd_t.locate_order += 1;
								}
								/*if(fd_t.locate_order >= 2559){
									printf("blockID: %d,offset: %d, nbyte: %lu, writtenBytes: %lu\n",blockID,fd_t.locate_offset,nbyte,writtenBytes); 
								}*/
								continue;
							} 
							//printf("offset: %d, bytes: %d\n",fd_t.locate_offset,512-fd_t.locate_offset);
							//printf("freeBlocks: %lu\n",block_store_get_free_blocks(fs->BlockStore_whole));
							//printf("blockID: %d,offset: %d, nbyte: %lu, writtenBytes: %lu\n",blockID,fd_t.locate_offset,nbyte,writtenBytes); 
							//printf("Used blocks: %lu\n",block_store_get_used_blocks(fs->BlockStore_whole));
							return -6; 
						}
					} else {
					//printf("error on getting block id: %d,order:%d\n",blockID,fd_t.locate_order);
					break;
					}	
				} 
				inode_t fileInode;
				block_store_inode_read(fs->BlockStore_inode,fd_t.inodeNum,&fileInode);
				if(fileInode.fileSize < locSize + writtenBytes){ // Need to recalculate
					fileInode.fileSize = locSize + writtenBytes;
				}
				if(0!=block_store_fd_write(fs->BlockStore_fd,fd,&fd_t) && 0!=block_store_inode_write(fs->BlockStore_inode,fd_t.inodeNum,&fileInode)){
					//printf("Finish writing: %lu\n",writtenBytes);
					return writtenBytes;
				} else {return -8;}							
	
			}
		}
	}
}

//
// Deletes the specified file and closes all open descriptors to the file
//   Directories can only be removed when empty
// \param fs The F17FS containing the file
// \param path Absolute path to file to remove
// \return 0 on success, < 0 on error
//
int fs_remove(F17FS_t *fs, const char *path) {
	if(fs == NULL || path == NULL || strlen(path) == 0){
		return -1;
	}
	// Validate the path
	char firstChar = *path;
	if(firstChar != '/'){return -2;}
	char dirc[strlen(path)+1];
	char basec[strlen(path)+1];
	strcpy(dirc,path);
	strcpy(basec,path);
	char *dirPath =  dirname(dirc);
	char *baseFileName = basename(basec);

	size_t dirInodeID = searchPath(fs,dirPath);
	size_t fileInodeID;
	if(dirInodeID != SIZE_MAX){
		if(0==strcmp(baseFileName,"/") && 0==strcmp(dirPath,"/") ){// Cannot remove root directory
			return -4;
		} else { // If not root, need to validate the file's inode ID
			fileInodeID = getFileInodeID(fs,dirInodeID,baseFileName);
			if(fileInodeID == 0){
				//printf("dirInodeID: %lu\n",dirInodeID);
				//printf("invalid fileInodeID %lu for dir: %s file: %s\n",fileInodeID,dirPath,baseFileName);
				return -4;
			}
		}
		inode_t fileInode;
		block_store_inode_read(fs->BlockStore_inode,fileInodeID,&fileInode);
		if(fileInode.fileType=='d'){
		// If the file is a dir, delete it only if it is empty, meaning vacantFile == 0x00
			if(fileInode.vacantFile == 0x00 || fileInode.linkCount > 1){
				// To delete a dir, remove its entry from the directory block of its parent inode, or the number of hardlinks > 1
				inode_t dirInode; // Parent directoy inode
				block_store_inode_read(fs->BlockStore_inode,dirInodeID,&dirInode);
				//printf("Dir %s before clear file,vacantFile: %d\n",path,dirInode.vacantFile); 
				bitmap_t *bmp = bitmap_overlay(8, &(dirInode.vacantFile));
				directoryBlock_t db_t;
				block_store_read(fs->BlockStore_whole,dirInode.directPointer[0],&db_t); 	
				int m=0;
				for(;m<7; m++){
					if(bitmap_test(bmp,m)){
						if(0 == strncmp(db_t.dentries[m].filename,baseFileName,FS_FNAME_MAX) /* && 0 < parentDir.dentries[k].inodeNumber*/){
							memset(db_t.dentries[m].filename,'\0',FS_FNAME_MAX);
							db_t.dentries[m].inodeNumber = 0x00;
							bitmap_reset(bmp,m);	
							break;
						}
					}	
				}
				//printf("Dir %s after clear file,vacantFile: %d\n",path,dirInode.vacantFile); 
				bitmap_destroy(bmp);
				block_store_inode_write(fs->BlockStore_inode,dirInodeID,&dirInode);
				block_store_write(fs->BlockStore_whole,dirInode.directPointer[0],&db_t);
				// If the directory file inode is not hardlinked to any other file
				if(fileInode.linkCount <= 1) {
					// Remove its file block pointed by directPointer[0], then remove its inode from inode table
					block_store_release(fs->BlockStore_whole,fileInode.directPointer[0]);
					block_store_sub_release(fs->BlockStore_inode,fileInodeID);
					//printf("Success Dir %s not empty,vacantFile: %d\n",path,dirInode.vacantFile); 
					return 0;
				} else {
					fileInode.linkCount -= 1;
					if(block_store_inode_write(fs->BlockStore_inode,fileInodeID,&fileInode)){
						//printf("Success Dir %s not empty,vacantFile: %d\n",path,dirInode.vacantFile); 
						return 0;
					}
				}
				return -8;
			}
			//printf("ErrorDir %s not empty,vacantFile: %d\n",path,fileInode.vacantFile); 
			return -5;
		} else if(fileInode.fileType=='r') { // if the file to remove is file
			if(fileInode.linkCount > 1){ 
				// If the file inode is referenced by more than one link, then just decrement the linkCount by 1 and update the inode 
				fileInode.linkCount -= 1;
				// Update the file inode
				if(0 == block_store_inode_write(fs->BlockStore_inode,fileInodeID,&fileInode)){
					return -11;
				}	
			} else { // If the file inode is only referened by one link, delete the content of the inode
				int i=0;
				for(; i<6; i++){ // if the directPointer is allocated, release those memory addresses first
					if(0x0000 != fileInode.directPointer[i] && block_store_test(fs->BlockStore_whole,fileInode.directPointer[i])){
						block_store_release(fs->BlockStore_whole,fileInode.directPointer[i]);	
					}	
				}
				if(0x0000 != fileInode.indirectPointer && block_store_test(fs->BlockStore_whole,fileInode.indirectPointer)){// if the indirectPointer is allocated, 
					uint16_t indexTable[256];
					memset(indexTable,0x0000,512);
					if(block_store_read(fs->BlockStore_whole,fileInode.indirectPointer,indexTable)){
						int j=0;
						for(; j<256; j++){ // release all the secondary memory addresses 
							if(0x0000 != indexTable[j] && block_store_test(fs->BlockStore_whole,indexTable[j])){
								block_store_release(fs->BlockStore_whole,indexTable[j]);	
							}	
						}
					} else {return -11;}
					block_store_release(fs->BlockStore_whole,fileInode.indirectPointer);	
				}
				if(0x0000 != fileInode.doubleIndirectPointer && block_store_test(fs->BlockStore_whole,fileInode.doubleIndirectPointer)){// if the doubleIndirectPointer is allocated, 
					uint16_t outerIndexTable[256];
					memset(outerIndexTable,0x0000,512);
					uint16_t innerIndexTable[256];
					memset(innerIndexTable,0x0000,512);
					if(block_store_read(fs->BlockStore_whole,fileInode.doubleIndirectPointer,outerIndexTable)){
						int j=0;
						for(; j<256; j++){ // release all the secondary memory addresses 
							if(outerIndexTable[j]!=0x0000 && block_store_test(fs->BlockStore_whole,outerIndexTable[j])){			
								if(block_store_read(fs->BlockStore_whole,outerIndexTable[j],innerIndexTable)){
									int k=0;
									for(; k<256; k++){ // release all the secondary memory addresses 
										if(innerIndexTable[k]!=0x0000 && block_store_test(fs->BlockStore_whole,innerIndexTable[k])){
											block_store_release(fs->BlockStore_whole,innerIndexTable[k]);	
										} 
									}
								} else {return -10;}
								block_store_release(fs->BlockStore_whole,outerIndexTable[j]);	
							} 	
						}
					} else {return -9;}
					block_store_release(fs->BlockStore_whole,fileInode.doubleIndirectPointer);	
				}
				int fd_count=0;
				for(;fd_count<256;fd_count++){ // close all fd pointing to the file
					if(block_store_sub_test(fs->BlockStore_fd,fd_count)){
						fileDescriptor_t fd_t;
						if(block_store_fd_read(fs->BlockStore_fd,fd_count,&fd_t)){
							if(fd_t.inodeNum==fileInodeID){
								fs_close(fs,fd_count);
							}
						}
						return -7;
					}
				}
				block_store_sub_release(fs->BlockStore_inode,fileInodeID);
			}
			// Remove the file entry in the parent directory data block by reducing one bit from vacantFile
			inode_t dirInode; // The inode for the parent directory containing the file
			block_store_inode_read(fs->BlockStore_inode,dirInodeID,&dirInode);
			//dirInode.vacantFile >>= 1 ; // Cannot do this, would mess up the directory file block
			//printf("Dir %s before clear file,vacantFile: %d\n",path,dirInode.vacantFile); 
			bitmap_t *bmp = bitmap_overlay(8, &(dirInode.vacantFile));
			directoryBlock_t db_t;
			block_store_read(fs->BlockStore_whole,dirInode.directPointer[0],&db_t); 	
			int m=0;
			for(;m<7; m++){
				if(bitmap_test(bmp,m)){
					if(0 == strncmp(db_t.dentries[m].filename,baseFileName,FS_FNAME_MAX) /* && 0 < parentDir.dentries[k].inodeNumber*/){
						bitmap_reset(bmp,m);	
						strncmp(db_t.dentries[m].filename,"\0",FS_FNAME_MAX);
						db_t.dentries[m].inodeNumber = 0x0000;
						break;
					}
				}
			}
			//printf("Dir %s after clear file,vacantFile: %d\n",path,dirInode.vacantFile); 
			bitmap_destroy(bmp);
			if(block_store_write(fs->BlockStore_whole,dirInode.directPointer[0],&db_t) &&	block_store_inode_write(fs->BlockStore_inode,dirInodeID,&dirInode)) {
				//printf("Success Dir %s not empty,vacantFile: %d\n",path,dirInode.vacantFile); 
				return 0;
			}
			return -12;
		}
		return -6;	
	}
	return -3;
	// To delete a file, you need to search all the data blocks allocated to it, including direct, indirect and dbindirect blocks.
}

///
/// Moves the R/W position of the given descriptor to the given location
///   Files cannot be seeked past EOF or before BOF (beginning of file or offset==0)
///   Seeking past EOF will seek to EOF, seeking before BOF will seek to BOF
/// \param fs The F17FS containing the file
/// \param fd The descriptor to seek
/// \param offset Desired offset relative to whence
/// \param whence Position from which offset is applied
/// \return offset from BOF, < 0 on error
///
off_t fs_seek(F17FS_t *fs, int fd, off_t offset, seek_t whence){
	if(fs !=NULL && fd >= 0 && block_store_sub_test(fs->BlockStore_fd,fd)){
		fileDescriptor_t fd_t;
		inode_t fileInode;
		if(0 !=block_store_fd_read(fs->BlockStore_fd,fd,&fd_t) && 0 != block_store_inode_read(fs->BlockStore_inode,fd_t.inodeNum,&fileInode)){
			ssize_t currentOffset = getFileSize(&fd_t);
			ssize_t fileSize = fileInode.fileSize;
			off_t stdOffset; // Standardized offset, starting from BOF
			// Standardize the offset against the BOF from the three cases: FS_SEEK_SET, FS_SEEK_CUR, and FS_SEEK_END
			if(whence == FS_SEEK_SET){
				if(offset<0){
					stdOffset = 0;
				} else if(offset > fileSize){
					stdOffset = fileSize;
				} else {
					stdOffset = offset;
				}
			} else if(whence == FS_SEEK_CUR){
				if(0 > currentOffset + offset){
					stdOffset = 0;	
				} else if(fileSize < currentOffset + offset){
					stdOffset = fileSize;
				} else {
					stdOffset = currentOffset + offset;
				}
			} else if(whence == FS_SEEK_END){
				if(offset>0){
					stdOffset = fileSize;
				} else if(offset + fileSize < 0){
					stdOffset = 0;
				} else {
					stdOffset = offset + fileSize;
				}
			} else {
				return -4;
			}	
			// Calculate the relative offset, order, and usage	
			if((stdOffset/BLOCK_SIZE_BYTES) >= DIRECT_BLOCKS + INDIRECT_BLOCKS) {
				fd_t.usage = 4;
				fd_t.locate_offset = stdOffset % BLOCK_SIZE_BYTES;
				fd_t.locate_order = (stdOffset/BLOCK_SIZE_BYTES) - (DIRECT_BLOCKS + INDIRECT_BLOCKS);
			} else if (ceil(1.0*stdOffset/BLOCK_SIZE_BYTES) >= DIRECT_BLOCKS){
				fd_t.usage = 2;
				fd_t.locate_offset = stdOffset % BLOCK_SIZE_BYTES;
				fd_t.locate_order = (stdOffset/BLOCK_SIZE_BYTES) - DIRECT_BLOCKS; 
			} else {
				fd_t.usage = 1;
                		fd_t.locate_offset = stdOffset % BLOCK_SIZE_BYTES;
                		fd_t.locate_order = stdOffset/BLOCK_SIZE_BYTES;
			}
			if(0 != block_store_fd_write(fs->BlockStore_fd,fd,&fd_t)){
				return stdOffset;
			} 
			return -3;
		}
		return -2;	
	} 
	return -1;
}

///
/// Reads data from the file linked to the given descriptor
///   Reading past EOF returns data up to EOF
///   R/W position in incremented by the number of bytes read
/// \param fs The F17FS containing the file
/// \param fd The file to read from
/// \param dst The buffer to write to
/// \param nbyte The number of bytes to read
/// \return number of bytes read (< nbyte IFF read passes EOF), < 0 on error
//
ssize_t fs_read(F17FS_t *fs, int fd, void *dst, size_t nbyte){
	// check if fs,fd,src are valid
	if(fs !=NULL && fd >= 0 && block_store_sub_test(fs->BlockStore_fd,fd) && dst != NULL){
		if(nbyte != 0){
			// Open the fileDescriptor and file inode
			fileDescriptor_t fd_t;
			inode_t fileInode;
			if(0 != block_store_fd_read(fs->BlockStore_fd,fd,&fd_t) && 0 != block_store_inode_read(fs->BlockStore_inode,fd_t.inodeNum,&fileInode)){
				// Get the current offset and the file size
				size_t fileSize =  fileInode.fileSize;
				size_t currentOffset = getFileSize(&fd_t);
				// Calculate the maximum of bytes it can read (fileSize - current offset)
				size_t leftBytes = fileSize - currentOffset;
				// If the maximum of bytes to read is smaller than nbyte, then set nbyte to  maximum of bytes 
				if(nbyte > leftBytes){
					nbyte =  leftBytes;
				}
				size_t readBytes = 0;
				while(nbyte - readBytes > 0){
					uint16_t blockID = get_data_block_id(fs,&fd_t); // Get the data block to read	
					if(blockID > 0){
						if(fd_t.locate_offset + nbyte - readBytes < BLOCK_SIZE_BYTES){
							// Need a special read method here to write less than BLOCK_SIZE_BYTES
							if(0 != block_store_n_read(fs->BlockStore_whole,blockID,fd_t.locate_offset,dst+readBytes,nbyte-readBytes)){
								fd_t.locate_offset = fd_t.locate_offset + nbyte - readBytes;
								readBytes = nbyte;
								continue;
							}
							return -4;
						} else {
							if(0 != block_store_n_read(fs->BlockStore_whole,blockID,fd_t.locate_offset,dst+readBytes,BLOCK_SIZE_BYTES-fd_t.locate_offset)){
								readBytes += (BLOCK_SIZE_BYTES-fd_t.locate_offset);
								// increment usage and locate_order
								if(fd_t.usage==1&&fd_t.locate_order== (DIRECT_BLOCKS-1)){ // directPointer space is full
									fd_t.locate_order = 0;
									fd_t.usage = 2;
									//printf("dip into indirect blocks\n");
								} else if(fd_t.usage==2&&fd_t.locate_order== (INDIRECT_BLOCKS-1)){ // indirectPointer space is full
									fd_t.locate_order = 0;
									fd_t.usage = 4;
									//printf("dip into double indirect blocks\n");
								} else { // as long as not exceeding the double inidirect max, unlikely
									fd_t.locate_order += 1;
								}
								fd_t.locate_offset = 0;
								continue;
							}
							return -5;	
						}
					}
					return -3;
				}
				if(0 != block_store_fd_write(fs->BlockStore_fd,fd,&fd_t)){ // update the fd
					return readBytes;
				}
				return -6;					
			}
			return -2;
		}
		return 0;
	} 
	return -1;
}


/// Moves the file from one location to the other
///   Moving files does not affect open descriptors
/// \param fs The F17FS containing the file
/// \param src Absolute path of the file to move
/// \param dst Absolute path to move the file to
/// \return 0 on success, < 0 on error
///
int fs_move(F17FS_t *fs, const char *src, const char *dst){
	if(fs && src && dst ){ // FS, src and dst must not be NULL
		// Check if src is a valid file or dir, it must exist and must not be root. dst cannot be root either
		// The path must start with '/'
		char firstChar_src = *src;
		char firstChar_dst = *dst;
		if(0 != strcmp(src,"/") && 0 != strcmp(dst,"/") && firstChar_src == '/' && firstChar_dst == '/'){
			// Get the dirname and basename of src
			char src_dirc[strlen(src)+1];
			char src_basec[strlen(src)+1];
			strcpy(src_dirc,src);
			strcpy(src_basec,src);
			char *src_dir = dirname(src_dirc);
			char *src_base = basename(src_basec);
			// Get the dirname and basename of dst
			char dst_dirc[strlen(dst)+1];				
			char dst_basec[strlen(dst)+1];
			strcpy(dst_dirc,dst);
			strcpy(dst_basec,dst);
			char *dst_dir = dirname(dst_dirc);
			char *dst_base = basename(dst_basec);
			
			size_t src_parentDirInodeID = searchPath(fs,src_dir);// src parent dir inode number 
			size_t dst_parentDirInodeID = searchPath(fs,dst_dir);// dst parent dir inode numbeir
			// Parent directories of both src and dst must exist
			// Make sure the src basename is not one of the parent dir names of the dst
			// src cannot be the parent directory or above of dst
			if((!(strlen(src) < strlen(dst) && 0 == strncmp(src,dst,strlen(src)))) && src_parentDirInodeID != SIZE_MAX && dst_parentDirInodeID != SIZE_MAX){
				size_t src_inodeID = getFileInodeID(fs,src_parentDirInodeID,src_base);
				size_t dst_inodeID = getFileInodeID(fs,dst_parentDirInodeID,dst_base);
				// src inode must exist, but dst inode must not
				if(src_inodeID != 0 && dst_inodeID == 0){
					// If src and dst under same parent dir, just change the entry name of the src to dst_base
					if(src_parentDirInodeID == dst_parentDirInodeID){
						inode_t src_parentDirInode;
						directoryBlock_t src_parentDirBlock;
						if(block_store_inode_read(fs->BlockStore_inode,src_parentDirInodeID,&src_parentDirInode) && 
							block_store_read(fs->BlockStore_whole,src_parentDirInode.directPointer[0],&src_parentDirBlock))
						{	 
							size_t i=0;
							for(;i<7;i++){
								// If the entry name and inode number match those of the src, change the name of src to that of the dst
								if(src_parentDirBlock.dentries[i].inodeNumber == src_inodeID && 0 == strcmp(src_parentDirBlock.dentries[i].filename,src_base)){
									// Change the file name but not the inode number
									strcpy(src_parentDirBlock.dentries[i].filename,dst_base);
									if(block_store_write(fs->BlockStore_whole,src_parentDirInode.directPointer[0],&src_parentDirBlock)){
										return 0;// Success	
									}
								}
							}
							return -6;	
						}		
						return -5;
					} else {
						// ...If not under the same directory, add the src file or dir under the parent directory of dst
						// ...and remove the src file or dir entry from the parent directory of src
						// Since we rule out the possibility of src being parent of the dst, no worries about that
						// First, check if the dst parent directory is full
						inode_t dst_parentDirInode;
						inode_t src_parentDirInode;
						directoryBlock_t dst_parentDirBlock;	
						directoryBlock_t src_parentDirBlock;	
						if(block_store_inode_read(fs->BlockStore_inode,dst_parentDirInodeID,&dst_parentDirInode) && block_store_read(fs->BlockStore_whole,dst_parentDirInode.directPointer[0],&dst_parentDirBlock) && block_store_inode_read(fs->BlockStore_inode,src_parentDirInodeID,&src_parentDirInode) && block_store_read(fs->BlockStore_whole,src_parentDirInode.directPointer[0],&src_parentDirBlock))
						{
							bitmap_t *dst_bmp = bitmap_overlay(8,&(dst_parentDirInode.vacantFile));
							bitmap_t *src_bmp = bitmap_overlay(8,&(src_parentDirInode.vacantFile));
							if(dst_bmp && src_bmp){
								size_t i=0;
								for(;i<7;i++){
									// Select an empty entry, and set it to the name of dst and inode number of the src
									if(!bitmap_test(dst_bmp,i)){
										bitmap_set(dst_bmp,i);
										strcpy(dst_parentDirBlock.dentries[i].filename,dst_base);
										dst_parentDirBlock.dentries[i].inodeNumber = src_inodeID;
										size_t j=0;
										for(;j<7;j++){
											// Remove the src file entry from its parent dir file block
											if(src_parentDirBlock.dentries[j].inodeNumber == src_inodeID){
												bitmap_reset(src_bmp,j);
												memset(src_parentDirBlock.dentries[j].filename,'\0',FS_FNAME_MAX) ;
												src_parentDirBlock.dentries[j].inodeNumber = 0x00;
												// Update the parent directory inodes and file blocks of both dst and src
												if(block_store_write(fs->BlockStore_whole,dst_parentDirInode.directPointer[0],&dst_parentDirBlock)
												&& block_store_write(fs->BlockStore_whole,src_parentDirInode.directPointer[0],&src_parentDirBlock)
												&& block_store_inode_write(fs->BlockStore_inode,dst_parentDirInodeID,&dst_parentDirInode)
												&& block_store_inode_write(fs->BlockStore_inode,src_parentDirInodeID,&src_parentDirInode)) {
													bitmap_destroy(dst_bmp);
													bitmap_destroy(src_bmp);
													return 0;
												}
												bitmap_destroy(dst_bmp);
												bitmap_destroy(src_bmp);
												return -10;	
											}
										}
										
									}
								}
								bitmap_destroy(dst_bmp);
								bitmap_destroy(src_bmp);
								return -9;	
							}	
							return -8;
						}
						return -7;
					}
				}
				//printf("src_base:%s, dst_base:%s\n",src_base,dst_base);
				//printf("srcInodeID:%lu, dstInodeID:%lu\n",src_inodeID,dst_inodeID);
				return -4;
			}
			return -3;
		}
		return -2;	
	}
	return -1;
}


// Hardlink dst to the src.
// All hardlinked files having same data and meta data except naming differences
// Return 0 on success, or < 0 on error
int fs_link(F17FS_t *fs, const char *src, const char *dst){
	if(fs && src && dst){
		char firstChar_src = *src;
		char firstChar_dst = *dst;
		if(0 != strcmp(dst,"/") && firstChar_src == '/' && firstChar_dst == '/'){
      		 // Get the dirname and basename of src
       		char src_dirc[strlen(src)+1];
	    	char src_basec[strlen(src)+1];
	    	strcpy(src_dirc,src);
			strcpy(src_basec,src);
			char *src_dir = dirname(src_dirc);
			char *src_base = basename(src_basec);
			// Get the dirname and basename of dst
			char dst_dirc[strlen(dst)+1];				
			char dst_basec[strlen(dst)+1];
			strcpy(dst_dirc,dst);
			strcpy(dst_basec,dst);
			char *dst_dir = dirname(dst_dirc);
			char *dst_base = basename(dst_basec);
		
			size_t src_parentDirInodeID = searchPath(fs,src_dir);// src parent dir inode number 
			size_t dst_parentDirInodeID = searchPath(fs,dst_dir);// dst parent dir inode numbeir
			// Parent directories of both src and dst must exist
			// Make sure the src basename is not one of the parent dir names of the dst
			// src cannot be the parent directory or above of dst
			if(src_parentDirInodeID != SIZE_MAX && dst_parentDirInodeID != SIZE_MAX){
				size_t src_inodeID = getFileInodeID(fs,src_parentDirInodeID,src_base);
				size_t dst_inodeID = getFileInodeID(fs,dst_parentDirInodeID,dst_base);
				// src inode must exist, but dst inode must not
				if(src_inodeID != 0 && dst_inodeID == 0){
					// Check if the dst parent dir is full	
					inode_t dst_parentDirInode;
					inode_t src_inode;
					directoryBlock_t dst_parentDirBlock;
					if(block_store_inode_read(fs->BlockStore_inode,dst_parentDirInodeID,&dst_parentDirInode) && block_store_inode_read(fs->BlockStore_inode,src_inodeID,&src_inode) && block_store_read(fs->BlockStore_whole,dst_parentDirInode.directPointer[0],&dst_parentDirBlock) && src_inode.linkCount < 255) {
						bitmap_t *bmp = bitmap_overlay(8,&(dst_parentDirInode.vacantFile));
						if(bmp){
							//printf("dst path: %s\n",dst);
							size_t i = 0;
							for(;i<7;i++){
								// Find an empty entry and set filename to the dst_base and inodeNumber to src_inodeID
								if(!bitmap_test(bmp,i)){
									strncpy(dst_parentDirBlock.dentries[i].filename,dst_base,FS_FNAME_MAX);
									//printf("index: %lu,dst_base: %s\n",i,dst_parentDirBlock.dentries[i].filename);
									dst_parentDirBlock.dentries[i].inodeNumber = src_inodeID;
									// Increment linkCount by 1
									src_inode.linkCount += 1;
									if(src_inodeID == dst_parentDirInodeID){
										dst_parentDirInode.linkCount += 1;
									}
									bitmap_set(bmp,i);
									// Have to update the src inode first, in case src_inodeID == dst_parentDirInodeID, ie, link to yourself
									if(block_store_inode_write(fs->BlockStore_inode,src_inodeID,&src_inode) && block_store_inode_write(fs->BlockStore_inode,dst_parentDirInodeID,&dst_parentDirInode) && block_store_write(fs->BlockStore_whole,dst_parentDirInode.directPointer[0],&dst_parentDirBlock)){
										bitmap_destroy(bmp);
										return 0;
									}
									//printf("Error: -9\n");
									bitmap_destroy(bmp);
									return -9;	
								}
								//printf("index: %lu,dst_base: %s\n",i,dst_parentDirBlock.dentries[i].filename);
							}
							//printf("Error: -8\n");
							bitmap_destroy(bmp);
							return -7;	
						}
						//printf("Error: -6\n");
						return -6;
					}
					//printf("Error: -5\n");
					return -5;	
				}
				//printf("Error: -4\n");
				return -4;
			}
			//printf("Error: -3\n");
			return -3;
		}
		//printf("Error: -2\n");
		return -2;		
	}
	//printf("Error: -1\n");
	return -1;
}
