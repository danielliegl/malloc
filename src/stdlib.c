#include <sys/syscall.h>
#include "stdlib.h"
#include <stdio.h>
#include "string.h"
#include "pthread.h"

//set to 1, if you want to print debug messages
#define MALLOC_DEBUG_ENABLED 0

//magic number to check for data corruption by the user
#define MAGIC_NUMBER 0xDEADBEEF

//------------------------------------------------------------------------------
// The malloc data structure should usually be locked, but we do not have
// userspace locks so I can't really make it thread-safe. Too bad!

struct Block* blocks_start = NULL;

//spinlock stuff
pthread_spinlock_t malloc_lock;
int lock_initialized = 0;

//this struct stores metadata
//i like the word metadata
struct Block
{
  size_t magic_;
  size_t size_;
  size_t used_;
  struct Block* next_;
  struct Block* prev_;
};

//------------------------------------------------------------------------------

//gets the size of the Block struct
size_t getMetaSize()
{
  return sizeof(struct Block);
}

//------------------------------------------------------------------------------

//returns the size of a malloc'd block
size_t getMallocSize(void* ptr)
{
  struct Block* block = ((struct Block*) ptr) - 1;
  if(block->magic_ != MAGIC_NUMBER)
  {
    printf("gms: Invalid pointer or Magic number corrupt!");
    exit(-1);
  }
  return block->size_;
}

//------------------------------------------------------------------------------

// inserts a new block into the end of the linked list
void insertBlock(struct Block* block_to_be_inserted, struct Block* current_block)
{
  if(current_block->magic_ != MAGIC_NUMBER)
  {
    printf("ib: Magic number overwritten!");
    exit(-1);
  }

  if(current_block->next_ == NULL)
  {
    current_block->next_ = block_to_be_inserted;
    block_to_be_inserted->prev_ = current_block;
    if(MALLOC_DEBUG_ENABLED)
    {
      printf("block %p has been inserted next_ %p prev_ %p\n",
              block_to_be_inserted, block_to_be_inserted->next_,
              block_to_be_inserted->prev_);
      printf("prev last block %p: next_ %p prev_ %p\n",
              current_block, current_block->next_,
              current_block->prev_);
    }
  }
  else
  {
    insertBlock(block_to_be_inserted, current_block->next_);
  }
}

//------------------------------------------------------------------------------

//checks if we can reuse memory by merging blocks going forward
size_t checkForwardMerge(struct Block* current_block, size_t needed_size, size_t available_size)
{
  if(current_block == NULL)
  {
    return NULL;
  }

  if(current_block->magic_ != MAGIC_NUMBER)
  {
    printf("cfm: Magic number overwritten!");
    exit(-1);
  }

  if(available_size >= needed_size)
  {
    return 1;
  }

  if(current_block->used_ && available_size < needed_size)
  {
    return 0;
  }

  if(current_block->used_ == 0)
  {
    return checkForwardMerge(current_block->next_, needed_size, available_size + sizeof(struct Block) + current_block->size_);
  }
  return 0;
}

//------------------------------------------------------------------------------

//merges malloc blocks
void forwardBlockMerge(struct Block* first, struct Block* current, size_t size, size_t available)
{
  // printf("fbm: f:%p, c:%p, s:%ld, a:%ld\n", first, current, size, available);

  if(current->magic_ != MAGIC_NUMBER)
  {
    printf("fbm: Magic number overwritten!");
    exit(-1);
  }

  size_t current_available = 0;
  if(current == first)
  {
    current_available = current->size_;
  }
  else
  {
    current_available = available + current->size_ + sizeof(struct Block);
  }

  if(current_available >= size)
  {
    // printf("Enough space available! %ld, needed %ld\n", current_available, size);
    
    first->next_ = current->next_;
    first->size_ = current_available;
    current->next_->prev_ = first;
    first->used_ = 1;
    return;
  }
  
  if(available < size && current->used_ == 0)
  {
    // printf("Checking next block, current available: %ld\n", current_available);
    if(current == first)
    {
      forwardBlockMerge(first, current->next_, size, current->size_);
    }
    else
    {
      forwardBlockMerge(first, current->next_, size, available + current->size_ + sizeof(struct Block));
    }
    return;
  }
}

//------------------------------------------------------------------------------

// block parameter is the start of the list on initial call
// goes through the list to find a block which fits for our needed size
// returns NULL if no suitable block is found
//
struct Block* findFreeBlock(struct Block* current_block, size_t size)
{
  // printf("ffb: args: %p | %ld\n", current_block, size);

  if(current_block == NULL)
  {
    return NULL;
  }

  if(current_block->magic_ != MAGIC_NUMBER)
  {
    printf("ffb: Magic number overwritten!");
    exit(-1);
  }

