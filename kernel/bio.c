// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// struct {
//   struct spinlock lock;
//   struct buf buf[NBUF];

//   // Linked list of all buffers, through prev/next.
//   // Sorted by how recently the buffer was used.
//   // head.next is most recent, head.prev is least.
//   struct buf head;
// } bcache;

#define NBUCKET 13
#define HASH(x) (x % NBUCKET)
struct {
  struct buf table[NBUCKET];
  struct spinlock lock[NBUCKET];
  struct buf buf[NBUF];
} bcache;

void
binit(void)
{
  struct buf *b;
  char lockname[16];
  // init the lock
  for (int i = 0; i < NBUCKET; ++i) {
    snprintf(lockname, sizeof lockname, "bcache[%d]", i);
    initlock(&bcache.lock[i], lockname);
    bcache.table[i].prev = &bcache.table[i];
    bcache.table[i].next = &bcache.table[i];
  }
  // all linked the table[0]
  for(b = bcache.buf; b < bcache.buf+NBUF; b++) {
    b->next = bcache.table[0].next;
    b->prev = &bcache.table[0];
    initsleeplock(&b->lock, "buffer");
    bcache.table[0].next->prev = b;
    bcache.table[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int id = HASH(blockno);
  acquire(&bcache.lock[id]);

  // Is the block already cached?
  for(b = bcache.table[id].next; b != &bcache.table[id]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[id]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  b = 0;
  struct buf* tmp;
  // FIXME split the id bucket and others
  for(int i = id, cycle = 0; cycle != NBUCKET; i = (i + 1) % NBUCKET) {
    ++cycle;
    // A thread get table[a] and going to get table[b]
    // B thread get table[b] and going to get table[a]
    // deadlock
    if (i != id) {
      if (!holding(&bcache.lock[i])) 
        acquire(&bcache.lock[i]);
      else
        continue;
    }
    // LRU
    for (tmp = bcache.table[i].next; tmp != &bcache.table[i]; tmp = tmp->next) {
      if ((tmp->refcnt == 0 ) && (b == 0 || tmp->timestamp < b->timestamp))
        b = tmp;
    }
    if (b) {
      if (i != id) {
        // insert to the table[id]
        b->next->prev = b->prev;
        b->prev->next = b->next;
        release(&bcache.lock[i]);

        b->next = bcache.table[id].next;
        b->prev = &bcache.table[id];
        bcache.table[id].next->prev = b;
        bcache.table[id].next = b;
      }
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;

      acquire(&tickslock);
      b->timestamp = ticks;
      release(&tickslock);

      release(&bcache.lock[id]);
      acquiresleep(&b->lock);
      return b;
    } else {
      if (i != id)
      release(&bcache.lock[i]);
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");
  int id = HASH(b->blockno);
  releasesleep(&b->lock);
  acquire(&bcache.lock[id]);

  b->refcnt--;
  acquire(&tickslock);
  b->timestamp = ticks;
  release(&tickslock);
  
  release(&bcache.lock[id]);
}

void
bpin(struct buf *b) {
  int id = HASH(b->blockno);
  acquire(&bcache.lock[id]);
  b->refcnt++;
  release(&bcache.lock[id]);
}

void
bunpin(struct buf *b) {
  int id = HASH(b->blockno);
  acquire(&bcache.lock[id]);
  b->refcnt--;
  release(&bcache.lock[id]);
}


