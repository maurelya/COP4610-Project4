#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "LibDisk.h"
#include "LibFS.h"
#include <ctype.h>

/*
By:
  Maurely Acosta (PID: 3914479)
  Tomas Ortega (PID: 5677483)
*/

// set to 1 to have detailed debug print-outs and 0 to have none
#define FSDEBUG 0

#if FSDEBUG
#define dprintf printf
#else
#define dprintf noprintf
void noprintf(char* str, ...) {}
#endif

// the file system partitions the disk into five parts:

// 1. the superblock (one sector), which contains a magic number at
// its first four bytes (integer)
#define SUPERBLOCK_START_SECTOR 0

// the magic number chosen for our file system
#define OS_MAGIC 0xdeadbeef

// 2. the inode bitmap (one or more sectors), which indicates whether
// the particular entry in the inode table (#4) is currently in use
#define INODE_BITMAP_START_SECTOR 1

// the total number of bytes and sectors needed for the inode bitmap;
// we use one bit for each inode (whether it's a file or directory) to
// indicate whether the particular inode in the inode table is in use
#define INODE_BITMAP_SIZE ((MAX_FILES+7)/8)
#define INODE_BITMAP_SECTORS ((INODE_BITMAP_SIZE+SECTOR_SIZE-1)/SECTOR_SIZE)

// 3. the sector bitmap (one or more sectors), which indicates whether
// the particular sector in the disk is currently in use
#define SECTOR_BITMAP_START_SECTOR (INODE_BITMAP_START_SECTOR+INODE_BITMAP_SECTORS)

// the total number of bytes and sectors needed for the data block
// bitmap (we call it the sector bitmap); we use one bit for each
// sector of the disk to indicate whether the sector is in use or not
#define SECTOR_BITMAP_SIZE ((TOTAL_SECTORS+7)/8)
#define SECTOR_BITMAP_SECTORS ((SECTOR_BITMAP_SIZE+SECTOR_SIZE-1)/SECTOR_SIZE)

// 4. the inode table (one or more sectors), which contains the inodes
// stored consecutively
#define INODE_TABLE_START_SECTOR (SECTOR_BITMAP_START_SECTOR+SECTOR_BITMAP_SECTORS)

// an inode is used to represent each file or directory; the data
// structure supposedly contains all necessary information about the
// corresponding file or directory
typedef struct _inode {
  int size; // the size of the file or number of directory entries
  int type; // 0 means regular file; 1 means directory
  int data[MAX_SECTORS_PER_FILE]; // indices to sectors containing data blocks
} inode_t;

// the inode structures are stored consecutively and yet they don't
// straddle accross the sector boundaries; that is, there may be
// fragmentation towards the end of each sector used by the inode
// table; each entry of the inode table is an inode structure; there
// are as many entries in the table as the number of files allowed in
// the system; the inode bitmap (#2) indicates whether the entries are
// current in use or not
#define INODES_PER_SECTOR (SECTOR_SIZE/sizeof(inode_t))
#define INODE_TABLE_SECTORS ((MAX_FILES+INODES_PER_SECTOR-1)/INODES_PER_SECTOR)

// 5. the data blocks; all the rest sectors are reserved for data
// blocks for the content of files and directories
#define DATABLOCK_START_SECTOR (INODE_TABLE_START_SECTOR+INODE_TABLE_SECTORS)

// other file related definitions

// max length of a path is 256 bytes (including the ending null)
#define MAX_PATH 256

// max length of a filename is 16 bytes (including the ending null)
#define MAX_NAME 16

// max number of open files is 256
#define MAX_OPEN_FILES 256

// each directory entry represents a file/directory in the parent
// directory, and consists of a file/directory name (less than 16
// bytes) and an integer inode number
typedef struct _dirent {
  char fname[MAX_NAME]; // name of the file
  int inode; // inode of the file
} dirent_t;

// the number of directory entries that can be contained in a sector
#define DIRENTS_PER_SECTOR (SECTOR_SIZE/sizeof(dirent_t))

// global errno value here
int osErrno;

// the name of the disk backstore file (with which the file system is booted)
static char bs_filename[1024];

/* the following functions are internal helper functions */

int signum(int n) {
  if (n == 0) {
    return(0);
  }else if (n > 0) {
    return(1);
  }else {
    return(-1);
  }
}

// check magic number in the superblock; return 1 if OK, and 0 if not
static int check_magic()
{
  char buf[SECTOR_SIZE];
  if(Disk_Read(SUPERBLOCK_START_SECTOR, buf) < 0)
    return 0;
  if(*(int*)buf == OS_MAGIC) return 1;
  else return 0;
}

// initialize a bitmap with 'num' sectors starting from 'start'
// sector; all bits should be set to zero except that the first
// 'nbits' number of bits are set to one
static void bitmap_init(int start, int num, int nbits)
{
  /* YOUR CODE  - Maurely Acosta*/
  dprintf("Creating a bitmap starting at sector %d, %d sectors long, %d bits are set to one\n", start, num, nbits);

  char bitmap_buf[SECTOR_SIZE];    //chars are size 1

  int sectors_set_to_one = nbits / 8 / SECTOR_SIZE; //number of sectors that are all 1
  int remaining_bytes = nbits / 8 % SECTOR_SIZE; //number of bytes on first sector after sectors_set_to_one
  int sectors_set_to_zero = num - sectors_set_to_one - signum(remaining_bytes); //number of sectors that are all 0

  //all full sectors
  for (int i = 0; i < SECTOR_SIZE; i++) {
    bitmap_buf[i] = 0xff;
  }
  for (int i = start; i < start + sectors_set_to_one; i++) {
    Disk_Write(i, bitmap_buf);
  }

  unsigned char bits[8] = { 0x0, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE };
  //partial sector
  int remaining = nbits % 8;
  dprintf("Writting partial byte %x\n", bits[remaining]);

  bitmap_buf[remaining_bytes] = bits[remaining];
  for (int i = remaining_bytes + 1; i < SECTOR_SIZE; i++) {
    bitmap_buf[i] = 0;
  }
  Disk_Write(start + sectors_set_to_one, bitmap_buf);


  // Write sectors to 0
  for (int i = 0; i < remaining_bytes; i++) {
    bitmap_buf[i] = 0;
  }
  int end_of_sector = start + sectors_set_to_one + 1 + sectors_set_to_zero;
  for (int i = start + sectors_set_to_one + 1; i < end_of_sector; i++) {
    Disk_Write(i, bitmap_buf);
  }

}

