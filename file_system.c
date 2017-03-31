#define FUSE_USE_VERSION 26
#include <fuse.h>
//Required to get size of "disk" file
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <errno.h>

// size of a data block
#define BLK_SIZE 4096

static struct fuse_operations opers;

char* diskName = "./myDisk";

/* Global file discriptor for the disk */
int fd;

/* Starting block of the current directory */
uint32_t cur_dir_blk;

/*
 * Gets the size of the disk file
 */
int get_disksize(const char* filename) {
   int fd = open(filename, O_RDWR);
	if(fd == -1){
		perror("Disk file failed to open");
		exit(1);
	}
	struct stat fileinfo;
	int fstat_ret = fstat(fd, &fileinfo);
	if(fstat_ret == -1) {
		perror("Error in reading disk metadata");
		exit(1);
	}
	if(close(fd) == -1){
		perror("Disk file failed to close");
		exit(1);
	}
	return fileinfo.st_size;
}

/*
 * Superblock struct. Placed at the 0th block of the disk
 */
typedef struct superblock {
	uint32_t magic_num;
	uint32_t num_blocks;
	uint32_t num_blocks_fat;
	uint32_t blk_size;
	uint32_t root_start;
} superblock;

/*
 * directory struct. Only holds a single directory entry
 */
typedef struct directory {
       char fname[24];
       uint64_t creation_time;
       uint64_t mod_time;
       uint64_t access_time;
       uint32_t len;
       uint32_t start_blk;
       uint32_t flags;
       uint32_t unused;
} directory;

/*
 * Initialize superblock and store it at the 0th block of the disk.
 * Takes the size of the disk as argument so it can be used to compute N and k
 */
void superblock_init(int fsize, int fd) {
	superblock sb; 
	sb.magic_num = 0xFADEDBEE;
	sb.num_blocks = (fsize / BLK_SIZE);
	sb.num_blocks_fat = (sb.num_blocks / 1024);
	// if the number of blocks N was not evenly divisible by 1024,
	// truncating division was done so increase the number of fat blocks k
	// by 1 so that there are enough blocks to fit all N entries
	if(sb.num_blocks % 1024 > 0){
		sb.num_blocks_fat++;
	}
	sb.blk_size = BLK_SIZE;
	// Root start is k+1
	sb.root_start = sb.num_blocks_fat + 1;
	// set the current directory to the root directory
   cur_dir_blk = sb.root_start;	
	//printf("sb.numblocks: %d\n", sb.num_blocks);
	// Now write the superblock to the disk
	lseek(fd, 0, SEEK_SET);
	write(fd, &sb, sizeof(sb));
}

/*
 * Initializes the fat table. First reads in the superblock to get N and k.
 * Then initializes an array of 4 byte signed integers that has length N.
 * The entries 0 to k have -1s to indicate that those blocks should not be
 * used so the superblock and fattable are not overwritten. The k+1st block
 * will store the root directory and it starts off as only one entry so a -2
 * is placed at that k+1st entry in the fat table. The remaining entries k+2 to
 * N-1 get 0s indicating that those blocks on the disk are free to use.
 */
void fat_init(int fd){
   lseek(fd, 0, SEEK_SET);
   superblock temp;
	read(fd, &temp, sizeof(temp));
   lseek(fd, BLK_SIZE, SEEK_SET); 
	int32_t fattable[temp.num_blocks];
	// set FAT entries 0 to k to -1
	for(int i = 0; i < temp.num_blocks_fat+1; ++i){
		fattable[i] = -1;
	} 
	// set FAT entries k+1 to N to 0
	for(int i = temp.num_blocks_fat+1; i < temp.num_blocks; ++i){
		fattable[i] = 0;
	}
	// update root entry in fat.
	fattable[temp.root_start] = -2;
	// write FAT table to disk
	write(fd, fattable, sizeof(fattable));
}

/*
 * *************Calling function must make sure block and dot unused
 * on the disk.***********************
 * Takes the block where the directory is to be initialized. Also takes
 * the starting block of the parent(..) and the starting block of the
 * directory itself. So block and dot should be the same.
 * Populates directory structs "." and ".." with all of the relevant information.
 * Then writes the directory entries to block. Because new block was written
 * update the FAT with a -2 at FAT[block]
 */
void dir_init(int fd, int block, uint32_t dot, uint32_t dotdot){	
    directory dotentry;
    strcpy(dotentry.fname, ".");
    dotentry.creation_time = (uint64_t)time(NULL);
    dotentry.mod_time = (uint64_t)time(NULL);
    dotentry.access_time = (uint64_t)time(NULL);
    dotentry.len = 64;
    dotentry.start_blk = dot;
    dotentry.flags = 0x00000001;
    dotentry.unused = 0xBEEF;

    directory ddotentry;
    strcpy(ddotentry.fname, "..");
    ddotentry.creation_time = (uint64_t)time(NULL);
    ddotentry.mod_time = (uint64_t)time(NULL);
    ddotentry.access_time = (uint64_t)time(NULL);
    ddotentry.len = 64;
    ddotentry.start_blk = dotdot;
    ddotentry.flags = 0x00000001; 
    ddotentry.unused = 0xBEEF;

    // write . and .. in the new directory
	 lseek(fd, block*BLK_SIZE, SEEK_SET);
    write(fd, &dotentry, sizeof(dotentry));    
    write(fd, &ddotentry, sizeof(ddotentry));

	 // update fat table entry
	 lseek(fd, BLK_SIZE, SEEK_SET);
	 lseek(fd, block*4, SEEK_CUR);
	 int32_t neg_two = -2;
	 write(fd, &neg_two, sizeof(neg_two));
}

/*
 * Initializes the root directory. Calls the dir_init 
 * function with block=dot=dotdot = k
 */
void root_init(int fd){
   lseek(fd, 0, SEEK_SET);
   superblock temp;
	read(fd, &temp, sizeof(temp));
	// go to k+1 datablock
	lseek(fd, temp.root_start*BLK_SIZE, SEEK_SET);
	// put root directory there
   dir_init(fd, temp.root_start, temp.root_start, temp.root_start);
}

/*
 * Given path name for a file and the block number of the directory where to begin
 * the search(either root or some other current directory). Parses the path into
 * into individual path tokens and recursively looks through the entire path by 
 * calling find_path with the truncated path and the block number of the current
 * directory. For instance if the path was /foo/bar/quux and the file foo was found
 * in the root with start block 3001, the recursive call would be find_path("/bar/quux",3001).
 * If the file quux is found we return the start block of quux. We also return the
 * metadata of quux by reference in the dir_entry directory pointer argument. 
 */