  if(current_block->used_ == 0 && size <= current_block->size_)
  {
    return current_block;
  }
  
  if(current_block->used_ == 0)
  {
    if(checkForwardMerge(current_block, size, 0) == 1)
    {
      forwardBlockMerge(current_block, current_block, size, 0);
      return current_block;
    }
  }

  if(current_block->next_)
  {
    return findFreeBlock(current_block->next_, size);
  }

  return NULL;
}

//------------------------------------------------------------------------------

// parameter is the last block in the linked list
// goes through the list backwards to free memory recursively
//
void freeBlocks(struct Block* current_block)
{
  if(current_block == NULL)
  {
    return;
  }

  if(current_block->magic_ != MAGIC_NUMBER)
  {
    printf("freeBlocks: Magic number overwritten!");
    exit(-1);
  }

  if(MALLOC_DEBUG_ENABLED)
  {
    printf("freeBlocks: arg: %p\n", current_block);
  }

  if(current_block->used_ == 1)
  {
    // printf("Block is still used\n");
    return;
  }

  int free_size = -1 * ((int)sizeof(struct Block) + (int)current_block->size_);
  struct Block* temp_prev = current_block->prev_;
  if(current_block->next_ == 0 && current_block->prev_ == 0 && current_block->used_ == 0)
  {
    if(MALLOC_DEBUG_ENABLED)
    {
      printf("Freeing memory: %d\n", free_size);
    }
    sbrk(free_size);
    blocks_start = NULL;
    return;
  }

  if(current_block->next_ == NULL && current_block->used_ == 0)
  {
    if(MALLOC_DEBUG_ENABLED)
    {
      printf("Freeing memory: %d\n", free_size);
    }
    if(current_block->prev_)
    {
      current_block->prev_->next_ = NULL;
    }
    sbrk(free_size);
  }
  
  if(temp_prev != NULL)
  {
    if(MALLOC_DEBUG_ENABLED)
    {
      printf("freeBlocks recursion: arg: %p\n", current_block->prev_);
    }
    freeBlocks(temp_prev);
  }
}

//------------------------------------------------------------------------------

size_t checkBackwardMerge(struct Block* current_block, size_t needed_size, size_t available_size)
{
  if(current_block == NULL)
  {
    return 0;
  }

  if(current_block->magic_ != MAGIC_NUMBER)
  {
    printf("cbm: Magic number overwritten!");
    exit(-1);
  }

  if(available_size + current_block->size_ >= needed_size && current_block->used_ == 0)
  {
    return 1;
  }

  if(current_block->used_ == 0 && current_block->prev_)
  {
    return checkBackwardMerge(current_block->prev_, needed_size, available_size + sizeof(struct Block) + current_block->size_);
  }

  return 0;
}

//------------------------------------------------------------------------------

struct Block* backwardBlockMerge(struct Block* first, struct Block* current, size_t size, size_t available)
{
  // printf("fbm: f:%p, c:%p, s:%ld, a:%ld\n", first, current, size, available);
  if(current == NULL)
  {
    return NULL;
  }

  if(current->magic_ != MAGIC_NUMBER)
  {
    printf("bbm: Magic number overwritten!");
    exit(-1);
  }

  size_t current_available = 0;
  if(current == first)
  {
    current_available = current->size_;
  }
  else
  {
    current_available = available + current->size_ + sizeof(struct Block);
  }

  if(current_available >= size)
  {
    // printf("Enough space available! %ld, needed %ld\n", current_available, size);
    if(first->next_)
    {
      first->next_->prev_ = current;
    }
    current->next_ = first->next_;
    current->size_ = current_available;
    current->used_ = 1;
    return current;
  }
  else
  {
    // printf("Checking next block, current available: %ld\n", current_available);
    if(current == first)
    {
      return backwardBlockMerge(first, current->prev_, size, current_available);
    }
    else
    {
      return backwardBlockMerge(first, current->prev_, size, current_available);
    }
  }

}

//------------------------------------------------------------------------------