/*find if a specific bit is set inside a byte
Return 1 if the bit is set in this position or 0 otherwise
*/
static int isBitSet (unsigned char c, int n) {
    static unsigned char mask[] = {128, 64, 32, 16, 8, 4, 2, 1};
    return ((c & mask[n]) != 0);
}

/* Set a specific bit inside a byte
Return the byte after the bit was set at position n
*/
static char setBit (unsigned char c, int n) {
    static unsigned char mask[] = {128, 64, 32, 16, 8, 4, 2, 1};
    return (c | mask[n]);
}

// set the first unused bit from a bitmap of 'nbits' bits (flip the
// first zero appeared in the bitmap to one) and return its location;
// return -1 if the bitmap is already full (no more zeros)
static int bitmap_first_unused(int start, int num, int nbits)
{
  int bytes_set_to_one = nbits / 8;           // Number of bytes to set to 1
  int remaining_bits = nbits % 8;             // Remainder bits after the last byte set to 1
  int last_block = 0;                        // am I in the last byte?
  char bitmap_buf[SECTOR_SIZE];              //Buffer sector
  int location = -1;

  int i;
  for(i = start; i < (start + num); i++){     //  Check all bytes of each sector

    if(bytes_set_to_one < SECTOR_SIZE){ //if the last sector
      last_block = 1;
    }

    if(Disk_Read(i, bitmap_buf) < 0){ // Read the sector
      dprintf("Oops, failed reading the block %d\n" , i);
      osErrno = E_GENERAL;
      return -1;
    }

    int a_byte;
    int bit;
    int limit;

    if(last_block == 1){ // Is this the last sector?
      limit = bytes_set_to_one;
    }else{ //Else check more than one sector
      limit = SECTOR_SIZE;
    }

    for(a_byte = 0; a_byte < limit; a_byte++){ //Check each byte in each sector
      for(bit = 0; bit < 8; bit++){  //Checking each bit inside this byte
        location++;
        if(!isBitSet(bitmap_buf[a_byte], bit)){

          bitmap_buf[a_byte] = setBit(bitmap_buf[a_byte], bit);

          if(Disk_Write(i, bitmap_buf) < 0) { //Write the sector back
            dprintf("Oops, failed writting the block %d\n" , i);
            osErrno = E_GENERAL;
            return -1;
          }
          return location;
        }

      }
    }


    //Do a last check
    //check the last bits in the last incompleted byte
    if(last_block == 1){
      for(bit = 0; bit < remaining_bits ; bit++){  //Check the remaining bits in this byte
        location ++;
        if(!isBitSet(bitmap_buf[a_byte], bit)){
           bitmap_buf[a_byte] = setBit(bitmap_buf[a_byte], bit); //Set this bit

          if(Disk_Write(i, bitmap_buf) < 0) { //Write the sector back
            dprintf("Oops, failed writting the block %d\n" , i);
            osErrno = E_GENERAL;
            return -1;
          }
          return location;
        }
      }
    }

  }
  return -1;
}

// reset the i-th bit of a bitmap with 'num' sectors starting from
// 'start' sector; return 0 if successful, -1 otherwise
static int bitmap_reset(int start, int num, int ibit)
{
  /* YOUR CODE  Maurely Acosta*/
  int number_bytes = ibit/8; // completed bytes before the one containing the reset bit
  int remaining_bits = ibit % 8; // location of the bit to reset within the last byte
  char bitmap_buf[SECTOR_SIZE];

    if(number_bytes > SECTOR_SIZE*num){
      //incorrect number of ibit because greater than the sector size.
      dprintf("... Error: The ibit=%d passed to reset is too large for the sector \n" , ibit);
      return -1;
    }

    while(number_bytes > SECTOR_SIZE){
      number_bytes = number_bytes - SECTOR_SIZE; //Go to the last sector
    }

    if(Disk_Read(start, bitmap_buf) < 0){
      dprintf("Error: failed reading the block %d\n" , start);
      osErrno = E_GENERAL;
      return -1;
    }

    static unsigned char mask[] = {127, 191, 223, 239, 247, 251, 253, 254};
    bitmap_buf[number_bytes] = (bitmap_buf[number_bytes] & mask[remaining_bits]);

    if(Disk_Write(start, bitmap_buf) < 0) {
            dprintf("Error: failed writing the block %d\n" , start);
            osErrno = E_GENERAL;
            return -1;
          }

  return 0;
}

// return 1 if the file name is illegal; otherwise, return 0; legal
// characters for a file name include letters (case sensitive),
// numbers, dots, dashes, and underscores; and a legal file name
// should not be more than MAX_NAME-1 in length
static int illegal_filename(char* name)
{
  /* YOUR CODE Maurely Acosta*/
  if(strlen(name) > MAX_NAME - 1){
    printf("ERROR: The file name is too long.\n");
    return 1;
  }else{
    int i;
    for(i = 0; i < strlen(name); i++){
      char current = name[i];
      if(!(isalpha(current) || isdigit(current) || current == '-' || current == '_' || current == '.'))
        return 1;
    }
  }
  return 0;
}