int find_path(const char *path, int block_num, directory *dir_entry){
    // start at the beginning of FAT
    lseek(fd, BLK_SIZE, SEEK_SET);
    // go to the correct block
    lseek(fd, block_num*4, SEEK_CUR);
    // find the value in FAT table, -1, 0, -2, or k+1 to N-1
    int32_t temp;
    read(fd, &temp, sizeof(temp));
    // do stuff only if its -2
    if(temp == -1 || temp == 0){
    	return -1;
    }else{
		// Parse string to find desired path and truncate the remaining
		// path so it can be recursively called
	   char *p;
		char str[strlen(path)];
		strcpy(str, path);

		// flag indicates whether path starts with slash or not
      int flag = 0;
		// indicates whether last entry in directory path
		int last = 0;
		p = str;
		if(*p == '/'){
         p =(char *)(p + sizeof(char));
			flag = 1;
		}
		unsigned int k = 0;
		while(*p != '/'){
         if(k == strlen(str) - 1){	
            last = 1;
				break;
			}
         p =(char *)(p + sizeof(char));
			k++;
		}
		// entry is the name of the file to search for in the current directory
		char entry[k];
		char rpath[strlen(str)-k];
		if(flag){
         memmove(entry, str + 1, k);
		}
		else{
         memmove(entry, str, k);
		}
		// str now has the remaining path to be used in the future recursive call
		if(!last){
         memmove(rpath, str + k + 1, strlen(str)-k);
		}

		//printf("Remaining path: %s\n", rpath);
		if(strlen(entry)==0){
         strcpy(entry, ".");
		}
      if(strcmp(entry,"/") == 0){
         strcpy(entry, ".");
		}
    
		/*****************************************************/ 	
		directory dtemp;
		// iterate over directory blocks because a directory may span several
		// data blocks
      while(1){
	      lseek(fd, block_num*BLK_SIZE, SEEK_SET);
          // iterate through single datablock of a directory to find 
			 // a directory entry that matches the entry string
			for(int i = 0; i < 64; ++i){
				   read(fd, &dtemp, sizeof(directory)); 
					// because of how we are removing files without defregmenting the directory
					// blocks, the entire directory block must be searched.
					if(dtemp.start_blk == 0){
						continue;
					}else{	
						// strcmp was not working for some reason, possibly becasue dtemp.fname
						// has length 24 and entry may not
						if(strncmp(dtemp.fname, entry, k) == 0) {	
							// if there are more path entries to search for then call find path
							if(!last){	
								return find_path(rpath, dtemp.start_blk, dir_entry);
							}
						   // otherwise return the start block of the found file
							// also populate the dir_entry directory struct so the
							//calling function can have the file metadata
							else{	
								strcpy(dir_entry->fname, dtemp.fname);
								dir_entry->creation_time = (uint64_t)dtemp.creation_time;
								dir_entry->mod_time = (uint64_t)dtemp.mod_time;
								dir_entry->access_time = (uint64_t)dtemp.access_time;
								dir_entry->len = dtemp.len;
								dir_entry->start_blk = dtemp.start_blk;
								dir_entry->flags = dtemp.flags;
								dir_entry->unused = dtemp.unused;
								return dtemp.start_blk;
							}
						}
						// if the name matches, truncate to the next /
						// recursively call this function again with the 
						// substring and dtemp.start_blk
						// if after truncating, substring is empty, return 
						// start_blk of found file
					}
			 }
			 // if there are no more blocks in the fat for the directory then 
			 // the path doesnt exist
			 if(temp == -2){	
             return -1;
			 }
			 // otherwise load in next directory entry 
			 else{
             block_num = temp;
				 lseek(fd, BLK_SIZE, SEEK_SET);
				 lseek(fd, 4*block_num, SEEK_CUR);
				 read(fd, &temp, sizeof(temp));
			 }
		}
    }
}

void add_dir_entry(int fd, char *fname, int32_t parent_blk, int32_t child_blk, int is_dir){
   int32_t parent_fat;
	int32_t parent_idx = parent_blk;
	lseek(fd, BLK_SIZE, SEEK_SET);
	lseek(fd, 4*parent_blk, SEEK_CUR);
	read(fd, &parent_fat, sizeof(int32_t));
	// find the last FAT entry in the parent directory
	while(1){
		if(parent_fat != -2){
         parent_idx = parent_fat;
		}
      else{
         break;
		}
		lseek(fd, BLK_SIZE, SEEK_SET);
	   lseek(fd, 4*parent_fat, SEEK_CUR);
	   read(fd, &parent_fat, sizeof(int32_t));
	}

  	//initialize new entry to add to parent directory
	directory new_entry;
	strcpy(new_entry.fname, fname);
	new_entry.creation_time = (uint64_t)time(NULL);
	new_entry.mod_time = (uint64_t)time(NULL);
	new_entry.access_time = (uint64_t)time(NULL);
	// if it is a directory then there are initially two entries . and ..
	if(is_dir){
		new_entry.len = 2*sizeof(directory);
		new_entry.flags = 0x00000001;
	}
	// a new file cant have any bytes
	else{
		new_entry.len = 0;
		new_entry.flags = 0;
	}
	new_entry.start_blk = child_blk;
	new_entry.unused = 0xBEEF;

	int found_empty_spot = 0;
	int directory_slot = 0;
   // find first empty directory entry
	directory temp_dir;
	lseek(fd, BLK_SIZE*parent_idx, SEEK_SET);
	for(directory_slot = 0; directory_slot < 64; ++directory_slot){
		read(fd, &temp_dir, sizeof(directory));
		if(temp_dir.start_blk == 0){
			found_empty_spot = 1; 	
			break;
		}
	}

	// must update FAT entry of child block because it was just added to the data block
	lseek(fd, BLK_SIZE, SEEK_SET);
	lseek(fd, child_blk*4, SEEK_CUR);
	int32_t fat_value;
	read(fd, &fat_value, sizeof(int32_t));
	if(fat_value <= 0) {
	   int32_t negative_two = -2;
		lseek(fd, BLK_SIZE+(child_blk*4), SEEK_SET);
	   write(fd, &negative_two, sizeof(int32_t));
	}


   // if we didn't find an empty slot in the directory block then we need to expand the
	// directory
	if(!found_empty_spot){
		// read in superblock
		superblock sb;
		lseek(fd, 0, SEEK_SET);
		read(fd, &sb, sizeof(superblock));

      lseek(fd, BLK_SIZE, SEEK_SET);
		int32_t freefat = 0;
		int32_t temp_fat;
	   int found_empty = 0;
		// iterate over entire FAT to find an empty entry
		// if one is not found then the disk is full
		for(; freefat < sb.num_blocks; ++freefat){
		   read(fd, &temp_fat, sizeof(int32_t));
			if(temp_fat == 0){
				found_empty = 1;
				// go to the free fat entry and set it to -2
            lseek(fd, BLK_SIZE, SEEK_SET);
				lseek(fd, 4*freefat, SEEK_CUR);
				int32_t neg_two = -2;
				write(fd, &neg_two, sizeof(int32_t));
				// update previous fat entry so block chain is complete
				lseek(fd, BLK_SIZE, SEEK_SET);
				lseek(fd, 4*parent_idx, SEEK_CUR);
				write(fd, &freefat, sizeof(int32_t));
				break;
			}
		}
		if(!found_empty){
         perror("fat is full\n");
			return;
		}
		// add new directory to top of fresh block
		lseek(fd, BLK_SIZE*freefat, SEEK_SET);
		write(fd, &new_entry, sizeof(directory));
		return;
   }
	// there was room for a directory entry in a directory so just add new entry there
	else{
		lseek(fd, BLK_SIZE*parent_idx, SEEK_SET);
      lseek(fd, sizeof(directory)*directory_slot, SEEK_CUR);
		write(fd, &new_entry, sizeof(directory));
	} 
}

