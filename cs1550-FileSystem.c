/*/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/
/*Virtual file system in user space
@Runyuan Yan*/


#define	FUSE_USE_VERSION 26
#include <stdbool.h> 
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//The attribute packed means to not align these things
struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nIndexBlock;				//where the index block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;
typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} ;


typedef struct cs1550_directory_entry cs1550_directory_entry;

//How many entries can one index block hold?
#define	MAX_ENTRIES_IN_INDEX_BLOCK (BLOCK_SIZE/sizeof(long))

struct cs1550_index_block
{
      //All the space in the index block can be used for index entries.
			// Each index entry is a data block number.
      long entries[MAX_ENTRIES_IN_INDEX_BLOCK];
};

typedef struct cs1550_index_block cs1550_index_block;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE)

struct cs1550_disk_block
{
	//All of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;

/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not.
 *
 * man -s 2 stat will show the fields of a stat structure
 */

//helper functions for bitmap operations
//check if the given bit in the bitNum is 0
static bool checkBit(int bitNum,int bitIndex){
	return (bitNum >> bitIndex | 0) == 0;	//true if this bit is 'free'
}
//sets the given index bit to 1
static char setBit(int bitNum,int bitIndex){
	return bitNum | (1<<bitIndex);
}

static int bitmap_find(FILE* file){
	char bitmap[1280];
	fseek(file,-3*512,SEEK_END);
	fread(bitmap,1280,1,file);
	int k;
	for(k=0;k<10240;k++){
		if(checkBit(bitmap[k/8],k%8)){
			bitmap[k/8]=setBit(bitmap[k/8],k%8);		//used	
			//update the bitmap
			fseek(file,-3*512,SEEK_END);
			fwrite(bitmap,1280,1,file);
			return k;
		}
	}
	return -1;	//not found
}
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
	char dir_name[MAX_FILENAME + 1];
	char filename[MAX_FILENAME + 1];
	char ext[MAX_EXTENSION + 1]; 
	int valid_name = sscanf(path, "/%[^/]/%[^.].%s", dir_name, filename, ext); 
	//check the filename length
	int res = 0;
	FILE *fp = fopen(".disk","rb+");
	memset(stbuf, 0, sizeof(struct stat));
	//is path the root dir?
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	}else if(valid_name==1){  //Check if name is subdirectory
		//start from the root check the subdirectories
		bool directory_found =false;
		cs1550_root_directory* root_dir = malloc(sizeof(cs1550_root_directory));
		fread(root_dir,sizeof(cs1550_root_directory),1,fp);
		int i;
		//printf("debugging 1:!\n");
		for(i =0;i<root_dir->nDirectories;i++){
			if(strcmp(dir_name,root_dir->directories[i].dname) == 0){
				directory_found=true;
				break;
			}
		}
		if(directory_found){
			//Might want to return a structure with these fields
			stbuf->st_mode = S_IFDIR | 0755;
			stbuf->st_nlink = 2;
			res = 0; //no error
		}
		else{//use your god damn brakets okay?
			res=-ENOENT;
		}
		free(root_dir);
	}
	//Check if name is a regular file
	else if(valid_name==3){
		bool file_found =false;
		fseek(fp,0,SEEK_SET);
		cs1550_root_directory* root_dir1 = malloc(sizeof(cs1550_root_directory));
		fread(root_dir1,sizeof(cs1550_root_directory),1,fp);
		cs1550_directory_entry* sub_directory = malloc(sizeof(cs1550_directory_entry));
		int i,j;
		for(i =0;i<root_dir1->nDirectories;i++){
			fseek(fp,512*root_dir1->directories[i].nStartBlock,SEEK_SET);
			fread(sub_directory,sizeof(cs1550_directory_entry),1,fp);
			for(j =0;j<sub_directory->nFiles;j++){
				if(strcmp(filename,sub_directory->files[j].fname)==0 && strcmp(ext,sub_directory->files[j].fext)==0){
					file_found = true;
					break;
				}
			}
			if(file_found)
				break;
		}
		if(file_found){
			//regular file, probably want to be read and write
			stbuf->st_mode = S_IFREG | 0666;
			stbuf->st_nlink = 1; //file links
			stbuf->st_size = sub_directory->files[j].fsize; //file size - make sure you replace with real size!
			res = 0; // no error
		}
		else{
			//Else return that file doesn't exist
			res = -ENOENT;
		}
	free(root_dir1);
	free(sub_directory);
	}
	else{
		res = -ENOENT;
	}
	fclose(fp);
	return res;
	
}