// return the child inode of the given file name 'fname' from the
// parent inode; the parent inode is currently stored in the segment
// of inode table in the cache (we cache only one disk sector for
// this); once found, both cached_inode_sector and cached_inode_buffer
// may be updated to point to the segment of inode table containing
// the child inode; the function returns -1 if no such file is found;
// it returns -2 is something else is wrong (such as parent is not
// directory, or there's read error, etc.)
static int find_child_inode(int parent_inode, char* fname,
			    int *cached_inode_sector, char* cached_inode_buffer)
{
  int cached_start_entry = ((*cached_inode_sector)-INODE_TABLE_START_SECTOR)*INODES_PER_SECTOR;
  int offset = parent_inode-cached_start_entry;
  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t* parent = (inode_t*)(cached_inode_buffer+offset*sizeof(inode_t));
  dprintf("... load parent inode: %d (size=%d, type=%d)\n",
	 parent_inode, parent->size, parent->type);
  if(parent->type != 1) {
    dprintf("... parent not a directory\n");
    return -2;
  }

  int nentries = parent->size; // remaining number of directory entries
  int idx = 0;
  while(nentries > 0) {
    char buf[SECTOR_SIZE]; // cached content of directory entries
    if(Disk_Read(parent->data[idx], buf) < 0) return -2;
    for(int i=0; i<DIRENTS_PER_SECTOR; i++) {
      if(i>nentries) break;
      if(!strcmp(((dirent_t*)buf)[i].fname, fname)) {
	// found the file/directory; update inode cache
	int child_inode = ((dirent_t*)buf)[i].inode;
	dprintf("... found child_inode=%d\n", child_inode);
	int sector = INODE_TABLE_START_SECTOR+child_inode/INODES_PER_SECTOR;
	if(sector != (*cached_inode_sector)) {
	  *cached_inode_sector = sector;
	  if(Disk_Read(sector, cached_inode_buffer) < 0) return -2;
	  dprintf("... load inode table for child\n");
	}
	return child_inode;
      }
    }
    idx++; nentries -= DIRENTS_PER_SECTOR;
  }
  dprintf("... could not find child inode\n");
  return -1; // not found
}

// follow the absolute path; if successful, return the inode of the
// parent directory immediately before the last file/directory in the
// path; for example, for '/a/b/c/d.txt', the parent is '/a/b/c' and
// the child is 'd.txt'; the child's inode is returned through the
// parameter 'last_inode' and its file name is returned through the
// parameter 'last_fname' (both are references); it's possible that
// the last file/directory is not in its parent directory, in which
// case, 'last_inode' points to -1; if the function returns -1, it
// means that we cannot follow the path
static int follow_path(char* path, int* last_inode, char* last_fname)
{
  if(!path) {
    dprintf("... invalid path\n");
    return -1;
  }
  if(path[0] != '/') {
    dprintf("... '%s' not absolute path\n", path);
    return -1;
  }

  // make a copy of the path (skip leading '/'); this is necessary
  // since the path is going to be modified by strsep()
  char pathstore[MAX_PATH];
  strncpy(pathstore, path+1, MAX_PATH-1);
  pathstore[MAX_PATH-1] = '\0'; // for safety
  char* lpath = pathstore;

  int parent_inode = -1, child_inode = 0; // start from root
  // cache the disk sector containing the root inode
  int cached_sector = INODE_TABLE_START_SECTOR;
  char cached_buffer[SECTOR_SIZE];
  if(Disk_Read(cached_sector, cached_buffer) < 0) return -1;
  dprintf("... load inode table for root from disk sector %d\n", cached_sector);

  // for each file/directory name separated by '/'
  char* token;
  while((token = strsep(&lpath, "/")) != NULL) {
    dprintf("... process token: '%s'\n", token);
    if(*token == '\0') continue; // multiple '/' ignored
    if(illegal_filename(token)) {
      dprintf("... illegal file name: '%s'\n", token);
      return -1;
    }
    if(child_inode < 0) {
      // regardless whether child_inode was not found previously, or
      // there was issues related to the parent (say, not a
      // directory), or there was a read error, we abort
      dprintf("... parent inode can't be established\n");
      return -1;
    }
    parent_inode = child_inode;
    child_inode = find_child_inode(parent_inode, token,
				   &cached_sector, cached_buffer);
    if(last_fname) strcpy(last_fname, token);
  }
  if(child_inode < -1) return -1; // if there was error, abort
  else {
    // there was no error, several possibilities:
    // 1) '/': parent = -1, child = 0
    // 2) '/valid-dirs.../last-valid-dir/not-found': parent=last-valid-dir, child=-1
    // 3) '/valid-dirs.../last-valid-dir/found: parent=last-valid-dir, child=found
    // in the first case, we set parent=child=0 as special case
    if(parent_inode==-1 && child_inode==0) parent_inode = 0;
    dprintf("... found parent_inode=%d, child_inode=%d\n", parent_inode, child_inode);
    *last_inode = child_inode;
    return parent_inode;
  }
}