// given the starting block of a directory and a directory entry name
// returns the actual block number that the entry is in and the index
// of the entry in that block(by reference)
int find_dir_entry(int fd, int32_t start_blk, char *entry_name, int32_t *actual_block){
   lseek(fd, BLK_SIZE, SEEK_SET);
   lseek(fd, 4*start_blk, SEEK_CUR);
	int32_t fat_ent;
	int32_t fat_index;
	fat_index = start_blk;	
	directory dtemp;

	while(1){
      read(fd, &fat_ent, sizeof(int32_t));
		lseek(fd, fat_index*BLK_SIZE, SEEK_SET);
		for(int dir_slot = 0; dir_slot < 64; ++dir_slot){
         read(fd, &dtemp, sizeof(directory));
			if(strcmp(dtemp.fname, entry_name)==0){
            *actual_block = fat_index;
				printf("actual_block: %d\n", *actual_block);
				printf("fat_index: %d\n", fat_index);
				return dir_slot;
			}
		}
		if(fat_ent == -2){
         return -1;
		}
		lseek(fd, BLK_SIZE, SEEK_SET);
		lseek(fd, fat_ent, SEEK_CUR);
		fat_index = fat_ent;
	}
}

static int fat_getattr(const char *path, struct stat *stbuf){

   uint32_t block;
	
   //get location of k+1, path is an absolute path
	if(strncmp("/", path, 1) == 0){
		lseek(fd, 0, SEEK_SET);
		superblock temp;
		read(fd, &temp, sizeof(temp));
		block = temp.root_start;
	// path is relative to current directory
	}else{
        block = cur_dir_blk;
	} 
	directory *dir_entry = (directory *)malloc(sizeof(directory));
   int start_block = find_path(path, block, dir_entry);
   // file path not found so return ENOENT
	if(start_block == -1){
      //path does not exist so return something 
		free(dir_entry);	
		return -ENOENT;
	}

	// file path was found so populate stbuf
   // we dont support hard links so set to 0
	stbuf->st_nlink = 0;
	// set st_uid to 1 for root
	stbuf->st_uid = 1;
	stbuf->st_gid = 1;
	
   //set times
	stbuf->st_atim.tv_sec = dir_entry->access_time;
	stbuf->st_mtim.tv_sec = dir_entry->mod_time;
	stbuf->st_ctim.tv_sec = dir_entry->creation_time;

	stbuf->st_rdev = 0;
	// if is directory. Give all permisions 
   if(dir_entry->flags){
      stbuf->st_mode = S_IFDIR | 0x0777;
	}
	//not a directory
	else{
      stbuf->st_mode = S_IFREG | 0x0777;
	}
   stbuf->st_size = dir_entry->len;
	free(dir_entry);
	return 0;
}

static int fat_mkdir(const char* path, mode_t mode){
	// get superblock
	
	lseek(fd, 0, SEEK_SET);
	superblock temp;
	read(fd, &temp, sizeof(temp));

  	directory *dir_entry = (directory *)malloc(sizeof(directory));	
	// directory already exists so do not make directory
	if(find_path(path, temp.root_start, dir_entry) != -1){
      // maybe don't return noent***********************
		free(dir_entry);
      return -EEXIST;
	}
	// directory does not exist, so we need to find the start block of the 
	// prefix and add a new directory entry to the directory of the prefix
	// with the name of the suffix.
	else{
      //parses the path into prefix and suffix
      char str[strlen(path)];
		strcpy(str, path);
	   int numberOfSlash = 0;
		int has_slash = 0;
		for(unsigned int i = 0; i < strlen(str); ++i){
         if(str[i] == '/'){
				++has_slash;
            ++numberOfSlash;
			}
		}
		
		int index = 0;
		while(numberOfSlash > 0){
         if(str[index] == '/'){
            --numberOfSlash;
			}
			++index;
		}
	
		int array_length;
		char temp_prefix_path[index-1];
		if(has_slash ==0 || has_slash == 1){ 
			array_length = 1;
		}
		else{
			array_length = index-1;
			for(int j = 0; j < index-1; ++j){
				temp_prefix_path[j] = str[j];
			}
			temp_prefix_path[index -1] = '\0';
		}
		char prefix_path[array_length];
		if(array_length == 1){
         strcpy(prefix_path, "/");
		}
		else{
         strcpy(prefix_path, temp_prefix_path);
		}
		int dirLength = strlen(str)-index;
		char suffix_path[dirLength];
		int dirIndex = 0;
		for(int k = index; k < strlen(str); ++k){
         suffix_path[dirIndex] = str[k];
			++dirIndex;
		}
      suffix_path[dirIndex] = '\0';
		//************************************************************************
      ///////***********parsing2***************///////////
   char str_2[strlen(prefix_path)];
	strcpy(str_2, prefix_path);
	numberOfSlash = 0;
	has_slash = 0;
	for(unsigned int i = 0; i < strlen(str_2); ++i){
		if(str_2[i] == '/'){
			++has_slash;
			++numberOfSlash;
		}
	}
	index = 0;
	while(numberOfSlash > 0){
		if(str_2[index] == '/'){
			--numberOfSlash;
		}
		++index;
	}
   int array_length_2;
   char temp_prefix_path_2[index-1];
	if(has_slash ==0 || has_slash == 1){
		array_length_2 = 1;
	}
	else{
		array_length_2 = index - 1;
		for(int j = 0; j < index-1; ++j){
			temp_prefix_path_2[j] = str_2[j];
		}
		temp_prefix_path_2[index -1] = '\0';
	}
	char prefix_path_2[array_length_2];
	if(array_length_2 == 1){
      strcpy(prefix_path_2, "/");
	}
	else{
      strcpy(prefix_path_2, temp_prefix_path_2);
	}
	dirLength = strlen(str_2)-index;
	char suffix_path_2[dirLength];
	dirIndex = 0;
	for(int k = index; k < strlen(str_2); ++k){
		suffix_path_2[dirIndex] = str_2[k];
		++dirIndex;
	}
	suffix_path_2[dirIndex] = '\0';
	///////////////////////////////////////////////////

      //get the start block of the directory where the new directory entry will be added
		int parent_blk = find_path(prefix_path, temp.root_start, dir_entry);
		// find next free entry in fat
		lseek(fd, BLK_SIZE, SEEK_SET);
		int found_empty = 0;
		int32_t open_blk;
		for(int i = 0; i < temp.num_blocks; ++i){
			read(fd, &open_blk, sizeof(int32_t));
			if(open_blk == 0){
            found_empty = 1;
				open_blk = i;
				break;
			}
		}
      if(!found_empty){
         return -ENOSPC;
		}
		dir_init(fd, open_blk, open_blk, parent_blk);
		add_dir_entry(fd, suffix_path, parent_blk, open_blk, 1);
		lseek(fd, parent_blk*BLK_SIZE, SEEK_SET);
		directory current_dot;
		read(fd, &current_dot, sizeof(directory));
		current_dot.len += 64;
		lseek(fd, parent_blk*BLK_SIZE, SEEK_SET);
		write(fd, &current_dot, sizeof(directory));
		//Read the ".." entry
		read(fd, &current_dot, sizeof(directory));
		int32_t actual_block;
		int dotdot_entry = find_dir_entry(fd, current_dot.start_blk, suffix_path_2, &actual_block);
		lseek(fd, actual_block*BLK_SIZE, SEEK_SET);
		lseek(fd, dotdot_entry*sizeof(directory), SEEK_CUR);
		read(fd, &current_dot, sizeof(directory));
		current_dot.len += 64;
		lseek(fd, actual_block*BLK_SIZE, SEEK_SET);
		lseek(fd, dotdot_entry*sizeof(directory), SEEK_CUR);
		write(fd, &current_dot, sizeof(directory));

		return 0;      
	}
}