/*
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	char dir_name[MAX_FILENAME + 1];
	char filename[MAX_FILENAME + 1];
	char ext[MAX_EXTENSION + 1]; 
	int valid_name = sscanf(path, "/%[^/]/%[^.].%s", dir_name, filename, ext); 
	//check the filename length
	if(valid_name>1){
		return -ENOENT; 
	}
	//but if it's the root...
	bool is_root = (strcmp(path,"/") == 0);
	FILE *fp = fopen(".disk","rb+");
	bool directory_found =false;
	int dir_index = -1;
	cs1550_root_directory* root_dir = malloc(sizeof(cs1550_root_directory)); //root
	fread(root_dir,sizeof(cs1550_root_directory),1,fp);
	if(is_root && root_dir->nDirectories==0 ){//if the root is empty
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		fclose(fp);
		free(root_dir);
		return 0;
	}
	int i,k;
	
	for(i =0;i<root_dir->nDirectories;i++){
		if(is_root){
			filler(buf,root_dir->directories[i].dname, NULL, 0);
			continue;
		}
		
		if(strcmp(dir_name,root_dir->directories[i].dname)==0){	//try to find where the directory is located in disk
			directory_found=true;
			dir_index = root_dir->directories[i].nStartBlock;
			break;
		}
	}
	if(is_root){
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		fclose(fp);
		free(root_dir);
		return 0;
	}
	if(!directory_found){
		fclose(fp);
		free(root_dir);
		return -ENOENT;
	}
	//if we found the directory on disk, loop through it
	cs1550_directory_entry* sub_directory = malloc(sizeof(cs1550_directory_entry));
	fseek(fp,dir_index*512,SEEK_SET);
	fread(sub_directory,sizeof(cs1550_directory_entry),1,fp);

	char f_names[9];
	char f_ext[4];
	memset(f_names,'\0',sizeof(f_names));
	memset(f_ext,'\0',sizeof(f_ext));
	for(k=0;k<sub_directory->nFiles;k++){	//add all to the listing
		strncpy(f_names,sub_directory->files[k].fname,sizeof(f_names)-1);
		strncpy(f_ext,sub_directory->files[k].fext,sizeof(f_ext)-1);
		char* fn1 = strcat(f_names,".");
		char* fn2 = strcat(fn1,f_ext); 
		filler(buf,fn2,NULL, 0);
	}

	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;
	//This line assumes we have no subdirectories, need to change
	//if (strcmp(path, "/") != 0)
	//return -ENOENT;
	//the filler function allows us to add entries to the listing
	//read the fuse.h file for a description (in the ../include dir)
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	/*
	//add the user stuff (subdirs or files)
	//the +1 skips the leading '/' on the filenames
	filler(buf, newpath + 1, NULL, 0);
	*/
	fclose(fp);
	free(root_dir);
	free(sub_directory);
	return 0;
}