// add a new file or directory (determined by 'type') of given name
// 'file' under parent directory represented by 'parent_inode'
int add_inode(int type, int parent_inode, char* file)
{
  // get a new inode for child
  int child_inode = bitmap_first_unused(INODE_BITMAP_START_SECTOR, INODE_BITMAP_SECTORS, INODE_BITMAP_SIZE);
  if(child_inode < 0) {
    dprintf("... error: inode table is full\n");
    return -1;
  }
  dprintf("... new child inode %d\n", child_inode);

  // load the disk sector containing the child inode
  int inode_sector = INODE_TABLE_START_SECTOR+child_inode/INODES_PER_SECTOR;
  char inode_buffer[SECTOR_SIZE];
  if(Disk_Read(inode_sector, inode_buffer) < 0) return -1;
  dprintf("... load inode table for child inode from disk sector %d\n", inode_sector);

  // get the child inode
  int inode_start_entry = (inode_sector-INODE_TABLE_START_SECTOR)*INODES_PER_SECTOR;
  int offset = child_inode-inode_start_entry;
  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t* child = (inode_t*)(inode_buffer+offset*sizeof(inode_t));

  // update the new child inode and write to disk
  memset(child, 0, sizeof(inode_t));
  child->type = type;
  if(Disk_Write(inode_sector, inode_buffer) < 0) return -1;
  dprintf("... update child inode %d (size=%d, type=%d), update disk sector %d\n",
	 child_inode, child->size, child->type, inode_sector);

  // get the disk sector containing the parent inode
  inode_sector = INODE_TABLE_START_SECTOR+parent_inode/INODES_PER_SECTOR;
  if(Disk_Read(inode_sector, inode_buffer) < 0) return -1;
  dprintf("... load inode table for parent inode %d from disk sector %d\n",
	 parent_inode, inode_sector);

  // get the parent inode
  inode_start_entry = (inode_sector-INODE_TABLE_START_SECTOR)*INODES_PER_SECTOR;
  offset = parent_inode-inode_start_entry;
  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t* parent = (inode_t*)(inode_buffer+offset*sizeof(inode_t));
  dprintf("... get parent inode %d (size=%d, type=%d)\n",
	 parent_inode, parent->size, parent->type);

  // get the dirent sector
  if(parent->type != 1) {
    dprintf("... error: parent inode is not directory\n");
    return -2; // parent not directory
  }
  int group = parent->size/DIRENTS_PER_SECTOR;
  char dirent_buffer[SECTOR_SIZE];
  if(group*DIRENTS_PER_SECTOR == parent->size) {
    // new disk sector is needed
    int newsec = bitmap_first_unused(SECTOR_BITMAP_START_SECTOR, SECTOR_BITMAP_SECTORS, SECTOR_BITMAP_SIZE);
    if(newsec < 0) {
      dprintf("... error: disk is full\n");
      return -1;
    }
    parent->data[group] = newsec;
    memset(dirent_buffer, 0, SECTOR_SIZE);
    dprintf("... new disk sector %d for dirent group %d\n", newsec, group);
  } else {
    if(Disk_Read(parent->data[group], dirent_buffer) < 0)
      return -1;
    dprintf("... load disk sector %d for dirent group %d\n", parent->data[group], group);
  }

  // add the dirent and write to disk
  int start_entry = group*DIRENTS_PER_SECTOR;
  offset = parent->size-start_entry;
  dirent_t* dirent = (dirent_t*)(dirent_buffer+offset*sizeof(dirent_t));
  strncpy(dirent->fname, file, MAX_NAME);
  dirent->inode = child_inode;
  if(Disk_Write(parent->data[group], dirent_buffer) < 0) return -1;
  dprintf("... append dirent %d (name='%s', inode=%d) to group %d, update disk sector %d\n",
	  parent->size, dirent->fname, dirent->inode, group, parent->data[group]);

  // update parent inode and write to disk
  parent->size++;
  if(Disk_Write(inode_sector, inode_buffer) < 0) return -1;
  dprintf("... update parent inode on disk sector %d\n", inode_sector);

  return 0;
}

// used by both File_Create() and Dir_Create(); type=0 is file, type=1
// is directory
int create_file_or_directory(int type, char* pathname)
{
  int child_inode;
  char last_fname[MAX_NAME];
  int parent_inode = follow_path(pathname, &child_inode, last_fname);
  if(parent_inode >= 0) {
    if(child_inode >= 0) {
      dprintf("... file/directory '%s' already exists, failed to create\n", pathname);
      osErrno = E_CREATE;
      return -1;
    } else {
      if(add_inode(type, parent_inode, last_fname) >= 0) {
	dprintf("... successfully created file/directory: '%s'\n", pathname);
	return 0;
      } else {
	dprintf("... error: something wrong with adding child inode\n");
	osErrno = E_CREATE;
	return -1;
      }
    }
  } else {
    dprintf("... error: something wrong with the file/path: '%s'\n", pathname);
    osErrno = E_CREATE;
    return -1;
  }
}