static int fat_unlink(const char *path){
	/////////******parsing**************//////////////
	char str[strlen(path)];
	strcpy(str, path);
	int numberOfSlash = 0;
	int has_slash = 0;
	for(unsigned int i = 0; i < strlen(str); ++i){
		if(str[i] == '/'){
			++has_slash;
			++numberOfSlash;
		}
	}
	
	int index = 0;
	while(numberOfSlash > 0){
		if(str[index] == '/'){
			--numberOfSlash;
		}
		++index;
	}
	int array_length;
	char temp_prefix_path[index-1];
	if(has_slash ==0 || has_slash == 1){
		array_length = 1;
	}
	else{
		array_length = index -1;
		for(int j = 0; j < index-1; ++j){
			temp_prefix_path[j] = str[j];
		}
		temp_prefix_path[index -1] = '\0';
	}
	char prefix_path[array_length];
	if(array_length ==1){
      strcpy(prefix_path, "/");
	}
	else{
      strcpy(prefix_path, temp_prefix_path);
	}
	int dirLength = strlen(str)-index;
	char suffix_path[dirLength];
	int dirIndex = 0;
	for(int k = index; k < strlen(str); ++k){
		suffix_path[dirIndex] = str[k];
		++dirIndex;
	}
	suffix_path[dirIndex] = '\0';	
   /*****************************************************/

   lseek(fd, 0, SEEK_SET);
   superblock temp;
   read(fd, &temp, sizeof(temp));
   int root_block = temp.root_start;      
   directory temp_dir;
   // If the file doesn't exist or is a directory then you can't delete anything.
   if(find_path(path, root_block, &temp_dir) == -1 || temp_dir.flags){
      return -ENOENT;
   }
   int prefix_block = find_path(prefix_path, root_block, &temp_dir);
   
   int32_t next_block;
	while(1){	 
     lseek(fd, BLK_SIZE, SEEK_SET);
	  lseek(fd, prefix_block*4, SEEK_CUR);
	  read(fd, &next_block, sizeof(int32_t));

	  lseek(fd, prefix_block*BLK_SIZE, SEEK_SET);
	  int directory_slot = 0;
	  for(; directory_slot < 64; ++directory_slot){
         read(fd, &temp_dir, sizeof(directory));
		   if(strcmp(temp_dir.fname, suffix_path)==0){
             uint32_t start_blk  = temp_dir.start_blk;	
			    directory empty_dir;
			    empty_dir.start_blk = 0;
			    lseek(fd, BLK_SIZE*prefix_block, SEEK_SET);
			    lseek(fd, directory_slot*sizeof(directory), SEEK_CUR);
			    write(fd, &empty_dir, sizeof(directory));

			    //zero out data blocks
			    int32_t data_fat;
			    while(1){
                lseek(fd, BLK_SIZE, SEEK_SET);
				    lseek(fd, start_blk*4, SEEK_CUR);
				    read(fd, &data_fat, sizeof(int32_t));
                char empty[BLK_SIZE];
	             for(int i = 0; i < 4096; ++i){
                   empty[i] = 0;
	             }
				    lseek(fd, start_blk*BLK_SIZE, SEEK_SET);
				    write(fd, empty, BLK_SIZE);
       
				    if(data_fat == -2){
                    return 0;
				    }
				    else{
                    start_blk = data_fat;
				    }				  
			    }
		   }
	  }
     if(next_block == -2){
        return -ENOENT;
	   }
	   else{
         prefix_block = next_block;
		}
   
	}

   return 0;
}

static int fat_create(const char *path, mode_t mode, struct fuse_file_info *fi){
	// first check if file exists in the prefix. so first get prefix and suffix
   // parses the path into prefix and suffix
	//     **************** NEED TO ADD ITERATING THROUGH MORE BLOCKS BECAUSE IT ONLY ITERATES 64
	//     ****************
	////////////////Parsing/////////////////
	char str[strlen(path)];
	strcpy(str, path);
	int numberOfSlash = 0;
	int has_slash = 0;
	for(unsigned int i = 0; i < strlen(str); ++i){
		if(str[i] == '/'){
			++has_slash;
			++numberOfSlash;
		}
	}
	int index = 0;
	while(numberOfSlash > 0){
		if(str[index] == '/'){
			--numberOfSlash;
		}
		++index;
	}
   int array_length;
   char temp_prefix_path[index-1];
	if(has_slash ==0 || has_slash == 1){
		array_length = 1;
	}
	else{
		array_length = index - 1;
		for(int j = 0; j < index-1; ++j){
			temp_prefix_path[j] = str[j];
		}
		temp_prefix_path[index -1] = '\0';
	}
	char prefix_path[array_length];
	if(array_length == 1){
      strcpy(prefix_path, "/");
	}
	else{
      strcpy(prefix_path, temp_prefix_path);
	}
	int dirLength = strlen(str)-index;
	char suffix_path[dirLength];
	int dirIndex = 0;
	for(int k = index; k < strlen(str); ++k){
		suffix_path[dirIndex] = str[k];
		++dirIndex;
	}
	suffix_path[dirIndex] = '\0';
	/////////////////////////////////////////////

   //go to prefix data block to check if it exists
   lseek(fd, 0, SEEK_SET);
   superblock temp;
   read(fd, &temp, sizeof(temp));
   int root_block = temp.root_start;      
   directory temp_dir;
   //file already exists
   if(find_path(path, root_block, &temp_dir) != -1){
      return -EEXIST;
   }
   int prefix_block = find_path(prefix_path, root_block, &temp_dir);

    //find a free place to put the new data block. Search through fat table
	 lseek(fd, BLK_SIZE, SEEK_SET);
	 int32_t free_blk;
	 int found_free = 0;
	 int temp_fat_entry;
	 for(free_blk=0; free_blk < temp.num_blocks; ++free_blk){
       read(fd, &temp_fat_entry, sizeof(int32_t));
		 if(temp_fat_entry == 0){
			 found_free = 1;
          break;
		 }
	 }
	 if(!found_free){
	    return -ENOSPC;
	 } 
    add_dir_entry(fd, suffix_path, prefix_block, free_blk, 0);

    return 0;
}

