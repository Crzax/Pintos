#include "devices/shutdown.h"
#include "devices/input.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "lib/kernel/list.h"
#ifdef VM
#include "vm/page.h"
#endif
#ifdef FILESYS
#include "filesys/inode.h"
#include "filesys/directory.h"
#endif

#ifdef DEBUG
#define _DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
#define _DEBUG_PRINTF(...) /**< do nothing */
#endif

static void syscall_handler (struct intr_frame *);

/** Auxiliary Functions.*/
static void check_user (const uint8_t *uaddr);
static int32_t get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);
static int memread_user (void *src, void *des, size_t bytes);
static struct file_desc* find_file_desc(struct thread *, int fd);
static bool validate_user_string(const char *uaddr);
static int fail_invalid_access(void);

#ifdef VM
static struct mmap_desc* find_mmap_desc(struct thread *, mmapid_t fd);
void preload_and_pin_pages(const void *, size_t);
void unpin_preloaded_pages(const void *, size_t);
#endif


void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int syscall_number;
  ASSERT( sizeof(syscall_number) == 4 ); /**< assuming x86. */

  /* The system call number is in the 32-bit word at the caller's stack pointer. */
  memread_user(f->esp, &syscall_number, sizeof(syscall_number));

  _DEBUG_PRINTF ("[DEBUG] system call, number = %d!\n", syscall_number);

  /* Store the esp, which is needed in the page fault handler.
   refer to exception.c:page_fault() (see manual 4.3.3) */
  thread_current() -> current_esp = f -> esp;

  /* Dispatch w.r.t system call number
   SYS_*** constants are defined in syscall-nr.h */
  switch (syscall_number) 
    {
      case SYS_HALT:
        {
          sys_halt();
          NOT_REACHED();
          break;
        }
    
      case SYS_EXIT:
        {
          int exitcode;
          memread_user(f->esp + 4, &exitcode, sizeof(exitcode));
          sys_exit(exitcode);
          NOT_REACHED();
          break;
        }
    
      case SYS_EXEC:
        {
          void* cmdline;
          memread_user(f->esp + 4, &cmdline, sizeof(cmdline));
          int return_code = sys_exec((const char*) cmdline);
          f->eax = (uint32_t) return_code;
          break;
        }
      case SYS_WAIT:
        {
          pid_t pid;
          memread_user(f->esp + 4, &pid, sizeof(pid_t));
          int ret = sys_wait(pid);
          f->eax = (uint32_t) ret;
          break;
        }
      case SYS_CREATE:
        {
          const char* filename;
          unsigned initial_size;
          bool return_code;
          memread_user(f->esp + 4, &filename, sizeof(filename));
          memread_user(f->esp + 8, &initial_size, sizeof(initial_size));
          return_code = sys_create(filename, initial_size);
          f->eax = return_code;
          break;
        }
      case SYS_REMOVE:
        {
          const char* filename;
          bool return_code;
          memread_user(f->esp + 4, &filename, sizeof(filename));
          return_code = sys_remove(filename);
          f->eax = return_code;
          break;
        }
      case SYS_OPEN:
        {
          const char* filename;
          int return_code;
          memread_user(f->esp + 4, &filename, sizeof(filename));
          return_code = sys_open(filename);
          f->eax = return_code;
          break;
        }
      case SYS_FILESIZE:
        {
          int fd, return_code;
          memread_user(f->esp + 4, &fd, sizeof(fd));
          return_code = sys_filesize(fd);
          f->eax = return_code;
          break;
        }
      case SYS_READ:
        {
          int fd, return_code;
          void *buffer;
          unsigned size;
          memread_user(f->esp + 4, &fd, sizeof(fd));
          memread_user(f->esp + 8, &buffer, sizeof(buffer));
          memread_user(f->esp + 12, &size, sizeof(size));
          return_code = sys_read(fd, buffer, size);
          f->eax = (uint32_t) return_code;
          break;
        }
      case SYS_WRITE:
      {
        int fd, return_code;
        const void *buffer;
        unsigned size;
        memread_user(f->esp + 4, &fd, sizeof(fd));
        memread_user(f->esp + 8, &buffer, sizeof(buffer));
        memread_user(f->esp + 12, &size, sizeof(size));
        return_code = sys_write(fd, buffer, size);
        f->eax = (uint32_t) return_code;
        break;
      }
      case SYS_SEEK:
        {
          int fd;
          unsigned position;
          memread_user(f->esp + 4, &fd, sizeof(fd));
          memread_user(f->esp + 8, &position, sizeof(position));
          sys_seek(fd, position);
          break;
        }
      case SYS_TELL:
        {
          int fd;
          unsigned return_code;
          memread_user(f->esp + 4, &fd, sizeof(fd));
          return_code = sys_tell(fd);
          f->eax = (uint32_t) return_code;
          break;
        }
      case SYS_CLOSE:
        {
          int fd;
          memread_user(f->esp + 4, &fd, sizeof(fd));
          sys_close(fd);
          break;
        }
#ifdef VM
        case SYS_MMAP:
          {
            int fd;
            void *addr;
            memread_user(f->esp + 4, &fd, sizeof(fd));
            memread_user(f->esp + 8, &addr, sizeof(addr));
      
            mmapid_t ret = sys_mmap (fd, addr);
            f->eax = ret;
            break;
          }
      
        case SYS_MUNMAP:
          {
            mmapid_t mid;
            memread_user(f->esp + 4, &mid, sizeof(mid));
      
            sys_munmap(mid);
            break;
          }
#endif
#ifdef FILESYS
        case SYS_CHDIR:
        {
          char* filename;
          bool return_code;
          memread_user(f->esp + 4, &filename, sizeof(filename));
          return_code = sys_chdir(filename);
          f->eax = (uint32_t) return_code;
          break;
        }
        case SYS_MKDIR:
        {  
          char* filename;
          bool return_code;
          memread_user(f->esp + 4, &filename, sizeof(filename));
          return_code = sys_mkdir(filename);
          f->eax = (uint32_t) return_code;
          break;
        }
        case SYS_READDIR:
        {  
          int fd;
          char *name;
          bool return_code;
          memread_user(f->esp + 4, &fd, sizeof(fd));
          memread_user(f->esp + 8, &name, sizeof(name));
          return_code = sys_readdir(fd,name);
          f->eax = (uint32_t) return_code;
          break;
        }
        case SYS_ISDIR:
         { 
          int fd;
          bool return_code;
          memread_user(f->esp + 4, &fd, sizeof(fd));
          return_code = sys_isdir(fd);
          f->eax = (uint32_t) return_code;
          break;
         }
        case SYS_INUMBER:
        {  
          int fd;
          int return_code;
          memread_user(f->esp + 4, &fd, sizeof(fd));
          return_code = sys_inumber(fd);
          f->eax = (uint32_t) return_code;
          break;
        }
#endif
      /* unhandled case */
      default:
        printf("[ERROR] system call %d is unimplemented!\n", syscall_number);
        sys_exit(-1);
        break;
    }
}