// remove the child from parent; the function is called by both
// File_Unlink() and Dir_Unlink(); the function returns 0 if success,
// -1 if general error, -2 if directory not empty, -3 if wrong type
int remove_inode(int type, int parent_inode, int child_inode)
{
  /* YOUR CODE */
  //load the child inode sector
  int inode_sector = INODE_TABLE_START_SECTOR + child_inode / INODES_PER_SECTOR;

  char inode_buffer[SECTOR_SIZE];
  if(Disk_Read(inode_sector, inode_buffer) < 0){
    return -1;
  }
  dprintf("Loading the inode table for child inode from disk sector %d\n", inode_sector);

  // get the child inode
  int inode_start_entry = (inode_sector - INODE_TABLE_START_SECTOR) * INODES_PER_SECTOR;

  int offset = child_inode - inode_start_entry;

  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t* child = (inode_t*)(inode_buffer + offset * sizeof(inode_t));

  //check the child inode for errors
  if(child->type != type){ //If the type don't match the child type
    return -3; //ERROR: wrong type
  }

  if(child->type == 1 && child->size > 0){ //Is the directory not empty?
    return -2; //ERROR: directory not empty,
  }

  //Reclaim the data sectors of the child inode if the inode is a file
  if(child->type == 0){ //If a directory is empty, delete it.
    int i;
    for(i = 0; i < MAX_SECTORS_PER_FILE; i++){ //Traverse all the sectors
        if(child->data[i] > 0){ //Is there valid data in this sector that we need to remove?
          bitmap_reset(SECTOR_BITMAP_START_SECTOR, SECTOR_BITMAP_SECTORS, child->data[i]);
          dprintf("Reseting the bit sector %d from the data index [%d] \n", child->data[i], i );
        }
      }
  }

  // Clear the child inode and write to disk
  memset(child, 0, sizeof(inode_t));

  if(Disk_Write(inode_sector, inode_buffer) < 0){
    return -1;
  }
  dprintf("Update the disk sector %d\n", inode_sector);
  bitmap_reset(INODE_BITMAP_START_SECTOR, INODE_BITMAP_SECTORS, child_inode);

  //Update the parent inode
  inode_sector = INODE_TABLE_START_SECTOR + parent_inode / INODES_PER_SECTOR;
  if(Disk_Read(inode_sector, inode_buffer) < 0){
    return -1;
  }
  dprintf("Load the inode table for the parent inode %d from disk the sector %d\n", parent_inode, inode_sector);

  // get the parent inode
  inode_start_entry = (inode_sector-INODE_TABLE_START_SECTOR) * INODES_PER_SECTOR;
  offset = parent_inode - inode_start_entry;
  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t* parent = (inode_t*)(inode_buffer + offset * sizeof(inode_t));
  dprintf("Getting the parent inode %d (size=%d, type=%d)\n", parent_inode, parent->size, parent->type);

  if(parent->type != 1) {
    dprintf("Error: The parent inode is not a directory\n");
    return -2;
  }

  //Find in the parent inode the dirent structure that contains the child inode
  //Then swap it with the last dirent entry in the parent inode and decrement the size
   char dirent_buffer[SECTOR_SIZE];
    int group = 0;
    int entry = 0;
    dirent_t* current_dirent;

  //Look for the last dirent entry in the parent inode
  if(parent->size > 1){ // if there are more files and directories in the parent inode
    int last_group = parent->size / DIRENTS_PER_SECTOR;
    char last_dirent_buffer[SECTOR_SIZE];
    int last_sector = parent->data[last_group];
    if(Disk_Read(last_sector, last_dirent_buffer) < 0){ //Read the sector used by the last entry
      return -1;
    }

    dprintf("Loading the disk sector %d corresponding to the last dirent entry in the group %d\n"
    , parent->data[last_group], group);

    //get the last dirent
    int start_entry = last_group * DIRENTS_PER_SECTOR;
    offset = parent->size - start_entry - 1;
    //last dirent to swap with the dirent that we are deleting
    dirent_t* last_dirent = (dirent_t*)(last_dirent_buffer + offset * sizeof(dirent_t));

    //Find the sector where the child dirent is
    for(group = 0; group<MAX_SECTORS_PER_FILE; group++){ //Iterate through all the groups in the parent inode
      if(Disk_Read(parent->data[group], dirent_buffer) < 0){ //Read the sector in this group
          return -1;
      }
      dprintf("Loading the disk sector %d for the dirent group %d\n", parent->data[last_group], group);
      for(entry = 0; entry<DIRENTS_PER_SECTOR; entry++){ //All the dirents inside this group
         current_dirent = (dirent_t*)(dirent_buffer+entry * sizeof(dirent_t));
         if(current_dirent->inode == child_inode){

            strncpy(current_dirent->fname, last_dirent->fname, MAX_NAME);
            current_dirent->inode = last_dirent->inode;

            char * temp = "";
            strncpy(last_dirent->fname, temp, MAX_NAME);
            last_dirent->inode = -1;

            memset(last_dirent, 0 , sizeof(dirent_t));
            //Update the sector with the removed node
            if(Disk_Write(parent->data[group], dirent_buffer) < 0){
              return -1;
            }
            dprintf("Updating the dirent %d (name='%s', inode=%d) to group %d, updating the disk sector %d\n",
            (group * 30) + entry, current_dirent->fname, current_dirent->inode, group, parent->data[group]);

            if(Disk_Write(inode_sector, inode_buffer) < 0){ //Update the parent inode sector
              return -1;
            }
            group = MAX_SECTORS_PER_FILE; //exit the outer loop
            break;
         }
      }
    }
  }

  // update parent inode and write to disk
  parent->size--;
  if(Disk_Write(inode_sector, inode_buffer) < 0){
    return -1;
  }
  dprintf("Updating the parent inode on the disk sector %d\n", inode_sector);

  return 0;
}

// representing an open file
typedef struct _open_file {
  int inode; // pointing to the inode of the file (0 means entry not used)
  int size;  // file size cached here for convenience
  int pos;   // read/write position
} open_file_t;
static open_file_t open_files[MAX_OPEN_FILES];

// return true if the file pointed to by inode has already been open
int is_file_open(int inode)
{
  for(int i=0; i<MAX_OPEN_FILES; i++) {
    if(open_files[i].inode == inode)
      return 1;
  }
  return 0;
}

// return a new file descriptor not used; -1 if full
int new_file_fd()
{
  for(int i=0; i<MAX_OPEN_FILES; i++) {
    if(open_files[i].inode <= 0)
      return i;
  }
  return -1;
}

/* end of internal helper functions, start of API functions */