static int fat_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
	       	off_t offset, struct fuse_file_info *fi){
    //get root block to find where current block is from path
    lseek(fd, 0, SEEK_SET);
    superblock temp;
    read(fd, &temp, sizeof(temp));
    int root_block = temp.root_start;

    directory temp_dir;
    
    //get the current block of the path
    int directory_block = find_path(path, root_block, &temp_dir);
    if(directory_block == -1) {
    	return -ENOENT;
    }

    // Need to check if the path was a directory or a file.
	 if(!temp_dir.flags){
       return -ENOTDIR;
	 }
    int32_t next_block;   
    while(1){
	    //seek to the data block of that block
	    lseek(fd, BLK_SIZE, SEEK_SET);
	    lseek(fd, directory_block*4, SEEK_CUR);
	    read(fd, &next_block, sizeof(int32_t));

	    lseek(fd, BLK_SIZE*directory_block, SEEK_SET);
	    //iterate through block and print out the fname
	    int directory_slot = 0;
	    for(directory_slot = 0; directory_slot < 64; ++directory_slot){
			read(fd, &temp_dir, sizeof(directory));
			if(temp_dir.start_blk != 0){
			    char *fname = temp_dir.fname; 
			    filler(buf, fname, NULL, 0);	   
			}
		 }
	    if(next_block == -2){
		    break;
	    }else{
		    directory_block = next_block;
	    }
    }
    return 0;    
}

static int fat_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
   //Find location of root dir
	lseek(fd, 0, SEEK_SET);
   superblock temp;
   read(fd, &temp, sizeof(temp));
   int root_block = temp.root_start;     
   directory temp_dir;
   // If the file doesn't exist or is a directory then you can't write to it.
	int file_loc = find_path(path, root_block, &temp_dir);
	int current_length = temp_dir.len;
   if(file_loc == -1 || temp_dir.flags){
      return -ENOENT;
   }
	if(temp_dir.flags){
      return -EISDIR;
	}

	//need to check if the offset is > the file size, in which case we will return
	if(offset > temp_dir.len){
      return -ESPIPE;
	}     

   ///////***********parsing***************///////////
	char str[strlen(path)];
	strcpy(str, path);
	int numberOfSlash = 0;
	int has_slash = 0;
	for(unsigned int i = 0; i < strlen(str); ++i){
		if(str[i] == '/'){
			++has_slash;
			++numberOfSlash;
		}
	}
	int index = 0;
	while(numberOfSlash > 0){
		if(str[index] == '/'){
			--numberOfSlash;
		}
		++index;
	}
   int array_length;
   char temp_prefix_path[index-1];
	if(has_slash ==0 || has_slash == 1){
		array_length = 1;
	}
	else{
		array_length = index - 1;
		for(int j = 0; j < index-1; ++j){
			temp_prefix_path[j] = str[j];
		}
		temp_prefix_path[index -1] = '\0';
	}
	char prefix_path[array_length];
	if(array_length == 1){
      strcpy(prefix_path, "/");
	}
	else{
      strcpy(prefix_path, temp_prefix_path);
	}
	int dirLength = strlen(str)-index;
	char suffix_path[dirLength];
	int dirIndex = 0;
	for(int k = index; k < strlen(str); ++k){
		suffix_path[dirIndex] = str[k];
		++dirIndex;
	}
	suffix_path[dirIndex] = '\0';
	///////////////////////////////////////////////////

	// Go to the file's FAT entry
	int32_t file_entry;
	int32_t file_index;
	int file_fat_count = 0;
	lseek(fd, BLK_SIZE, SEEK_SET);
   lseek(fd, file_loc*4, SEEK_CUR);
	while(1) {
		read(fd, &file_entry, sizeof(int32_t));
		++file_fat_count;
		if(file_entry == -2) {
			break;
		}
		else {
         file_index = file_entry;
			lseek(fd, BLK_SIZE, SEEK_SET);
			lseek(fd, file_index*4, SEEK_CUR);
		}
	}
   
	// check to see if more blocks need to be allocated to fit the write data
	if(offset + size > BLK_SIZE*file_fat_count){
      int bytes_needed = (offset+size) - BLK_SIZE*file_fat_count;
		int blocks_needed = (bytes_needed/BLK_SIZE);
		if(bytes_needed%BLK_SIZE){
         ++blocks_needed;
		}
		// now allocate new blocks to file
		for(int i = 0; i < blocks_needed; ++i){
         lseek(fd, BLK_SIZE, SEEK_SET);
			int32_t free_blk;
			int32_t temp_blk;
			int found_free = 0;
			for(free_blk = 0; free_blk < temp.num_blocks; ++free_blk){
            read(fd, &temp_blk, sizeof(int32_t));
				// found a free spot so, extend the block chain	
				if(temp_blk == 0){
					found_free = 1;
               lseek(fd, BLK_SIZE, SEEK_SET);
					lseek(fd, file_loc*4, SEEK_CUR);
					int32_t last_blk = file_loc;
					while(1){
                  read(fd, &temp_blk, sizeof(int32_t));
						if(temp_blk == -2){
                     lseek(fd, BLK_SIZE, SEEK_SET);
							lseek(fd, last_blk*4, SEEK_CUR);
							write(fd, &free_blk, sizeof(int32_t));
	                  int32_t neg_two = -2;
						   lseek(fd, BLK_SIZE, SEEK_SET);
							lseek(fd, free_blk*4, SEEK_CUR);
							write(fd, &neg_two, sizeof(int32_t));
							break;
						}
						else{
                     last_blk = temp_blk;
							lseek(fd, BLK_SIZE, SEEK_SET);
							lseek(fd, last_blk*4, SEEK_CUR);
						}
					}
				}
			}
			if(!found_free){
				return -ENOSPC;
			}
		}

	}      
   ///////////////////////////////////////////////////
   
	// count number of blocks so we can cache FAT in an array
	file_fat_count = 0;
	lseek(fd, BLK_SIZE, SEEK_SET);
   lseek(fd, file_loc*4, SEEK_CUR);
	while(1) {
		read(fd, &file_entry, sizeof(int32_t));
		++file_fat_count;
		if(file_entry == -2) {
			break;
		}
		else {
         file_index = file_entry;
			lseek(fd, BLK_SIZE, SEEK_SET);
			lseek(fd, file_index*4, SEEK_CUR);
		}
	}


	// create an array holding all of the FAT indices for the file
	int32_t fat_array[file_fat_count];
	lseek(fd, BLK_SIZE, SEEK_SET);
	lseek(fd, file_loc*4, SEEK_CUR);
	fat_array[0] = file_loc;
	for(int i = 1; i < file_fat_count; ++i) {
		read(fd, &file_entry, sizeof(int32_t));
		fat_array[i] = file_entry;
		file_index = file_entry;
		lseek(fd, BLK_SIZE, SEEK_SET);
		lseek(fd, file_index*4, SEEK_CUR);
	}
   
	// need to figure out where to start writing based on the offset
   int write_block = offset/BLK_SIZE;
	if(offset%BLK_SIZE == 0 && offset != 0){
      ++write_block;
	}
	int write_loc = offset - (write_block*BLK_SIZE);
   //begin writing buffer to disk
	int bytes_to_write = size;
	int bytes_written = 0;
	int first_write = 1;

	while(bytes_to_write > 0){
      if(first_write){
         lseek(fd, (fat_array[write_block]*BLK_SIZE)+write_loc, SEEK_SET);
			if(size < (BLK_SIZE - write_loc)){
            write(fd, buf, size);
				bytes_written = size;
				bytes_to_write -= bytes_written;
				break;
			}
			else{
			   write(fd, buf, (BLK_SIZE-write_loc));
				bytes_written = BLK_SIZE - write_loc;
				bytes_to_write -= bytes_written;
				++write_block;
		   }
		   first_write = 0;
	   }
		else{
         lseek(fd, fat_array[write_block]*BLK_SIZE, SEEK_SET);
			if(bytes_to_write < BLK_SIZE){
            write(fd, (buf+(sizeof(char)*bytes_written)), bytes_to_write);
				bytes_written += bytes_to_write;
				bytes_to_write -= bytes_written;
				break;
			}
			else{
            write(fd, (buf+(sizeof(char)*bytes_written)), BLK_SIZE);
				++write_block;
				bytes_written += BLK_SIZE;
				bytes_to_write -= BLK_SIZE;
			}		
		}
	}
   

	// update len of diretory entries

	// update . entry
	int dir_blk = find_path(prefix_path, temp.root_start, &temp_dir);
	lseek(fd, temp_dir.start_blk*BLK_SIZE, SEEK_SET);
	read(fd, &temp_dir, sizeof(directory));
	if(offset + bytes_written <= current_length) {
		return bytes_written;
	}
	else{
		temp_dir.len = temp_dir.len - offset + bytes_written;	
		lseek(fd, temp_dir.start_blk*BLK_SIZE, SEEK_SET);
		write(fd, &temp_dir, sizeof(directory));
	}

   // update file entry's len in its parent's directory
	int32_t actual_blk;
	int entry_in_dir = find_dir_entry(fd, dir_blk, suffix_path, &actual_blk);
	lseek(fd, actual_blk*BLK_SIZE, SEEK_SET);
	lseek(fd, entry_in_dir*sizeof(directory), SEEK_CUR);
	read(fd, &temp_dir, sizeof(directory));
	temp_dir.len = temp_dir.len - offset + bytes_written;
	lseek(fd, actual_blk*BLK_SIZE, SEEK_SET);
	lseek(fd, entry_in_dir*sizeof(directory), SEEK_CUR);
	write(fd, &temp_dir, sizeof(directory));

   // update grandparent's suffix_path entry's len
   lseek(fd, dir_blk*BLK_SIZE, SEEK_SET);
	lseek(fd, sizeof(directory), SEEK_CUR);
	read(fd, &temp_dir, sizeof(directory));
	lseek(fd, temp_dir.start_blk*BLK_SIZE, SEEK_SET);

  ///////***********parsing***************///////////
   char str_2[strlen(prefix_path)];
	strcpy(str_2, prefix_path);
	numberOfSlash = 0;
	has_slash = 0;
	for(unsigned int i = 0; i < strlen(str_2); ++i){
		if(str_2[i] == '/'){
			++has_slash;
			++numberOfSlash;
		}
	}
	index = 0;
	while(numberOfSlash > 0){
		if(str_2[index] == '/'){
			--numberOfSlash;
		}
		++index;
	}
   int array_length_2;
   char temp_prefix_path_2[index-1];
	if(has_slash ==0 || has_slash == 1){
		array_length_2 = 1;
	}
	else{
		array_length_2 = index - 1;
		for(int j = 0; j < index-1; ++j){
			temp_prefix_path_2[j] = str_2[j];
		}
		temp_prefix_path_2[index -1] = '\0';
	}
	char prefix_path_2[array_length_2];
	if(array_length_2 == 1){
      strcpy(prefix_path_2, "/");
	}
	else{
      strcpy(prefix_path_2, temp_prefix_path_2);
	}
	dirLength = strlen(str_2)-index;
	char suffix_path_2[dirLength];
	dirIndex = 0;
	for(int k = index; k < strlen(str_2); ++k){
		suffix_path_2[dirIndex] = str_2[k];
		++dirIndex;
	}
	suffix_path_2[dirIndex] = '\0';
	///////////////////////////////////////////////////

   entry_in_dir = find_dir_entry(fd, temp_dir.start_blk, suffix_path_2, &actual_blk);
   lseek(fd, actual_blk*BLK_SIZE, SEEK_SET);
	lseek(fd, entry_in_dir*sizeof(directory), SEEK_CUR);
	read(fd, &temp_dir, sizeof(directory));
	temp_dir.len =temp_dir.len - offset + bytes_written;
   lseek(fd, actual_blk*BLK_SIZE, SEEK_SET);
	lseek(fd, entry_in_dir*sizeof(directory), SEEK_CUR);
	write(fd, &temp_dir, sizeof(directory));

	return bytes_written;
}