/*
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{
	char dir_name[MAX_FILENAME +1];
	char filename[MAX_FILENAME +1];
	char ext[MAX_EXTENSION +1]; 
	int valid_name = sscanf(path, "/%[^/]/%[^.].%s", dir_name, filename, ext); 
	//check the filename length
	//check if it's only under the root 
	if(valid_name >1)
	return -EPERM;
	//check length
	if(strlen(dir_name)>8){
		return -ENAMETOOLONG;
	}
	FILE *fp = fopen(".disk","rb+");
	cs1550_root_directory* root = malloc(sizeof(cs1550_root_directory));
	//start from the root check the subdirectories
	fread(root,sizeof(cs1550_root_directory),1,fp);
	if(root->nDirectories == 29){
		return -ENOSPC;
	}
	int i;
	for(i =0;i<root->nDirectories;i++){
		if(strcmp(dir_name,root->directories[i].dname) == 0){
			return -EEXIST;
		}
	}
	//
	char bitmap[1280];
	//search the bitmap to find the block
	fseek(fp,-3*512,SEEK_END);
	fread(bitmap,1280,1,fp);
	int h;
	for(h=0;h<10240;h++){
		if(checkBit(bitmap[h/8],h%8)){
			bitmap[h/8]=setBit(bitmap[h/8],h%8);
			break;
		}
	}
	//add the new dir to root
	root->nDirectories++;
	int sizeof_name = sizeof(root->directories[root->nDirectories-1].dname);
	strncpy(root->directories[root->nDirectories-1].dname,dir_name,sizeof_name);
	root->directories[root->nDirectories-1].nStartBlock=h;
	fseek(fp,0,SEEK_SET);
	fwrite(root,sizeof(cs1550_root_directory),1,fp); //update the disk root
	fseek(fp,-3*512,SEEK_END);
	fwrite(bitmap,1280,1,fp);			//update bitmap
	fseek(fp,h*512,SEEK_SET);			//go to the free block
	//make a new entey
	cs1550_directory_entry* new_dir = malloc(sizeof(cs1550_directory_entry));
	new_dir->nFiles=0;
	fwrite(new_dir,sizeof(cs1550_directory_entry),1,fp); //write the new entry to the disk
	(void) path;
	(void) mode;
	fclose(fp);
	free(root);
	free(new_dir);
	return 0;
}

/*
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
    return 0;
}

/*
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	unsigned long int file_name_length = MAX_FILENAME +1;
	unsigned long int file_ext_length = MAX_EXTENSION +1;
	char dir_name[file_name_length];
	char filename[file_name_length];
	char ext[file_ext_length];
	memset(dir_name,0,file_name_length);
	memset(filename,0,file_name_length);
	memset(ext,0,file_ext_length);	
	int valid_name = sscanf(path, "/%[^/]/%[^.].%s", dir_name, filename, ext); 
	//check the filename length

	if(strlen(filename) >MAX_FILENAME || strlen(ext)>MAX_EXTENSION){
		return -ENAMETOOLONG; 
	}
	if(valid_name == 1){
		return -EPERM;
	}
	FILE* fp =fopen(".disk","rb+");
	cs1550_root_directory* root = malloc(sizeof(cs1550_root_directory));
	cs1550_directory_entry* dir = malloc(sizeof(cs1550_directory_entry));
	fread(root,sizeof(cs1550_root_directory),1,fp);
	int i;
	int j;
	for(i =0;i<root->nDirectories;i++){
		if(strcmp(dir_name,root->directories[i].dname) == 0){
			fseek(fp,512*root->directories[i].nStartBlock,SEEK_SET);
			fread(dir,sizeof(cs1550_directory_entry),1,fp);
			break;
		}
	}
	if(i == root->nDirectories)
		return -ENOENT;
	//file already exist
	for(j=0;j<dir->nFiles;j++){
		if(strcmp(filename,dir->files[j].fname)==0 && strcmp(ext,dir->files[j].fext) ==0){
			free(root);
			free(dir);
			return -EEXIST;
			}
		}
	 //IF THE FILE DOESN'T EXIST AND EVERYTHING IS FINE
	if(dir->nFiles==MAX_FILES_IN_DIR){
		free(root);
		free(dir);
		return -ENOSPC;
	}

	char bitmap[1280];
	//search the bitmap to find the block
	fseek(fp,-3*512,SEEK_END);
	fread(bitmap,1280,1,fp);
	int h;
	int index_block =-1;//index block for the file
	int start_index =-1;//first entry in the index block
	for(h=0;h<10240;h++){
		if(checkBit(bitmap[h/8],h%8)){
			bitmap[h/8]=setBit(bitmap[h/8],h%8);
			if(index_block==-1){
				index_block = h;
			}else if(start_index ==-1){
				start_index =h;
				break;
			}
		}
	}
	//update the bitmap
	fseek(fp,-3*512,SEEK_END);
	fwrite(bitmap,1280,1,fp);

	//make an index block and write to disk
	cs1550_index_block* i_block = malloc(sizeof(cs1550_index_block));
	i_block->entries[0] = start_index;
	fseek(fp,512*index_block,SEEK_SET);
	fwrite(i_block,sizeof(cs1550_index_block),1,fp);//write the index block at :index_block 
	//update the directory information
	
	int sizeof_name = sizeof(dir->files[dir->nFiles].fname);
	int sizeof_ext =sizeof(dir->files[dir->nFiles].fext);
	strncpy(dir->files[dir->nFiles].fname,filename,sizeof_name);//copy the new file name
	strncpy(dir->files[dir->nFiles].fext,ext,sizeof_ext);//copy the extention
	dir->files[dir->nFiles].fsize = 0;	//set file size to 0
	dir->files[dir->nFiles].nIndexBlock = index_block; //set the index block to :index_block	
	dir->nFiles++;//increment the count of files in the directory
	fseek(fp,512*root->directories[i].nStartBlock,SEEK_SET);
	fwrite(dir,sizeof(cs1550_directory_entry),1,fp);		//write the updated directory to disk
	//success
	(void) mode;
	(void) dev;
	(void) path;
	free(i_block);
	free(root);
	free(dir);
	fclose(fp);
	return 0;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
    (void) path;

    return 0;
}

/*
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;
	char dir_name[MAX_FILENAME + 1];
	char filename[MAX_FILENAME + 1];
	char ext[MAX_EXTENSION + 1]; 
	int valid_name = sscanf(path, "/%[^/]/%[^.].%s", dir_name, filename, ext);
	if(valid_name==1){
		return -EISDIR;
	}
	FILE *fp = fopen(".disk","rb+");
	cs1550_root_directory* root = malloc(sizeof(cs1550_root_directory));
	cs1550_directory_entry* dirt = malloc(sizeof(cs1550_directory_entry));
	fread(root,sizeof(cs1550_root_directory),1,fp);
	int i;
	//check that size is > 0
	if(size<=0){		//size less than 0
		free(root);
		free(dirt);
		return -ENOENT;
	}
	//check to make sure path exists
	for(i=0;i<root->nDirectories;i++){
		if(strcmp(root->directories[i].dname,dir_name)==0){
			fseek(fp,512*root->directories[i].nStartBlock,SEEK_SET);
			fread(dirt,sizeof(cs1550_directory_entry),1,fp);
			break;
		}
	}
	if(i==root->nDirectories){	//path doesn't exist
		free(root);
		free(dirt);
		return -ENOENT;
	}
	int j;
	int file_size = 0;
	for(j=0;j<dirt->nFiles;j++){//j will be the index for the file in dirt
		if(strcmp(dirt->files[j].fname,filename)==0 && strcmp(dirt->files[j].fname,filename)==0){
			file_size=dirt->files[j].fsize;
			if(offset>dirt->files[j].fsize){ //offset too big//check that offset is <= to the file size
				free(root);
				free(dirt);
				return -EFBIG;		
			}
			break;
		} 
	}
	if(j==dirt->nFiles){// path doesn't exist
		free(root);
		free(dirt);
		return -ENOENT;
	}

	cs1550_index_block* index_blk = malloc(sizeof(cs1550_index_block));
	fseek(fp,512*dirt->files[j].nIndexBlock,SEEK_SET);
	fread(index_blk,sizeof(cs1550_index_block),1,fp);
	
	//find out how many blocks do we need...
	int count = file_size/512+1;	//how many blocks are used by the file
	int start_block = 0;	//where the write should start
	int start_from = 0;		//which byte in the block to start from
	if(offset<file_size){	
		start_block = offset/512;
		start_from = offset%512;
	}
	else 	
	{	
		start_block = file_size/512;
		start_from = file_size%512;	
	}
	cs1550_disk_block* data_block = malloc(sizeof(cs1550_disk_block));
	char data_buf[file_size];
	memset(data_buf,'\0',sizeof(data_buf));
	int total_to_read = file_size-offset;
	int a;
	for(a=start_block;a<count;a++){
		fseek(fp,index_blk->entries[a]*512,SEEK_SET);
		if (a==start_block){
			fseek(fp,start_from,SEEK_CUR);
		}
		if(total_to_read<=512){
			fread(data_block->data,total_to_read,1,fp);
			strncat(data_buf,data_block->data,total_to_read);
		}else{
			fread(data_block->data,512,1,fp);
			strncat(data_buf,data_block->data,512);
			free(data_block);
			data_block = malloc(sizeof(cs1550_disk_block));
			total_to_read -=512;
		}

	
	}
	//read in data
	//set size and return, or error
	strncpy(buf,data_buf,file_size-offset);
	//free(data_buf);
	free(root);
	free(dirt);
	free(index_blk);
	free(data_block);
	fclose(fp);
	return size;
}

/*
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size,
			  off_t offset, struct fuse_file_info *fi)
{
	char dir_name[MAX_FILENAME + 1];
	char filename[MAX_FILENAME + 1];
	char ext[MAX_EXTENSION + 1]; 
	sscanf(path, "/%[^/]/%[^.].%s", dir_name, filename, ext); 
	//check the filename length
	
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;
	FILE *fp = fopen(".disk","rb+");
	cs1550_root_directory* root = malloc(sizeof(cs1550_root_directory));
	cs1550_directory_entry* dirt = malloc(sizeof(cs1550_directory_entry));
	fread(root,sizeof(cs1550_root_directory),1,fp);
	int i;
	if(size<=0){		//size less than 0
		free(root);
		free(dirt);
		return -ENOENT;
	}

	for(i=0;i<root->nDirectories;i++){
		if(strcmp(root->directories[i].dname,dir_name)==0){
			fseek(fp,512*root->directories[i].nStartBlock,SEEK_SET);
			fread(dirt,sizeof(cs1550_directory_entry),1,fp);
			break;
		}
	}
	if(i==root->nDirectories){	//path doesn't exist
		free(root);
		free(dirt);
		return -ENOENT;
	}
	int j;
	int file_size =0;
	for(j=0;j<dirt->nFiles;j++){//j will be the index for the file in dirt
		if(strcmp(dirt->files[j].fname,filename)==0 && strcmp(dirt->files[j].fname,filename)==0){
			if(offset>dirt->files[j].fsize){ //offset too big
				free(root);
				free(dirt);
				return -EFBIG;		
			}
			file_size = dirt->files[j].fsize;
			break;
		} 
	}
	if(j==dirt->nFiles){// path doesn't exist
		free(root);
		free(dirt);
		return -ENOENT;
	}
	cs1550_index_block* index_blk = malloc(sizeof(cs1550_index_block));
	fseek(fp,512*dirt->files[j].nIndexBlock,SEEK_SET);
	fread(index_blk,sizeof(cs1550_index_block),1,fp);
	
	int count = file_size/512+1;	//how many blocks are used by the file
	int block_need = 0;
	int start_block = 0;	//where the write should start
	int start_from = 0;		//which byte in the block to start from
	//size_t new_file_size = 0;
	if(offset<file_size){	//this is an overwrite
		int free_space = offset+size - count*512;	//how much more space needed for writing
		if(free_space>0){	//need more blocks
			block_need = 1 + (offset+size-file_size)/512;
		}
		file_size = offset+size ; 				//file size is now offset+size
		start_block = offset/512;
		start_from = offset%512;
	}
	else 	//this is a append
	{	

		if(file_size%512 == 0 && file_size!=0 ){		//this means that there is no more space left for the write	
			block_need = size/512 + 1;
		}
		//if the file ends in the middle of a block
		else if(512-file_size%512 >= size){		//there is enough space to write, no more block needed

		}
		else{	//remaining space in the block is not enough
			block_need = 1 + (size-(512-file_size%512))/512;		//simple arithmitic
		}
		start_block = file_size/512;
		start_from = file_size%512;
		file_size += size;
		
	}
	//now we need to find the blocks if needed
	if(block_need >0){
		int a;
		for(a=0;a<block_need;a++){
			index_blk->entries[count] = bitmap_find(fp);
			count++;
		}
	}
	fseek(fp,512*dirt->files[j].nIndexBlock,SEEK_SET);
	fwrite(index_blk,sizeof(cs1550_index_block),1,fp);
	//after everythinh start writing to disk
	long data_to_write = size;
	cs1550_disk_block* data_block = malloc(sizeof(cs1550_disk_block));
	char* data_buf = malloc(size);
	strcpy(data_buf,buf);

	strncpy(data_block->data,data_buf,sizeof(data_block->data));
	while(data_to_write>0){
		fseek(fp,index_blk->entries[start_block]*512,SEEK_SET);
		fseek(fp,start_from,SEEK_CUR);
		if(data_to_write<=512){
			fwrite(data_block,data_to_write,1,fp);
		}else{
			fwrite(data_block,512,1,fp);
			free(data_block);
			data_block = malloc(sizeof(cs1550_disk_block));
			strncpy(data_block->data,data_buf+=512*sizeof(char),sizeof(data_block->data));
		}

		data_to_write -=512;
		start_block++;
	}

	//Also update the file size 
	dirt->files[j].fsize =file_size;
	fseek(fp,512*root->directories[i].nStartBlock,SEEK_SET);
	fwrite(dirt,sizeof(cs1550_directory_entry),1,fp);
	free(data_buf);
	free(index_blk);
	free(data_block);
	free(root);
	free(dirt);
	fclose(fp);
	//set size (should be same as input) and return, or error
	return size;
}
/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}


/*
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}

/* Thanks to Mohammad Hasanzadeh Mofrad (@moh18) for these
   two functions */