int FS_Boot(char* backstore_fname)
{
  dprintf("FS_Boot('%s'):\n", backstore_fname);
  // initialize a new disk (this is a simulated disk)
  if(Disk_Init() < 0) {
    dprintf("... disk init failed\n");
    osErrno = E_GENERAL;
    return -1;
  }
  dprintf("... disk initialized\n");

  // we should copy the filename down; if not, the user may change the
  // content pointed to by 'backstore_fname' after calling this function
  strncpy(bs_filename, backstore_fname, 1024);
  bs_filename[1023] = '\0'; // for safety

  // we first try to load disk from this file
  if(Disk_Load(bs_filename) < 0) {
    dprintf("... load disk from file '%s' failed\n", bs_filename);

    // if we can't open the file; it means the file does not exist, we
    // need to create a new file system on disk
    if(diskErrno == E_OPENING_FILE) {
      dprintf("... couldn't open file, create new file system\n");

      // format superblock
      char buf[SECTOR_SIZE];
      memset(buf, 0, SECTOR_SIZE);
      *(int*)buf = OS_MAGIC;
      if(Disk_Write(SUPERBLOCK_START_SECTOR, buf) < 0) {
	dprintf("... failed to format superblock\n");
	osErrno = E_GENERAL;
	return -1;
      }
      dprintf("... formatted superblock (sector %d)\n", SUPERBLOCK_START_SECTOR);

      // format inode bitmap (reserve the first inode to root)
      bitmap_init(INODE_BITMAP_START_SECTOR, INODE_BITMAP_SECTORS, 1);
      dprintf("... formatted inode bitmap (start=%d, num=%d)\n",
	     (int)INODE_BITMAP_START_SECTOR, (int)INODE_BITMAP_SECTORS);

      // format sector bitmap (reserve the first few sectors to
      // superblock, inode bitmap, sector bitmap, and inode table)
      bitmap_init(SECTOR_BITMAP_START_SECTOR, SECTOR_BITMAP_SECTORS,
		  DATABLOCK_START_SECTOR);
      dprintf("... formatted sector bitmap (start=%d, num=%d)\n",
	     (int)SECTOR_BITMAP_START_SECTOR, (int)SECTOR_BITMAP_SECTORS);

      // format inode tables
      for(int i=0; i<INODE_TABLE_SECTORS; i++) {
	memset(buf, 0, SECTOR_SIZE);
	if(i==0) {
	  // the first inode table entry is the root directory
	  ((inode_t*)buf)->size = 0;
	  ((inode_t*)buf)->type = 1;
	}
	if(Disk_Write(INODE_TABLE_START_SECTOR+i, buf) < 0) {
	  dprintf("... failed to format inode table\n");
	  osErrno = E_GENERAL;
	  return -1;
	}
      }
      dprintf("... formatted inode table (start=%d, num=%d)\n",
	     (int)INODE_TABLE_START_SECTOR, (int)INODE_TABLE_SECTORS);

      // we need to synchronize the disk to the backstore file (so
      // that we don't lose the formatted disk)
      if(Disk_Save(bs_filename) < 0) {
	// if can't write to file, something's wrong with the backstore
	dprintf("... failed to save disk to file '%s'\n", bs_filename);
	osErrno = E_GENERAL;
	return -1;
      } else {
	// everything's good now, boot is successful
	dprintf("... successfully formatted disk, boot successful\n");
	memset(open_files, 0, MAX_OPEN_FILES*sizeof(open_file_t));
	return 0;
      }
    } else {
      // something wrong loading the file: invalid param or error reading
      dprintf("... couldn't read file '%s', boot failed\n", bs_filename);
      osErrno = E_GENERAL;
      return -1;
    }
  } else {
    dprintf("... load disk from file '%s' successful\n", bs_filename);

    // we successfully loaded the disk, we need to do two more checks,
    // first the file size must be exactly the size as expected (thiis
    // supposedly should be folded in Disk_Load(); and it's not)
    int sz = 0;
    FILE* f = fopen(bs_filename, "r");
    if(f) {
      fseek(f, 0, SEEK_END);
      sz = ftell(f);
      fclose(f);
    }
    if(sz != SECTOR_SIZE*TOTAL_SECTORS) {
      dprintf("... check size of file '%s' failed\n", bs_filename);
      osErrno = E_GENERAL;
      return -1;
    }
    dprintf("... check size of file '%s' successful\n", bs_filename);

    // check magic
    if(check_magic()) {
      // everything's good by now, boot is successful
      dprintf("... check magic successful\n");
      memset(open_files, 0, MAX_OPEN_FILES*sizeof(open_file_t));
      return 0;
    } else {
      // mismatched magic number
      dprintf("... check magic failed, boot failed\n");
      osErrno = E_GENERAL;
      return -1;
    }
  }
}

int FS_Sync()
{
  if(Disk_Save(bs_filename) < 0) {
    // if can't write to file, something's wrong with the backstore
    dprintf("FS_Sync():\n... failed to save disk to file '%s'\n", bs_filename);
    osErrno = E_GENERAL;
    return -1;
  } else {
    // everything's good now, sync is successful
    dprintf("FS_Sync():\n... successfully saved disk to file '%s'\n", bs_filename);
    return 0;
  }
}

int File_Create(char* file)
{
  dprintf("File_Create('%s'):\n", file);
  return create_file_or_directory(0, file);
}

/*
 *
 * This function:
 * - delete the file referenced by file.
 * - removes its name from the directory
 * - frees up any data blocks and inodes used by the file
 */
 int File_Unlink(char* file)
 {
   /* YOUR CODE */
   dprintf("File_Unlink('%s'):\n", file);

   int child_inode;
   char last_fname[MAX_NAME];
   int parent_inode = follow_path(file, &child_inode, last_fname); //Get the father inode

   if(parent_inode >= 0) {

     if(child_inode >= 0) {

       if(is_file_open(child_inode)==1){
         osErrno = E_FILE_IN_USE;
         return -1;
       }

       int result;
       result  = remove_inode(0, parent_inode, child_inode); //remove the inode representing a file

       switch(result){
         case 0:   dprintf("Succefully removed the inode representing a file\n");
                   return 0;

         case -1:  dprintf("Error: General error removing the inode\n");
                   osErrno = E_GENERAL;
                   return -1;

         case -2:  dprintf("Error: The current directory is not empty\n");
                   osErrno = E_DIR_NOT_EMPTY;
                   return -1;

         case -3:  dprintf("Error: Wrong type\n");
                   osErrno = E_GENERAL;
                   return -1;
       }

     }
     else{
       dprintf("The file '%s' does not exists, so the file failed to delete\n", file);
       osErrno = E_NO_SUCH_FILE;
       return -1;
     }
   }
   else {
       dprintf("Error: something is wrong with the file/path: '%s'\n", file);
       osErrno = E_NO_SUCH_FILE;
       return -1;
   }
   return -1;

 }

