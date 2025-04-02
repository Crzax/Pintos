#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "devices/timer.h"
#include "threads/synch.h"

#define CACHE_MAX_SIZE 64

enum cache_state { FREE, LOADING, READY };

struct disk_cache 
{    
    uint8_t block[BLOCK_SECTOR_SIZE];   /**< 512 Bytes */
    block_sector_t disk_sector;         /**< disk sector */
    enum cache_state state;             /**< cache state */
    struct lock entry_lock;             /**< lock for entry */
    struct condition loading_cond;      /**< condition for loading */
    int open_cnt;                       /**< open count */
    bool accessed;                      /**< accessed */
    bool dirty;                         /**< dirty */  
};

struct lock cache_lock;                 /**< cache lock */
struct disk_cache cache_array[64];      /**< cache array */

/** Cache functions */
void init_entry(int idx);
void init_cache(void);
int find_cache_entry(block_sector_t disk_sector);
int get_free_entry(void);
int access_cache_entry(block_sector_t disk_sector, bool dirty);
void func_periodic_writer(void *aux);
void write_back(bool clear);
void schedule_read_ahead(block_sector_t sector);

#endif /**< filesys/cache.h */ 