/****************** System Call Implementations ********************/
void 
sys_halt(void) 
{
  shutdown_power_off();
}

void 
sys_exit(int status UNUSED) 
{
  printf("%s: exit(%d)\n", thread_current()->name, status);

  /* The process exits.
    wake up the parent process (if it was sleeping) using semaphore,
    and pass the return code. */
  struct process_control_block *pcb = thread_current()->pcb;
  if(pcb != NULL) 
    {
      pcb->exitcode = status;
    }

  thread_exit();
}

pid_t 
sys_exec(const char *cmdline) 
{
  _DEBUG_PRINTF("[DEBUG] Exec : %s\n", cmdline);

  /* check the cmdline is in user mem. */
  if (!validate_user_string (cmdline))
    {
      fail_invalid_access ();
    }
  
  /* load() uses filesystem. */
    pid_t pid = process_execute(cmdline);
  return pid;
}

int 
sys_wait(pid_t pid) 
{
  _DEBUG_PRINTF ("[DEBUG] Wait : %d\n", pid);
  return process_wait(pid);
}

bool 
sys_create(const char* filename, unsigned initial_size) 
{
  bool return_code;
  /* memory validation */
  check_user((const uint8_t*) filename);

    return_code = filesys_create(filename, initial_size, false);
    return return_code;
}

bool 
sys_remove(const char* filename) 
{
  bool return_code;
  /* memory validation */
  check_user((const uint8_t*) filename);

    return_code = filesys_remove(filename);
  return return_code;
}

