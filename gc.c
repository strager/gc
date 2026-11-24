#include "procmaps.inc.c"
#include <linux/prctl.h>
#include <sys/prctl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/errno.h>
#include <stdbit.h>
#include <sys/mman.h>
#include <assert.h>

typedef struct GC_Chunk {
  char *start;
  char *end;              // &start[total_size] (one past the end)
  size_t remaining_size;  // In bytes.
  size_t size_class;
  struct GC_Chunk *next;
  uint8_t marks[];        // One bit per object.
} GC_Chunk;

static pthread_mutex_t GC_mutex = PTHREAD_MUTEX_INITIALIZER;

static GC_Chunk *GC_chunks = nullptr;

static void GC_lock() {
  int rc = pthread_mutex_lock(&GC_mutex);
  assert(rc == 0);
}

static void GC_unlock() {
  int rc = pthread_mutex_unlock(&GC_mutex);
  assert(rc == 0);
}

static void GC_printf(const char *format, ...) {
  va_list va;
  va_start(va, format);
  int fd = STDOUT_FILENO;
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), format, va);
  write(fd, buffer, strlen(buffer));
  va_end(va);
}

static void GC_dump_chunks() {
  static int dump_counter = 0;
  GC_printf("--- GC chunk dump % 7d ---\n", dump_counter);
  GC_Chunk *c = GC_chunks;
  while (c != nullptr) {
    GC_printf("%p: % 10zu  %p..%p  %zuB free\n", c, c->size_class, c->start, c->end, c->remaining_size);
    c = c->next;
  }
  GC_printf("--- GC chunk dump end ---\n");
  dump_counter += 1;
}

static void GC_dump_chunk_objects(GC_Chunk *c) {
  GC_printf("--- GC chunk object dump %p ---\n", c);
  size_t i = 0;
  size_t object_count = ((c->end - c->start) - c->remaining_size) / c->size_class;
  char *p = c->start;
  while (i < object_count) {
    size_t mark_byte_index = i/8;
    size_t mark_bit = 1 << (i & 7);
    bool was_marked = (c->marks[mark_byte_index] & mark_bit) != 0;
    GC_printf("%p: %c\n", p, was_marked ? 'M' : ' ');
    i += 1;
    p += c->size_class;
  }
  GC_printf("--- GC chunk object dump end ---\n");
}

static size_t GC_size_class_for_size(size_t size, size_t alignment) {
  assert(size >= alignment);
  return stdc_bit_ceil(size);
}

static size_t GC_chunk_object_count_for_size_class(size_t size_class) {
  if (size_class <= 1024) {
    return 1<<16;
  }
  return 1;
}

static GC_Chunk *GC_find_chunk_for_pointer(void *p) {
  if (p == nullptr) return nullptr;
  GC_Chunk *c = GC_chunks;
  while (c != nullptr) {
    if ((uintptr_t)(c->start) <= (uintptr_t)(p) && (uintptr_t)(p) < (uintptr_t)(c->end)) {
      return c;
    }
    c = c->next;
  }
  return nullptr;
}

static void *GC_alloc_pages(size_t size, const char *tag) {
  size_t page_size = sysconf(_SC_PAGESIZE);
  size = stdc_bit_ceil(size);
  if (size < page_size) {
    size = page_size;
  }

  void *result = mmap(/*addr=*/nullptr, /*length=*/size, /*prot=*/PROT_READ | PROT_WRITE, /*flags=*/MAP_PRIVATE | MAP_ANONYMOUS, /*fd=*/-1, /*offset=*/0);
  if (result == MAP_FAILED) {
    __builtin_trap();
  }
  int rc = prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (uintptr_t)(result), size, tag);
  if (rc == -1) {
    GC_printf("fatal: gc: prctl: %d\n", errno);
    __builtin_trap();
  }
  return result;
}

static void GC_dealloc_pages(void *p, size_t size) {
  size_t page_size = sysconf(_SC_PAGESIZE);
  size = stdc_bit_ceil(size);
  if (size < page_size) {
    size = page_size;
  }
  int rc = munmap(p, size);
  if (rc == -1) {
    __builtin_trap();
  }
}

static GC_Chunk *GC_find_or_make_chunk(size_t size_class, size_t size_needed) {
  GC_Chunk **c = &GC_chunks;
  while (*c != nullptr) {
    if ((*c)->size_class == size_class && (*c)->remaining_size >= size_needed) {
      return *c;
    }
    c = &(*c)->next;
  }

  size_t object_count = GC_chunk_object_count_for_size_class(size_class);
  GC_Chunk *chunk = GC_alloc_pages(sizeof(GC_Chunk) + (sizeof(uint8_t)*object_count+7)/8, "gcC");
  size_t chunk_total_size = object_count * size_class;
  chunk->remaining_size = chunk_total_size;
  chunk->size_class = size_class;
  chunk->next = nullptr;
  chunk->start = GC_alloc_pages(chunk_total_size, "gcU");
  chunk->end = &chunk->start[chunk_total_size];
  *c = chunk;
  return chunk;
}

typedef struct {
  void **start;
  void **end;
} GC_WorkQueueItem;
typedef struct GC_WorkQueue {
  struct GC_WorkQueue *next;
  size_t count;
  GC_WorkQueueItem items[];
} GC_WorkQueue;
enum { GC_WorkQueueCapacity = 10 };