static int fat_truncate(const char *path, off_t size){
   return 0;
}

static int fat_utimens(const char *path, const struct timespec ts[2]){
   return 0;
}

static int fat_chown(const char *path, uid_t uid, gid_t gid){
   return 0;
}

static int fat_open(const char* path, struct fuse_file_info* fi){
   directory temp_dir;
	superblock sb;
	lseek(fd, 0, SEEK_SET);
	read(fd, &sb, sizeof(superblock));
	if(find_path(path, sb.root_start, &temp_dir) == -1){
      return -ENOENT;
	}
	return 0;

}

static int fat_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
	///////***********parsing***************///////////
	char str[strlen(path)];
	strcpy(str, path);
	int numberOfSlash = 0;
	int has_slash = 0;
	for(unsigned int i = 0; i < strlen(str); ++i){
		if(str[i] == '/'){
			++has_slash;
			++numberOfSlash;
		}
	}
	int index = 0;
	while(numberOfSlash > 0){
		if(str[index] == '/'){
			--numberOfSlash;
		}
		++index;
	}
   int array_length;
   char temp_prefix_path[index-1];
	if(has_slash ==0 || has_slash == 1){
		array_length = 1;
	}
	else{
		array_length = index - 1;
		for(int j = 0; j < index-1; ++j){
			temp_prefix_path[j] = str[j];
		}
		temp_prefix_path[index -1] = '\0';
	}
	char prefix_path[array_length];
	if(array_length == 1){
      strcpy(prefix_path, "/");
	}
	else{
      strcpy(prefix_path, temp_prefix_path);
	}
	int dirLength = strlen(str)-index;
	char suffix_path[dirLength];
	int dirIndex = 0;
	for(int k = index; k < strlen(str); ++k){
		suffix_path[dirIndex] = str[k];
		++dirIndex;
	}
	suffix_path[dirIndex] = '\0';
	///////////////////////////////////////////////////
    superblock sb;
	lseek(fd, 0, SEEK_SET);
	read(fd, &sb, sizeof(superblock));
	directory temp_dir;
	int32_t start_block = find_path(path, sb.root_start, &temp_dir);
	lseek(fd, start_block*BLK_SIZE, SEEK_SET);
	lseek(fd, sizeof(directory), SEEK_CUR);
	directory dotdot;
	read(fd, &dotdot, sizeof(directory));
	// if doesn't exist then return enoent
	if(start_block == -1) {
	    return -ENOENT;
	}
   // count number of blocks so we can cache FAT in an array
	int file_fat_count = 0;
	int32_t file_entry;
	int32_t file_index;
	lseek(fd, BLK_SIZE, SEEK_SET);
   lseek(fd, start_block*4, SEEK_CUR);
	while(1) {
		read(fd, &file_entry, sizeof(int32_t));
		++file_fat_count;
		if(file_entry == -2) {
			break;
		}
		else {
         file_index = file_entry;
			lseek(fd, BLK_SIZE, SEEK_SET);
			lseek(fd, file_index*4, SEEK_CUR);
		}
	}
	// create an array holding all of the FAT indices for the file
	int32_t fat_array[file_fat_count];
	lseek(fd, BLK_SIZE, SEEK_SET);
	lseek(fd, start_block*4, SEEK_CUR);
	fat_array[0] = start_block;
	for(int i = 1; i < file_fat_count; ++i) {
		read(fd, &file_entry, sizeof(int32_t));
		fat_array[i] = file_entry;
		file_index = file_entry;
		lseek(fd, BLK_SIZE, SEEK_SET);
		lseek(fd, file_index*4, SEEK_CUR);
	}

   // start reading the file into the buffer
	char read_byte;
	int byte_count = 0;
	// illegal seek
	if(offset > temp_dir.len) {
	   return -ESPIPE;
	}
	int current_block = offset / BLK_SIZE;
	if(offset % BLK_SIZE == 0 && current_block != 0) {
	   ++current_block;
	}
	int first_block = 1;
	int read_index;
	for(; current_block < file_fat_count; ++current_block) {
	   lseek(fd, (fat_array[current_block]*BLK_SIZE), SEEK_SET);
	   if(first_block) {
		   lseek(fd, (offset % BLK_SIZE), SEEK_CUR);
			read_index = offset;
		}
		else {
		   read_index = 0;
		}
		for(; read_index < 4096; ++read_index) {
		   read(fd, &read_byte, sizeof(char));
			++byte_count;
			if(read_byte == EOF || byte_count == size) {
			   lseek(fd,dotdot.start_blk*BLK_SIZE,SEEK_SET);
			   int32_t actual_block;
			   int32_t blk_slot = find_dir_entry(fd, dotdot.start_blk, suffix_path, &actual_block);
			   lseek(fd, actual_block*BLK_SIZE, SEEK_SET);
			   lseek(fd, blk_slot*sizeof(directory), SEEK_CUR);
			   directory update_dir;
			   read(fd, &update_dir, sizeof(directory));
			   update_dir.access_time = (uint64_t)(time(NULL));
			   lseek(fd, actual_block*BLK_SIZE, SEEK_SET);
			   lseek(fd, blk_slot*sizeof(directory), SEEK_CUR);
			   write(fd, &update_dir, sizeof(directory));
			   return byte_count;
			}
			buf[byte_count-1] = read_byte;
		}
	}
   return -EINVAL;
}