int File_Open(char* file)
{
  dprintf("File_Open('%s'):\n", file);
  int fd = new_file_fd();
  if(fd < 0) {
    dprintf("... max open files reached\n");
    osErrno = E_TOO_MANY_OPEN_FILES;
    return -1;
  }

  int child_inode;
  follow_path(file, &child_inode, NULL);
  if(child_inode >= 0) { // child is the one
    // load the disk sector containing the inode
    int inode_sector = INODE_TABLE_START_SECTOR+child_inode/INODES_PER_SECTOR;
    char inode_buffer[SECTOR_SIZE];

    if(Disk_Read(inode_sector, inode_buffer) < 0) {
       osErrno = E_GENERAL;
       return -1;
     }
    dprintf("... load inode table for inode from disk sector %d\n", inode_sector);

    // get the inode
    int inode_start_entry = (inode_sector-INODE_TABLE_START_SECTOR)*INODES_PER_SECTOR;
    int offset = child_inode-inode_start_entry;
    assert(0 <= offset && offset < INODES_PER_SECTOR);
    inode_t* child = (inode_t*)(inode_buffer+offset*sizeof(inode_t));
    dprintf("... inode %d (size=%d, type=%d)\n",
	    child_inode, child->size, child->type);

    if(child->type != 0) {
      dprintf("... error: '%s' is not a file\n", file);
      osErrno = E_GENERAL;
      return -1;
    }

    // initialize open file entry and return its index
    open_files[fd].inode = child_inode;
    open_files[fd].size = child->size;
    open_files[fd].pos = 0;
    return fd;
  }
  else {
    dprintf("... file '%s' is not found\n", file);
    osErrno = E_NO_SUCH_FILE;
    return -1;
  }
  return -1;
}

int File_Read(int fd, void* buffer, int size)
{
  /* YOUR CODE */
  if (!is_file_open(fd)) {
    osErrno = E_BAD_FD;
    return -1;
  }

  open_file_t *f = &open_files[fd];

  if (f->pos == f->size) {
    return 0;
  }


  // Load the disk sector containing the inode
  int  inode_sector = INODE_TABLE_START_SECTOR + f->inode / INODES_PER_SECTOR;
  char inode_buffer[SECTOR_SIZE];

  if (Disk_Read(inode_sector, inode_buffer) < 0) {
    osErrno = E_GENERAL;
    return -1;
  }
  dprintf("Loading the inode table for inode from the disk sector %d\n", inode_sector);

  // get the inode
  int inode_start_entry = (inode_sector - INODE_TABLE_START_SECTOR) * INODES_PER_SECTOR;
  int offset            = f->inode - inode_start_entry;
  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t *child = (inode_t *)(inode_buffer + offset * sizeof(inode_t));


  int  left            = size;
  int  current_position_in_sector = f->pos % SECTOR_SIZE;
  int  current_sector        = f->pos / SECTOR_SIZE;
  int  out_pos         = 0;
  char data_buf[SECTOR_SIZE];


  while (left > 0 && f->pos < f->size) {
    //read in sector, write to buffer, update left and pos
    Disk_Read(child->data[current_sector], data_buf);

    int to_read = 0;

    if (left > SECTOR_SIZE) {
      to_read = SECTOR_SIZE - current_position_in_sector;
    }
    else{
      to_read = left - current_position_in_sector;
    }
    memcpy(buffer, data_buf + current_position_in_sector, to_read);

    left -= to_read;
    current_position_in_sector = 0;
    f->pos += to_read;
    current_sector += 1;
    out_pos += to_read;
    if (f->pos > f->size) {
      f->pos = f->size;
    }
    return out_pos;
  }
  return -1;
}

/*
This method writes the size bytes from the buffer and writes them into the file
referenced by fd.
*/
int File_Write(int fd, void* buffer, int size)
{
  /* YOUR CODE */
  if (!is_file_open(fd)) {
    dprintf("Error: Could not write to a file that is not open.\n");
    osErrno = E_BAD_FD;
    return -1;
  }
  open_file_t *f = &open_files[fd];
  if (f->pos + size > MAX_SECTORS_PER_FILE * SECTOR_SIZE) {
    dprintf("Error: The file is too big to write to.\n");
    osErrno = E_FILE_TOO_BIG;
    return -1;
  }


  // Load the disk sector containing the inode
  int  inode_sector = INODE_TABLE_START_SECTOR + f->inode / INODES_PER_SECTOR;
  char inode_buffer[SECTOR_SIZE];

  if (Disk_Read(inode_sector, inode_buffer) < 0) {
    osErrno = E_GENERAL;
    return -1;
  }
  dprintf("Loading the inode table from the inode's disk sector %d\n", inode_sector);

  // get the inode
  int inode_start_entry = (inode_sector - INODE_TABLE_START_SECTOR) * INODES_PER_SECTOR;
  int offset = f->inode - inode_start_entry;
  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t *child = (inode_t *)(inode_buffer + offset * sizeof(inode_t));

  int allocated_sectors = (f->size + SECTOR_SIZE - 1) / SECTOR_SIZE;
  int needed_sectors = (size - (allocated_sectors * SECTOR_SIZE - f->pos) + SECTOR_SIZE - 1) / SECTOR_SIZE;

  for (int i = allocated_sectors; i < allocated_sectors + needed_sectors; i++) {

    int next = bitmap_first_unused(SECTOR_BITMAP_START_SECTOR, SECTOR_BITMAP_SECTORS, SECTOR_BITMAP_SIZE);
    dprintf("Assigning  the block %d, to the file for writing\n", next);

    if (next < 0) {
      dprintf("Error: The disk ran out of space while allocating blocks to write to.\n");
      osErrno = E_NO_SPACE;
      return -1;
    }
    child->data[i] = next;
  }
  Disk_Write(inode_sector, inode_buffer);

  allocated_sectors += needed_sectors;
  f->size = f->pos + size;
  child->size = f->size;

  int  left = size;
  int  current_position_in_sector = f->pos % SECTOR_SIZE;
  int  current_sector = f->pos / SECTOR_SIZE;
  int  in_pos = 0;
  char data_buf[SECTOR_SIZE];

  // Write out while still in allocated sectors
  while (left > 0 && current_sector < allocated_sectors) {

    //Read the sector, write to the buffer, and update the left and pos
    Disk_Read(child->data[current_sector], data_buf);
    int to_write = 0;
    if (left > SECTOR_SIZE) {
      to_write = SECTOR_SIZE - current_position_in_sector;
    }else{
      to_write = left - current_position_in_sector;
    }
    memcpy(data_buf + current_position_in_sector, buffer + in_pos, to_write);
    Disk_Write(child->data[current_sector], data_buf);

    left -= to_write;
    current_position_in_sector = 0;
    f->pos += to_write;
    current_sector += 1;
    in_pos += to_write;
  }

  int result = size - left;

  return result;
}