int 
sys_open(const char* file) 
{
  /* memory validation */
  check_user((const uint8_t*) file);
  
  struct file* file_opened;
  struct file_desc* fd = palloc_get_page(0);

  if (!fd) {
    return -1;
  }

    file_opened = filesys_open(file);
  if (!file_opened) {
    palloc_free_page (fd);
      return -1;
  }

  fd->file = file_opened;

  struct list* fd_list = &thread_current ()->file_descriptors;
  if (list_empty(fd_list)) 
    {
      /* 0, 1, 2 are reserved for stdin, stdout, stderr. */
      fd->id = 3;
    }
  else 
    {
      fd->id = (list_entry(list_back(fd_list), struct file_desc, elem)->id) + 1;
    }
  list_push_back(fd_list, &(fd->elem));

  return fd->id;
}

int 
sys_filesize(int fd) 
{
  
  struct file_desc* file_d;

  file_d = find_file_desc(thread_current(), fd);

  if(file_d == NULL) 
  {
      return -1;
  }
  if (inode_is_dir(file_get_inode(file_d->file))) 
  {
      return -1;
  }
  int ret = file_length(file_d->file);
  return ret;
}

void sys_seek(int fd, unsigned position) 
{
    struct file_desc* file_d = find_file_desc(thread_current(), fd);
  if(file_d && file_d->file && !inode_is_dir(file_get_inode(file_d->file))) 
    {
      file_seek(file_d->file, position);
    }
  else
    return; // TODO need sys_exit?
}

unsigned 
sys_tell(int fd) 
{
    struct file_desc* file_d = find_file_desc(thread_current(), fd);
  unsigned ret;
  if(file_d && file_d->file && !inode_is_dir(file_get_inode(file_d->file))) 
    {
      ret = file_tell(file_d->file);
    }
  else
    ret = -1; // TODO need sys_exit?

  return ret;
}

void 
sys_close(int fd) 
{
    struct file_desc* file_d = find_file_desc(thread_current(), fd);

  if(file_d && file_d->file) 
    {
      if (!inode_is_dir(file_get_inode(file_d->file)))
      {
        file_close(file_d->file);
        list_remove(&(file_d->elem));
        palloc_free_page(file_d);
      }else
        {
          dir_close(file_d->file);
          list_remove(&(file_d->elem));
          palloc_free_page(file_d);
        }
    }
}

int 
sys_read(int fd, void *buffer, unsigned size) 
{
   /* memory validation : [buffer+0, buffer+size) should be all valid. */
   check_user((const uint8_t*) buffer);
   check_user((const uint8_t*) buffer + size - 1);

      int ret;

  if(fd == 0) 
    { /**< stdin */
      unsigned i;
      for(i = 0; i < size; ++i) 
        {
          if(! put_user(buffer + i, input_getc()) ) 
            {
                          sys_exit(-1); /**< segfault */
            }
        }
      ret = size;
    }
  else 
    {
      /* read from file */
      struct file_desc* file_d = find_file_desc(thread_current(), fd);

      if(file_d && file_d->file && !inode_is_dir(file_get_inode(file_d->file))) 
        {
#ifdef VM
          preload_and_pin_pages(buffer, size);
#endif
          ret = file_read(file_d->file, buffer, size);
#ifdef VM
          unpin_preloaded_pages(buffer, size);
#endif
        }
      else /**< no such file or can't open */
        ret = -1;
    }

  return ret;
}