void *realloc(void *ptr, size_t size)
{
  if(!lock_initialized)
  {
    lock_initialized = 1;
    pthread_spin_init(&malloc_lock, 0);
  }

  if(ptr == NULL)
  {
    return malloc(size);
  }

  pthread_spin_lock(&malloc_lock);

  struct Block* block = ((struct Block*)ptr) - 1;

  if(block->magic_ != MAGIC_NUMBER)
  {
    printf("Realloc: Magic number invalid!");
    pthread_spin_unlock(&malloc_lock);
    exit(-1);
  }

  //check prev and next if we can just extend the memory
  //if this is not possible, just move the block to the end of the list
  block->used_ = 0;
  //try forward merge
  if(block->next_ && checkForwardMerge(block, size, 0))
  {
    forwardBlockMerge(block, block, size, 0);
    pthread_spin_unlock(&malloc_lock);
    return block + 1;
  }

  //try backward merge
  if(block->prev_ && checkBackwardMerge(block, size, 0))
  {
    void* new_block_data = backwardBlockMerge(block, block, size, 0) + 1;
    memcpy(new_block_data, ptr, block->size_);
    pthread_spin_unlock(&malloc_lock);
    return new_block_data;
  }


  pthread_spin_unlock(&malloc_lock);
  //if we cannot reuse memory, we just malloc new memory and copy the contents
  void* new_block_data = malloc(size);
  pthread_spin_lock(&malloc_lock);

  struct Block* new_block = ((struct Block*)new_block_data) - 1;

  if(new_block->magic_ != MAGIC_NUMBER)
  {
    printf("Realloc: Magic number invalid!");
    pthread_spin_unlock(&malloc_lock);
    exit(-1);
  }

  memcpy(new_block_data, ptr, block->size_);
  pthread_spin_unlock(&malloc_lock);
  return new_block_data;
}

//------------------------------------------------------------------------------

void *calloc(size_t nmemb, size_t size)
{
  if(!lock_initialized)
  {
    lock_initialized = 1;
    pthread_spin_init(&malloc_lock, 0);
  }

  if(nmemb == 0 || size == 0)
  {
    return NULL;
  }

  pthread_spin_lock(&malloc_lock);

  void* alloced_memory = malloc(nmemb * size);
  if(alloced_memory == NULL)
  {
    pthread_spin_unlock(&malloc_lock);
    return NULL;
  }
  pthread_spin_unlock(&malloc_lock);

  memset(alloced_memory, 0, nmemb * size);
  return alloced_memory;
}

//------------------------------------------------------------------------------

void *malloc(size_t size)
{
  if(!lock_initialized)
  {
    lock_initialized = 1;
    pthread_spin_init(&malloc_lock, 0);
  }

  //check for 0 size malloc
  if(size == 0)
  {
    return NULL;
  }

  pthread_spin_lock(&malloc_lock);

  //check if there is an empty block available
  struct Block* free_block = findFreeBlock(blocks_start, size);
  if(free_block != NULL)
  {
    // printf("block has been reused\n");
    free_block->used_ = 1;
    pthread_spin_unlock(&malloc_lock);
    return free_block + 1;
  }

  //get memory for struct
  struct Block* new_block = (struct Block*)sbrk(sizeof(struct Block) + size);
  if(new_block == (void*)-1)
  {
    pthread_spin_unlock(&malloc_lock);
    return (void*)-1;
  }
  
  //initialize struct
  new_block->magic_ = MAGIC_NUMBER;
  new_block->used_ = 1;
  new_block->next_ = NULL;
  new_block->prev_ = NULL;
  new_block->size_ = size;

  //this happens upon the first time malloc is used
  if(blocks_start == NULL)
  {
    blocks_start = new_block;
    if(MALLOC_DEBUG_ENABLED)
    {
      printf("block %p has been inserted next_ %p prev_ %p\n",
              new_block, new_block->next_,
              new_block->prev_);
    }
    pthread_spin_unlock(&malloc_lock);
    return new_block + 1;
  }

  //insert block into double linked list
  insertBlock(new_block, blocks_start);

  if(MALLOC_DEBUG_ENABLED)
  {
    printf("Pointer to new malloc block: %p\n", new_block);
    printf("Size of malloc block: %ld\n", sizeof(struct Block));
    printf("Pointer to space useable by the user: %p\n", new_block + 1);
  }

  pthread_spin_unlock(&malloc_lock);
  
  //+1 moves the pointer to end of the struct
  return new_block + 1;
}

//------------------------------------------------------------------------------

void free(void *ptr)
{
  if(!lock_initialized)
  {
    lock_initialized = 1;
    pthread_spin_init(&malloc_lock, 0);
  }

  if(ptr == NULL)
  {
    return;
  }

  pthread_spin_lock(&malloc_lock);

  struct Block* block = ((struct Block*) ptr) - 1;

  if(block->magic_ != MAGIC_NUMBER)
  {
    printf("Free: Invalid free or magic number overwritten\n");
    pthread_spin_unlock(&malloc_lock);
    exit(-1);
  }

  if(MALLOC_DEBUG_ENABLED)
  {
    printf("freeing block %p next_ %p prev_ %p free arg: %p\n",
              block, block->next_,
              block->prev_, ptr);
  }

  if(block->used_ == 0)
  {
    //block was already free'd
    pthread_spin_unlock(&malloc_lock);
    return;
  }

  block->used_ = 0;

  if(block->next_ == NULL)
  {
    //if this is the last block in the list, free the memory
    freeBlocks(block);
  }
  pthread_spin_unlock(&malloc_lock);
  return;
}

//------------------------------------------------------------------------------