static void * cs1550_init(struct fuse_conn_info* fi)
{

	  (void) fi;
    printf("We're all gonna live from here ....\n");
		return NULL;
}

static void cs1550_destroy(void* args)
{
		(void) args;
    printf("... and die like a boss here\n");
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
    .getattr	= cs1550_getattr,
    .readdir	= cs1550_readdir,
    .mkdir	= cs1550_mkdir,
		.rmdir = cs1550_rmdir,
    .read	= cs1550_read,
    .write	= cs1550_write,
		.mknod	= cs1550_mknod,
		.unlink = cs1550_unlink,
		.truncate = cs1550_truncate,
		.flush = cs1550_flush,
		.open	= cs1550_open,
		.init = cs1550_init,
    .destroy = cs1550_destroy,
};

//Don't change this.
int main(int argc, char *argv[])
{
	//check look at the bit map to see if the root exits
	FILE *fp = fopen(".disk","rb+");
	unsigned char bmap[1280];
	fseek(fp,-3*512,SEEK_END);
	fread(bmap,1280,1,fp);
	//if the disk is empty create the root write to disk and initialize the bitmap
	//printf("check bit %d\n",checkBit(3,1));//0
	//printf("check set bit%d\n",setBit(3,2) );//7
	//printf("check reset bit%d\n",resetBit(7,2) );//3
	// checking the first bit of the first entry
	if(checkBit(bmap[0],0)){
		//write a new root directory to disk
		cs1550_root_directory* root = malloc(sizeof(cs1550_root_directory));
		root->nDirectories = 0;
		//root->directories=NULL;
		fseek(fp,0,SEEK_SET);
		fwrite(root,sizeof(cs1550_root_directory),1,fp);
		//a new bitmap and initialized values
		//1280 blocks
		bmap[0]=setBit(0,0); //same as bmap[0]=1
		/*
		bmap[1279] = setBit(bmap[1279],5);
		bmap[1279] =setBit(bmap[1279],6);
		bmap[1279] =setBit(bmap[1279],7);*/
		// really the same as: last 3 bits of last entry
		bmap[1279] = 224;
		fseek(fp,-3*512,SEEK_END);
		fwrite(bmap,1280,1,fp);
		free(root);
	}
	fclose(fp);
	return fuse_main(argc, argv, &hello_oper, NULL);
}