static int fat_rename(const char *from, const char *to){
   ///////***********parse from***************///////////
	char str_from[strlen(from)];
	strcpy(str_from, from);
	int numberOfSlash_from = 0;
	int has_slash_from = 0;
	for(unsigned int i = 0; i < strlen(str_from); ++i){
		if(str_from[i] == '/'){
			++has_slash_from;
			++numberOfSlash_from;
		}
	}
	int index_from = 0;
	while(numberOfSlash_from > 0){
		if(str_from[index_from] == '/'){
			--numberOfSlash_from;
		}
		++index_from;
	}
   int array_length_from;
   char temp_prefix_path_from[index_from-1];
	if(has_slash_from ==0 || has_slash_from == 1){
		array_length_from = 1;
	}
	else{
		array_length_from = index_from - 1;
		for(int j = 0; j < index_from-1; ++j){
			temp_prefix_path_from[j] = str_from[j];
		}
		temp_prefix_path_from[index_from -1] = '\0';
	}
	char prefix_path_from[array_length_from];
	if(array_length_from == 1){
      strcpy(prefix_path_from, "/");
	}
	else{
      strcpy(prefix_path_from, temp_prefix_path_from);
	}
	int dirLength_from = strlen(str_from)-index_from;
	char suffix_path_from[dirLength_from];
	int dirIndex_from = 0;
	for(int k = index_from; k < strlen(str_from); ++k){
		suffix_path_from[dirIndex_from] = str_from[k];
		++dirIndex_from;
	}
	suffix_path_from[dirIndex_from] = '\0';
	///////////////////////////////////////////////////

   ///////***********parse to***************///////////
	char str_to[strlen(to)];
	strcpy(str_to, to);
	int numberOfSlash_to = 0;
	int has_slash_to = 0;
	for(unsigned int i = 0; i < strlen(str_to); ++i){
		if(str_to[i] == '/'){
			++has_slash_to;
			++numberOfSlash_to;
		}
	}
	int index_to = 0;
	while(numberOfSlash_to > 0){
		if(str_to[index_to] == '/'){
			--numberOfSlash_to;
		}
		++index_to;
	}
   int array_length_to;
   char temp_prefix_path_to[index_to-1];
	if(has_slash_to ==0 || has_slash_to == 1){
		array_length_to = 1;
	}
	else{
		array_length_to = index_to - 1;
		for(int j = 0; j < index_to-1; ++j){
			temp_prefix_path_to[j] = str_to[j];
		}
		temp_prefix_path_to[index_to-1] = '\0';
	}
	char prefix_path_to[array_length_to];
	if(array_length_to == 1){
      strcpy(prefix_path_to, "/");
	}
	else{
      strcpy(prefix_path_to, temp_prefix_path_to);
	}
	int dirLength_to = strlen(str_to)-index_to;
	char suffix_path_to[dirLength_to];
	int dirIndex_to = 0;
	for(int k = index_to; k < strlen(str_to); ++k){
		suffix_path_to[dirIndex_to] = str_to[k];
		++dirIndex_to;
	}
	suffix_path_to[dirIndex_to] = '\0';
   ///////////////////////////////////////////////////////////

   superblock sb_temp;
	lseek(fd, 0, SEEK_SET);
	read(fd, &sb_temp, sizeof(superblock));
	directory from_dir_entry;
	int32_t from_loc = find_path(from, sb_temp.root_start, &from_dir_entry);
	printf("from_loc: %d\n", from_loc);

   if(from_loc == -1){
	   return -ENOENT;
	}

	directory to_dir_entry;
	//int32_t to_loc = find_path(to, sb_temp.root_start, &to_dir_entry);

	int32_t to_loc = find_path(prefix_path_to, sb_temp.root_start, &to_dir_entry);

   if(to_loc == -1){
      //move the file
	}else{
      //replace the file
		int32_t entry_block;
		printf("to_loc: %d\n", to_loc);
		printf("suffix_path_from: %s\n", suffix_path_from);
		//int entry_slot = find_dir_entry(fd, to_loc, suffix_path_from, &entry_block);
		int entry_slot = find_dir_entry(fd, to_loc, suffix_path_from, &entry_block);
		printf("entry slot: %d\n", entry_slot);
		printf("entry_block: %d\n", entry_block);

      lseek(fd, entry_block*BLK_SIZE, SEEK_SET);
		lseek(fd, entry_slot*sizeof(directory), SEEK_CUR);

		directory check_dir;
		read(fd, &check_dir, sizeof(directory));
		//If it is a directory, do nothing
		if(check_dir.flags == 1) {
			return -EISDIR; 
		}
		//It is just a file, update the directory's "." entry
      
		printf("entry slot is: %d\n", entry_block);
		lseek(fd, entry_block*BLK_SIZE, SEEK_SET);
		printf("entry slot after: %d\n", entry_block);
		directory dotentry;
		read(fd, &dotentry, sizeof(directory));
		printf("dot entry name: %s\n", dotentry.fname);
		printf("dot entry len before: %d\n", dotentry.len);
		dotentry.len = dotentry.len - to_dir_entry.len + from_dir_entry.len;
		printf("dot entry len: %d\n", dotentry.len);
		dotentry.access_time = (uint64_t)(time(NULL));
		dotentry.mod_time = (uint64_t)(time(NULL));
		lseek(fd, entry_block*BLK_SIZE, SEEK_SET);
		write(fd, &dotentry, sizeof(directory));

		//Replace the actual file
		lseek(fd, entry_block*BLK_SIZE, SEEK_SET);
		lseek(fd, entry_slot*sizeof(directory), SEEK_CUR);
		from_dir_entry.mod_time = (uint64_t)(time(NULL));
		write(fd, &from_dir_entry, sizeof(directory));
		lseek(fd, check_dir.start_blk*BLK_SIZE, SEEK_SET);
		int32_t remove_from_entry;
		directory remove_from_directory;
		/////////////////////////////////////////////////
		directory a;
		int prefix_path_from_sb = find_path(prefix_path_from, sb_temp.root_start, &a);
		int remove_from = find_dir_entry(fd, prefix_path_from_sb, suffix_path_from, &remove_from_entry);
		printf("remove_from: %d\n", remove_from_entry);
		printf("remove_from_entry: %d\n", remove_from);
		lseek(fd, BLK_SIZE*remove_from_entry, SEEK_SET);
		lseek(fd, remove_from*sizeof(directory), SEEK_CUR);
		read(fd, &remove_from_directory, sizeof(directory));

		remove_from_directory.start_blk = 0;
		lseek(fd, BLK_SIZE*remove_from_entry, SEEK_SET);
		lseek(fd, remove_from*sizeof(directory), SEEK_CUR);
		write(fd, &remove_from_directory, sizeof(directory));
	
	}

   return 0;
}