int File_Seek(int fd, int offset)
{
  /* YOUR CODE */
  open_file_t* f = &open_files[fd];
  if (is_file_open(f->inode) == 0){
    osErrno = E_BAD_FD;
    return -1;
  }
  if(f->size < offset || f->size < 0){
    osErrno = E_SEEK_OUT_OF_BOUNDS;
    return -1;
  }
  f->pos = offset;
  return f->pos;
}

int File_Close(int fd)
{
  dprintf("File_Close(%d):\n", fd);
  if(0 > fd || fd > MAX_OPEN_FILES) {
    dprintf("... fd=%d out of bound\n", fd);
    osErrno = E_BAD_FD;
    return -1;
  }
  if(open_files[fd].inode <= 0) {
    dprintf("... fd=%d not an open file\n", fd);
    osErrno = E_BAD_FD;
    return -1;
  }

  dprintf("... file closed successfully\n");
  open_files[fd].inode = 0;
  return 0;
}

int Dir_Create(char* path)
{
  dprintf("Dir_Create('%s'):\n", path);
  return create_file_or_directory(1, path);
}

int Dir_Unlink(char* path)
{
  /* YOUR CODE */
  int child_inode; // maybe just child
  char path_name[MAX_PATH];
  int parent_inode = follow_path(path, &child_inode, path_name);
  if(strcmp("/", path) == 0){
    osErrno = E_ROOT_DIR;
    return -1;
  }
  if(parent_inode < 0){
    osErrno = E_NO_SUCH_DIR;
    return -1;
  }
  if(remove_inode(1, parent_inode, child_inode) < 0){
    dprintf("Directory not empty/n");
    osErrno = E_DIR_NOT_EMPTY;
    return -1;
  }
  return 0;
}

int Dir_Size(char* path)
{
  /* YOUR CODE */
  char path_name[MAX_PATH];
  int child_inode;
  int parent_inode = follow_path(path, &child_inode, path_name);
  int result = 0;
  if(parent_inode < 0){
    osErrno = E_NO_SUCH_DIR;
    return -1;
  }
  if(child_inode >= 0){
    // load the disk sector containing the inode
    int inode_sector = INODE_TABLE_START_SECTOR + child_inode / INODES_PER_SECTOR;
    char inode_buffer[SECTOR_SIZE];
    if(Disk_Read(inode_sector, inode_buffer) < 0) {
      osErrno = E_GENERAL;
      return -1;
    }
    dprintf("... load inode table for inode from disk sector %d\n", inode_sector);

    // get the inode
    int inode_start_entry = (inode_sector - INODE_TABLE_START_SECTOR) * INODES_PER_SECTOR;
    int offset = child_inode - inode_start_entry;
    assert(0 <= offset && offset < INODES_PER_SECTOR);
    inode_t* child = (inode_t*)(inode_buffer + offset * sizeof(inode_t));
    dprintf("... inode %d (size=%d, type=%d)\n", child_inode, child->size, child->type);

    if(child->type != 1){
      dprintf("... is a file not a directory\n");
      return -1;
    }
    result = child->size * (sizeof(dirent_t));
  }


  return result;
}

int Dir_Read(char* path, void* buffer, int size)
{
  /* YOUR CODE */
  char child_name[16]; child_name[15] = '\0';
  int  child_node;
  int  parent_node = follow_path(path, &child_node, child_name);

  dprintf("Dir_Read: Followed the path\n");

  if (parent_node < 0) {
    osErrno = E_NO_SUCH_DIR;
    return -1;
  }

  int  inode_sector = INODE_TABLE_START_SECTOR + child_node / INODES_PER_SECTOR;
  char inode_buffer[SECTOR_SIZE];

  if (Disk_Read(inode_sector, inode_buffer) < 0) {
    return -1;
  }
  int offset = child_node % INODES_PER_SECTOR;

  inode_t *child = (inode_t *)(inode_buffer + offset * sizeof(inode_t));

  if (size < child->size * sizeof(dirent_t)) {

    osErrno = E_BUFFER_TOO_SMALL;
    return -1;
  }

  int out_pos = 0;
  // Read sectors into buffer
  // copy all dirents in full sectors
  char sec_buf[SECTOR_SIZE];

  for (int i = 0; i < child->size / DIRENTS_PER_SECTOR; i++) {

    if (Disk_Read(child->data[i], sec_buf) < 0) {
      return -1;
    }
    memcpy(buffer + out_pos, sec_buf, DIRENTS_PER_SECTOR * sizeof(dirent_t));

    out_pos += DIRENTS_PER_SECTOR * sizeof(dirent_t);
  }
  //copy over the last, partially filled sector
  int left = child->size % DIRENTS_PER_SECTOR;

  if (left > 0) {
    if (Disk_Read(child->data[child->size / DIRENTS_PER_SECTOR], sec_buf) < 0) {
      return -1;
    }
    memcpy(buffer + out_pos, sec_buf, left * sizeof(dirent_t));
  }
  return child->size;
}