static GC_WorkQueue *GC_work_queue = nullptr;

static GC_WorkQueueItem GC_work_queue_take() {
  GC_WorkQueue *q = GC_work_queue;
  while (q != nullptr && q->count == 0) {
    GC_work_queue = q->next;
    GC_dealloc_pages(q, sizeof(GC_WorkQueue) + sizeof(void*)*GC_WorkQueueCapacity);
    q = GC_work_queue;
  }
  if (q == nullptr) {
    return (GC_WorkQueueItem){};
  }
  q->count -= 1;
  return q->items[q->count];
}

static void GC_work_queue_add(GC_WorkQueueItem item) {
  GC_WorkQueue *q = GC_work_queue;
  if (q == nullptr || q->count == GC_WorkQueueCapacity) {
    q = GC_alloc_pages(sizeof(GC_WorkQueue) + sizeof(void*)*GC_WorkQueueCapacity, "gcW");
    q->next = GC_work_queue;
    q->count = 0;
    GC_work_queue = q;
  }
  q->items[q->count] = item;
  q->count += 1;
}

static void GC_mark_sweep() {
  // Find roots.
  GC_ProcMaps procmaps;
  GC_procmaps_open(&procmaps);
  while (GC_procmaps_next(&procmaps)) {
    switch (procmaps.maptype) {
    case GC_PROCMAPS_MAPTYPE_OTHER:
      GC_work_queue_add((GC_WorkQueueItem){
          .start = (void **)(procmaps.start),
          .end = (void **)(procmaps.end),
        });
      break;

    case GC_PROCMAPS_MAPTYPE_IGNORE:
    case GC_PROCMAPS_MAPTYPE_CHUNKS:
    case GC_PROCMAPS_MAPTYPE_USERDATA:
    case GC_PROCMAPS_MAPTYPE_WORKQUEUE:
      break;
    }
  }
  GC_procmaps_close(&procmaps);

  // Clear marks.
  GC_Chunk *c = GC_chunks;
  while (c != nullptr) {
    size_t object_count = ((uintptr_t)(c->end) - (uintptr_t)(c->start)) / c->size_class;
    memset(c->marks, 0, (object_count+7)/8);
    c = c->next;
  }

  // Mark.
  for (;;) {
    GC_WorkQueueItem item = GC_work_queue_take();
    if (item.start == nullptr) {
      break;
    }
    // TODO(strager): Worry about other threads unmapping stuff.
    for (void **pp = item.start; pp < item.end; pp += 1) {
      void *p = *pp;
      GC_Chunk *c = GC_find_chunk_for_pointer(p);
      if (c == nullptr) {
        continue;
      }

      uintptr_t start_misalignment = (uintptr_t)(c->start) & (c->size_class - 1);
      uintptr_t object_start = (((uintptr_t)(p) - start_misalignment) & ~(c->size_class - 1)) + start_misalignment;
      size_t object_index = (object_start - (uintptr_t)(c->start)) / c->size_class;

      size_t mark_byte_index = object_index/8;
      size_t mark_bit = 1 << (object_index & 7);
      bool was_marked = (c->marks[mark_byte_index] & mark_bit) != 0;
      if (!was_marked) {
        c->marks[mark_byte_index] |= mark_bit;
        GC_work_queue_add((GC_WorkQueueItem){
            .start = (void **)(object_start),
            .end = (void **)(object_start + c->size_class),
          });
      }
    }
  }
}

void *aligned_alloc(size_t alignment, size_t size) {
  if (size < alignment) size = alignment;
  size_t size_class = GC_size_class_for_size(size, alignment);
  GC_lock();
  GC_Chunk *chunk = GC_find_or_make_chunk(size_class, size);
  char *result = &chunk->end[-chunk->remaining_size];
  chunk->remaining_size -= size_class;

  GC_mark_sweep();
  GC_unlock();
  memset(result, 0, size);
  return result;
}

void *malloc(size_t size) {
  return aligned_alloc(16, size);
}

void *memalign(size_t alignment, size_t size) {
  return aligned_alloc(alignment, size);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
  *memptr = memalign(alignment, size);
  if (*memptr == nullptr) {
    return ENOMEM;
  }
  return 0;
}

void *valloc(size_t size) {
  return memalign(sysconf(_SC_PAGESIZE), size);
}

void *pvalloc(size_t size) {
  (void)(size);
  __builtin_trap();
}

void *calloc(size_t a, size_t b) {
  // TODO(strager): Bounds check.
  return malloc(a * b);
}

size_t malloc_usable_size(void *p) {
  GC_lock();
  GC_Chunk *c = GC_find_chunk_for_pointer(p);
  size_t size_class = c->size_class;
  GC_unlock();
  return size_class;
}

void *realloc(void *p, size_t new_size) {
  if (p == nullptr) {
    return malloc(new_size);
  }
  size_t old_size = malloc_usable_size(p);
  if (old_size >= new_size) {
    return p;
  }
  void *result = malloc(new_size);
  memcpy(result, p, old_size);
  return result;
}

void *reallocarray(void *p, size_t a, size_t b) {
  // TODO(strager): Bounds check.
  return realloc(p, a * b);
}

void free(void *p) {
  (void)(p);
}