//Remove an empty directory. If the directory has stuff in it, then return an error
static int fat_rmdir(const char *path){
	lseek(fd, 0, SEEK_SET);
	superblock sb_temp;
	read(fd, &sb_temp, sizeof(superblock));
	directory dir_path;
	int32_t path_exists = find_path(path, sb_temp.root_start, &dir_path);
	if(path_exists == -1) {
		return -ENOENT;
	}
	
	///////***********parsing***************///////////
	char str[strlen(path)];
	strcpy(str, path);
	int numberOfSlash = 0;
	int has_slash = 0;
	for(unsigned int i = 0; i < strlen(str); ++i){
		if(str[i] == '/'){
			++has_slash;
			++numberOfSlash;
		}
	}
	int index = 0;
	while(numberOfSlash > 0){
		if(str[index] == '/'){
			--numberOfSlash;
		}
		++index;
	}
   int array_length;
   char temp_prefix_path[index-1];
	if(has_slash ==0 || has_slash == 1){
		array_length = 1;
	}
	else{
		array_length = index - 1;
		for(int j = 0; j < index-1; ++j){
			temp_prefix_path[j] = str[j];
		}
		temp_prefix_path[index -1] = '\0';
	}
	char prefix_path[array_length];
	if(array_length == 1){
      strcpy(prefix_path, "/");
	}
	else{
      strcpy(prefix_path, temp_prefix_path);
	}
	int dirLength = strlen(str)-index;
	char suffix_path[dirLength];
	int dirIndex = 0;
	for(int k = index; k < strlen(str); ++k){
		suffix_path[dirIndex] = str[k];
		++dirIndex;
	}
	suffix_path[dirIndex] = '\0';
	///////////////////////////////////////////////////
	//Go to the directory's parent block and set its start block to 0
	directory parent_dir;
	int32_t prefix_block = find_path(prefix_path, sb_temp.root_start, &parent_dir);

    int32_t actual_block;
    int32_t the_entry = find_dir_entry(fd, prefix_block, suffix_path, &actual_block);

    lseek(fd, actual_block*BLK_SIZE, SEEK_SET);
    lseek(fd, the_entry*sizeof(directory), SEEK_CUR);
    directory the_dir;
    read(fd, &the_dir, sizeof(directory)); 

	if(the_dir.len > 128) {
		return -ENOTEMPTY;
	}
	int32_t child_block;
	int32_t child_slot = find_dir_entry(fd, prefix_block, suffix_path, &child_block);
	lseek(fd, child_block*BLK_SIZE, SEEK_SET);
	lseek(fd, child_slot*sizeof(directory), SEEK_CUR);
	directory child_dir;
	read(fd, &child_dir, sizeof(directory));
	child_dir.start_blk = 0;
	lseek(fd, child_block*BLK_SIZE, SEEK_SET);
	lseek(fd, child_slot*sizeof(directory), SEEK_CUR);
	write(fd, &child_dir, sizeof(directory));
	//Go to directory's FAT and set it to 0
	lseek(fd, BLK_SIZE, SEEK_SET);
	lseek(fd, path_exists*4, SEEK_CUR);
	int32_t zero = 0;
	write(fd, &zero, sizeof(int32_t));
	//Clear the diretory's data block
	lseek(fd, path_exists*BLK_SIZE, SEEK_SET);
	directory clear_entries;
	read(fd, &clear_entries, sizeof(directory));
	lseek(fd, path_exists*BLK_SIZE, SEEK_SET);
	for(int i = 0; i < 2; ++i) {
		clear_entries.start_blk = 0;
		write(fd, &clear_entries, sizeof(directory));
		lseek(fd, path_exists*BLK_SIZE, SEEK_SET);
		lseek(fd, i*sizeof(directory), SEEK_CUR);
		read(fd, &clear_entries, sizeof(directory));
	}
	return 0;
}

static struct fuse_operations opers = {
	.getattr		        = fat_getattr,
	.mkdir                  = fat_mkdir,
	.readdir                = fat_readdir,
	.create                 = fat_create,
	.unlink                 = fat_unlink,
	.write					= fat_write,
	.truncate               = fat_truncate,
	.utimens                = fat_utimens,
	.chown                  = fat_chown,
	.open                   = fat_open,
	.read                   = fat_read,
	.rename                 = fat_rename,
	.rmdir                  = fat_rmdir,
};

int main(int argc, char** argv) {
   // clean file by overwriting everything to 0
	int size = get_disksize(diskName);
	fd = open(diskName, O_RDWR);
	lseek(fd, 0, SEEK_SET);
	char empty[BLK_SIZE];
	for(int i = 0; i < 4096; ++i){
       empty[i] = 0;
	}
	for(int i = 0; i < size/BLK_SIZE; ++i){
		write(fd, empty, sizeof(empty));
	}
	/******************************************/
////
   //initializing file system
   superblock_init(size, fd);
   fat_init(fd);
   root_init(fd);

	return fuse_main(argc, argv, &opers, NULL);
}