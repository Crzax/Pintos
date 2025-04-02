#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"

#define WRITE_BACK_PERIOD 4 * TIMER_FREQ
struct read_ahead_request 
{
    block_sector_t sector;    /**< read ahead sector */
    struct list_elem elem;    /**< list element */
};

static struct list read_ahead_queue;    /**< read ahead queue */
static struct lock read_ahead_lock;     /**< read ahead lock */
static struct semaphore read_ahead_sema;  /**< read ahead semaphore */

#ifdef VM
/** Daemon to handle read-ahead requests. */
static void 
read_ahead_daemon(void *aux UNUSED) {
  while (true) {
      sema_down(&read_ahead_sema);

      lock_acquire(&read_ahead_lock);
      while (!list_empty(&read_ahead_queue)) {
          struct list_elem *e = list_pop_front(&read_ahead_queue);
          struct read_ahead_request *req = list_entry(e, struct read_ahead_request, elem);
          block_sector_t sector = req->sector;
          free(req);
          lock_release(&read_ahead_lock);

          int idx = access_cache_entry(sector, false);
          if (idx >= 0) {
              cache_array[idx].open_cnt--;
          }

          lock_acquire(&read_ahead_lock);
      }
      lock_release(&read_ahead_lock);
  }
}

/** Request to read ahead a block sector. */
void 
schedule_read_ahead(block_sector_t sector) {
  struct read_ahead_request *req = malloc(sizeof(*req));
  if (req == NULL) return;

  req->sector = sector;

  lock_acquire(&read_ahead_lock);
  list_push_back(&read_ahead_queue, &req->elem);
  lock_release(&read_ahead_lock);

  sema_up(&read_ahead_sema);
}
#endif

/** Init the cache of IDX. */
void 
init_entry(int idx) {
  cache_array[idx].state = FREE;
  cache_array[idx].open_cnt = 0;
  cache_array[idx].dirty = false;
  cache_array[idx].accessed = false;
}

/** Init all caches. */
void 
init_cache(void) {
  int i;
  lock_init(&cache_lock);
  list_init(&read_ahead_queue);
  lock_init(&read_ahead_lock);
  sema_init(&read_ahead_sema, 0);
  for(i = 0; i < CACHE_MAX_SIZE; i++) {
      lock_init(&cache_array[i].entry_lock);
      cond_init(&cache_array[i].loading_cond);
      init_entry(i);
  }
  thread_create("cache_writeback", PRI_MIN, func_periodic_writer, NULL);
#ifdef VM
  thread_create("cache-readahead", PRI_MIN, read_ahead_daemon, NULL);
#endif
}
/** Find the cache of DISK_SECTOR. 
   If the cache is not in cache, return -1. */
int 
find_cache_entry(block_sector_t disk_sector) {
  int i;
  for(i = 0; i < CACHE_MAX_SIZE; i++) {
      if(cache_array[i].state != FREE && cache_array[i].disk_sector == disk_sector) {
          return i;
      }
  }
  return -1;
}

/** Get the first free cache. */
int 
get_free_entry(void)
{
  int i;
  for(i = 0; i < CACHE_MAX_SIZE; i++)
  {
    if(cache_array[i].state == FREE)
    {
      cache_array[i].state = LOADING;
      return i;
    }
  }

  return -1;  /**< All caches are full. */
}

static int
evict_cache_entry(void) 
{
    static int clock = 0;  /**< Clock hand for second chance algorithm */
    while (true) 
    {
        for (int j = 0; j < CACHE_MAX_SIZE * 2; j++) 
        {
            int i = (clock + j) % CACHE_MAX_SIZE;
            struct disk_cache *entry = &cache_array[i];
            
            lock_acquire(&entry->entry_lock);
            
            /* Skip if the entry is free or still in use */
            if (entry->state == FREE || entry->open_cnt > 0) 
            {
                lock_release(&entry->entry_lock);
                continue;
            }
            
            /* Check if the entry is accessed */
            if (entry->accessed) 
            {
                entry->accessed = false;  /**< Reset accessed bit */
                clock = (i + 1) % CACHE_MAX_SIZE;  /**< Move clock hand */
                lock_release(&entry->entry_lock);
            } 
            else 
            {
                /* Write back if dirty */
                if (entry->dirty) 
                {
                    block_write(fs_device, entry->disk_sector, entry->block);
                    entry->dirty = false;
                }
                
                /* Reset the entry */
                entry->state = FREE;
                entry->disk_sector = 0;
                entry->open_cnt = 0;
                entry->accessed = false;
                
                lock_release(&entry->entry_lock);
                clock = (i + 1) % CACHE_MAX_SIZE;  /**< Move clock hand */
                return i;  /**< Return the index of the evicted entry */
            }
        }
    }
}

/** Access the cache of DISK_SECTOR. 
   Increment the open count of the cache.
   Set the dirty bit of the cache.
   If the cache is not in the cache, replace it. */
int 
access_cache_entry(block_sector_t disk_sector, bool dirty) {
    lock_acquire(&cache_lock);
    int idx = find_cache_entry(disk_sector);
    if(idx != -1) {
        struct disk_cache *e = &cache_array[idx];
        while(e->state == LOADING) {
            cond_wait(&e->loading_cond, &cache_lock);
        }
        lock_acquire(&e->entry_lock);
        e->open_cnt++;
        e->accessed = true;
        e->dirty |= dirty;
        lock_release(&e->entry_lock);
        lock_release(&cache_lock);
        return idx;
    } else {
        int free_idx = get_free_entry();
        if(free_idx == -1) {
            free_idx = evict_cache_entry();
        }
        struct disk_cache *f = &cache_array[free_idx];
        bool need_write_back = false;
        block_sector_t old_sector;
        uint8_t temp_block[BLOCK_SECTOR_SIZE];
        if(f->state == READY) {
            lock_acquire(&f->entry_lock);
            if(f->dirty) {
                need_write_back = true;
                old_sector = f->disk_sector;
                memcpy(temp_block, f->block, BLOCK_SECTOR_SIZE);
            }
            lock_release(&f->entry_lock);
        }
        f->disk_sector = disk_sector;
        f->state = LOADING;
        f->open_cnt = 1;
        f->accessed = true;
        f->dirty = dirty;
        lock_release(&cache_lock);
        if(need_write_back) {
            block_write(fs_device, old_sector, temp_block);
        }
        block_read(fs_device, disk_sector, f->block);
        lock_acquire(&f->entry_lock);
        f->state = READY;
        cond_broadcast(&f->loading_cond, &f->entry_lock);
        lock_release(&f->entry_lock);
        return free_idx;
    }
}

/** Write back the cache to disk periodically. */
void
func_periodic_writer(void *aux UNUSED)
{
    while(true)
    {
        timer_sleep(WRITE_BACK_PERIOD);
        write_back(false);
    }
}

/** Write back the dirty cache to disk. 
   If clear is true, clear the cache. */
void 
write_back(bool clear)
{
    for (int i = 0; i < CACHE_MAX_SIZE; i++) 
    {
        struct disk_cache *entry = &cache_array[i];
        lock_acquire(&entry->entry_lock);
        
        if (entry->dirty) 
        {
            block_write(fs_device, entry->disk_sector, entry->block);
            entry->dirty = false;
        }
        
        if (clear) 
        {
            entry->state = FREE;
            entry->open_cnt = 0;
            entry->dirty = false;
            entry->accessed = false;
        }
        
        lock_release(&entry->entry_lock);
    }
}