int 
sys_write(int fd, const void *buffer, unsigned size) {
  /* memory validation : [buffer+0, buffer+size) should be all valid */
  check_user((const uint8_t*) buffer);
  check_user((const uint8_t*) buffer + size - 1);
    int ret;
  if(fd == 1) 
    { /**< write to stdout */
      putbuf(buffer, size);
      ret = size;
    }
  else 
    {
      /* write into file */
      struct file_desc* file_d = find_file_desc(thread_current(), fd);

      if(file_d && file_d->file && !inode_is_dir(file_get_inode(file_d->file))) 
        {
#ifdef VM
          preload_and_pin_pages(buffer, size);
#endif
    
          ret = file_write(file_d->file, buffer, size);
    
#ifdef VM
          unpin_preloaded_pages(buffer, size);
#endif        
        }
      else /**< no such file or can't open */
        ret = -1;
    }
  
  return ret;
}
#ifdef VM
mmapid_t sys_mmap(int fd, void *upage) {
  /* check arguments */
  if (upage == NULL || pg_ofs(upage) != 0) return -1;
  if (fd <= 1) return -1; /**< 0 and 1 are unmappable */
  struct thread *curr = thread_current();

  
  /* 1. Open file */
  struct file *f = NULL;
  struct file_desc* file_d = find_file_desc(thread_current(), fd);
  if(file_d && file_d->file&& !inode_is_dir(file_get_inode(file_d->file))) {
    /* reopen file so that it doesn't interfere with process itself
     it will be store in the mmap_desc struct (later closed on munmap) */
    f = file_reopen (file_d->file);
  }
  if(f == NULL||inode_is_dir(file_get_inode(file_d->file))) 
    goto MMAP_FAIL;

  size_t file_size = file_length(f);
  if(file_size == 0) goto MMAP_FAIL;

  /* 2. Mapping memory pages */
  /* First, ensure that all the page address is NON-EXIESENT. */
  size_t offset;
  for (offset = 0; offset < file_size; offset += PGSIZE) {
    void *addr = upage + offset;
    if (vm_supt_has_entry(curr->supt, addr)) goto MMAP_FAIL;
  }

  /* Now, map each page to filesystem */
  for (offset = 0; offset < file_size; offset += PGSIZE) {
    void *addr = upage + offset;

    size_t read_bytes = (offset + PGSIZE < file_size ? PGSIZE : file_size - offset);
    size_t zero_bytes = PGSIZE - read_bytes;

    vm_supt_install_filesys(curr->supt, addr,
        f, offset, read_bytes, zero_bytes, /*writable*/true);
  }

  /* 3. Assign mmapid */
  mmapid_t mid;
  if (! list_empty(&curr->mmap_list)) {
    mid = list_entry(list_back(&curr->mmap_list), struct mmap_desc, elem)->id + 1;
  }
  else mid = 1;

  struct mmap_desc *mmap_d = (struct mmap_desc*) malloc(sizeof(struct mmap_desc));
  mmap_d->id = mid;
  mmap_d->file = f;
  mmap_d->addr = upage;
  mmap_d->size = file_size;
  list_push_back (&curr->mmap_list, &mmap_d->elem);

  /* OK, release and return the mid */
  return mid;


MMAP_FAIL:
  /* finally: release and return */
  return -1;
}

bool sys_munmap(mmapid_t mid)
{
  struct thread *curr = thread_current();
  struct mmap_desc *mmap_d = find_mmap_desc(curr, mid);

  if(mmap_d == NULL) { /**< not found such mid. */
    return false; 
  }

    {
    /* Iterate through each page */
    size_t offset, file_size = mmap_d->size;
    for(offset = 0; offset < file_size; offset += PGSIZE) {
      void *addr = mmap_d->addr + offset;
      size_t bytes = (offset + PGSIZE < file_size ? PGSIZE : file_size - offset);
      vm_supt_mm_unmap (curr->supt, curr->pagedir, addr, mmap_d->file, offset, bytes);
    }

    /* Free resources, and remove from the list. */
    list_remove (& mmap_d->elem);
    file_close (mmap_d->file);
    free (mmap_d);
  }

  return true;
}
#endif
#ifdef FILESYS
bool 
sys_chdir(char* path)
{
    check_user((const uint8_t*) path);
    bool success = filesys_chdir(path);
    return success;
}

bool 
sys_mkdir(char* path)
{
    bool success = filesys_create(path, 0, true);
    return success;
}

bool 
sys_readdir(int fd, char* path)
{
    ASSERT (fd >= 0);
    
    struct file* file = find_file_desc(thread_current(), fd)->file;
    if (file == NULL) return false;
    
    struct inode* inode = file_get_inode(file);
    if(inode == NULL) return false;
    if(!inode_is_dir(inode)) return false;
    
    struct dir* dir = (struct dir*) file;
    if(!dir_readdir(dir, path)) return false;
    
    return true;
}

bool 
sys_isdir(int fd)
{
    ASSERT (fd >= 0);

    struct file* file = find_file_desc(thread_current(), fd)->file;
    if (file == NULL) return false;

    struct inode* inode = file_get_inode(file);
    if(inode == NULL) return false;
    if(!inode_is_dir(inode)) return false;
    
    return true;
}

int 
sys_inumber(int fd)
{
    ASSERT (fd >= 0);

    struct file* file = find_file_desc(thread_current(), fd)->file;
    if (file == NULL) return -1;

    struct inode* inode = file_get_inode(file);
    if(inode == NULL) return -1;

    block_sector_t inumber = inode_get_inumber(inode);
    return inumber;
}
#endif
/****************** Auxiliary Functions on Memory Access ********************/

/** Invalid memory access, fail and exit. */ 
static int 
fail_invalid_access(void) 
{
  sys_exit (-1);
  NOT_REACHED();
}

/** check the whole user_string is in user mem. */
static bool 
validate_user_string(const char *uaddr) 
{
  const char *p = uaddr;
  size_t max_len = 1024; 
  while (max_len--) {
    int c = get_user((const uint8_t *)p);
    if (c == -1) return false;
    if (c == 0) return true;
    p++;
  }
  return false;
}

/** check a single byte is in user mem. */
static void
check_user (const uint8_t *uaddr) 
{
  /* check uaddr range or segfaults */
  if(get_user (uaddr) == -1)
    fail_invalid_access();
}

/** Reads a single 'byte' at user memory admemory at 'uaddr'.
  'uaddr' must be below PHYS_BASE.
 
  Returns the byte value if successful (extract the least significant byte),
  or -1 in case of error (a segfault occurred or invalid uaddr) */
static int32_t
get_user (const uint8_t *uaddr) 
{
  /* check that a user pointer `uaddr` points below PHYS_BASE*/
  if (! ((void*)uaddr < PHYS_BASE))
    return -1; /**< invalid memory access */

  /* as suggested in the reference manual, see (3.1.5) */
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
      : "=&a" (result) : "m" (*uaddr));
  return result;
}

/** Writes a single byte (content is 'byte') to user address 'udst'.
  'udst' must be below PHYS_BASE.
 
  Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte) 
{
  /* check that a user pointer `udst` points below PHYS_BASE */
  if (! ((void*)udst < PHYS_BASE)) 
    {
      return false;
    }
  int error_code;

  /* as suggested in the reference manual, see (3.1.5) */
  asm ("movl $1f, %0; movb %b2, %1; 1:"
      : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}

/** Reads a consecutive `bytes` bytes of user memory with the
  starting address `src` (uaddr), and writes to dst.
  Returns the number of bytes read, or -1 on page fault (invalid memory access) */
static int
memread_user (void *src, void *dst, size_t bytes)
{
  int32_t value;
  size_t i;
  for(i = 0; i < bytes; i++) 
    {
      value = get_user(src + i);
      if(value == -1) /**< segfault or invalid memory access */
        fail_invalid_access();
      *(char*)(dst + i) = value & 0xff;
    }
  return (int)bytes;
}

/** Find file description. */
static struct file_desc*
find_file_desc(struct thread *t, int fd)
{
  ASSERT (t != NULL);

  if (fd < 3) 
  {
    return NULL;
  }

  struct list_elem *e;
  if (! list_empty(&t->file_descriptors)) 
  {
    for(e = list_begin(&t->file_descriptors);
        e != list_end(&t->file_descriptors); e = list_next(e))
    {
      struct file_desc *desc = list_entry(e, struct file_desc, elem);
      if(desc->id == fd) 
      {
        return desc;
      }
    }
  }

  return NULL; /**< not found */
}
#ifdef VM
/** Find mmap description. */
static struct mmap_desc*
find_mmap_desc(struct thread *t, mmapid_t mid)
{
  ASSERT (t != NULL);

  struct list_elem *e;

  if (! list_empty(&t->mmap_list)) {
    for(e = list_begin(&t->mmap_list);
        e != list_end(&t->mmap_list); e = list_next(e))
    {
      struct mmap_desc *desc = list_entry(e, struct mmap_desc, elem);
      if(desc->id == mid) {
        return desc;
      }
    }
  }

  return NULL; /**< not found */
}

/** Preload and pin pages. */
void preload_and_pin_pages(const void *buffer, size_t size)
{
  struct supplemental_page_table *supt = thread_current()->supt;
  uint32_t *pagedir = thread_current()->pagedir;

  void *upage;
  for(upage = pg_round_down(buffer); upage < buffer + size; upage += PGSIZE)
  {
    vm_load_page (supt, pagedir, upage);
    vm_pin_page (supt, upage);
  }
}

/** Unpin preloaded pages. */
void unpin_preloaded_pages(const void *buffer, size_t size)
{
  struct supplemental_page_table *supt = thread_current()->supt;

  void *upage;
  for(upage = pg_round_down(buffer); upage < buffer + size; upage += PGSIZE)
  {
    vm_unpin_page (supt, upage);
  }
}

#endif
