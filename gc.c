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

static void GC_printf(const char *format, ...);
#define assert(cond)                                                    \
  do {                                                                  \
    if (!(cond)) {                                                      \
      GC_printf("%s:%d: assert failed: %s\n", __FILE__, __LINE__, #cond); \
      __builtin_trap();                                                 \
    }                                                                   \
  } while (false)

typedef struct GC_Chunk {
  char *start;
  char *end;              // &start[total_size] (one past the end)
  size_t remaining_size;  // In bytes.
  size_t size_class;
  // TODO(strager): Should we remove this linked list?
  struct GC_Chunk *next;
  uint8_t tags[];         // Packed array of GC_ObjectTag-s.
} GC_Chunk;

// 1 bit
typedef enum {
  GC_ObjectTag_Free = 0,
  GC_ObjectTag_Used = 1,
} GC_ObjectTag;

typedef struct GC_ChunkMapLevel {
  union {
    struct GC_ChunkMapLevel *children[];
    struct GC_Chunk *chunks[];
  };
} GC_ChunkMapLevel;
static size_t GC_chunkmaplevel_alloc_size(size_t child_count) {
  return sizeof(GC_Chunk *) * child_count;
}

static uint64_t GC_unmanaged_low_count = 0;
static uint64_t GC_unmanaged_high_count = 0;
static uint64_t GC_unmanaged_l1_count = 0;
static uint64_t GC_unmanaged_l2_count = 0;
static uint64_t GC_unmanaged_l3_count = 0;
static uint64_t GC_managed_count = 0;

static uint64_t GC_alloc_count = 0;
static uint64_t GC_gc_count = 0;

enum {
  GC_ChunkAllocationGranularityBits = 12,
  GC_ChunkAllocationPointerBits = 48,
  GC_ChunkAllocationPointerMax = (UINT64_C(1) << GC_ChunkAllocationPointerBits) - 1,
};

enum {
  GC_ChunkMapLevel4Bits = GC_ChunkAllocationGranularityBits,
  GC_ChunkMapLevel3Bits = 12,
  GC_ChunkMapLevel2Bits = 12,
  GC_ChunkMapLevel1Bits = GC_ChunkAllocationPointerBits - (GC_ChunkMapLevel2Bits+GC_ChunkMapLevel3Bits+GC_ChunkMapLevel4Bits),
};
typedef struct {
  size_t level1;
  size_t level2;
  size_t level3;
} GC_ChunkMapLevelIndexes;
static GC_ChunkMapLevelIndexes GC_chunkmaplevel_indexes(uintptr_t p) {
  GC_ChunkMapLevelIndexes result;
  p >>= GC_ChunkMapLevel4Bits;
  result.level3 = p & ((UINT64_C(1)<<GC_ChunkMapLevel3Bits) - 1); p >>= GC_ChunkMapLevel3Bits;
  result.level2 = p & ((UINT64_C(1)<<GC_ChunkMapLevel2Bits) - 1); p >>= GC_ChunkMapLevel2Bits;
  assert(p < (UINT64_C(1)<<GC_ChunkMapLevel1Bits));
  result.level1 = p;
  return result;
}

static pthread_mutex_t GC_mutex = PTHREAD_MUTEX_INITIALIZER;

static GC_ChunkMapLevel *GC_chunkmap_level1 = nullptr;
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

static GC_ObjectTag GC_chunk_get_object_tag(GC_Chunk *c, size_t i) {
  return (c->tags[i/8] >> (i & 7)) & 1;
}

static void GC_chunk_set_object_tag(GC_Chunk *c, size_t i, GC_ObjectTag tag) {
  uint8_t mask = UINT8_C(1) << (i&7);
  uint8_t bit = (uint8_t)(tag) << (i&7);
  c->tags[i/8] = (c->tags[i/8] & ~mask) | bit;
}

static void GC_dump_chunk_objects(GC_Chunk *c) {
  GC_printf("--- GC chunk object dump %p ---\n", c);
  size_t i = 0;
  size_t object_count = ((c->end - c->start) - c->remaining_size) / c->size_class;
  char *p = c->start;
  while (i < object_count) {
    GC_ObjectTag tag = GC_chunk_get_object_tag(c, i);
    GC_printf("%p: %c\n", p, tag == GC_ObjectTag_Used ? 'M' : ' ');
    i += 1;
    p += c->size_class;
  }
  GC_printf("--- GC chunk object dump end ---\n");
}

static void GC_dump_chunkmap() {
  GC_printf("--- GC chunkmap dump ---\n");
  GC_ChunkMapLevel *level1 = GC_chunkmap_level1;
  if (level1 != nullptr) {
    for (size_t level1_index = 0; level1_index < UINT64_C(1)<< GC_ChunkMapLevel1Bits; level1_index += 1) {
      GC_ChunkMapLevel *level2 = level1->children[level1_index];
      if (level2 != nullptr) {
        uintptr_t level2_total_bits = GC_ChunkMapLevel2Bits+GC_ChunkMapLevel3Bits+GC_ChunkMapLevel4Bits;
        uintptr_t level2_p = level1_index << level2_total_bits;
        GC_printf("- %p..%p\n", (void*)(level2_p), (void*)(level2_p + (UINT64_C(1)<<level2_total_bits)-1));

        for (size_t level2_index = 0; level2_index < UINT64_C(1)<< GC_ChunkMapLevel2Bits; level2_index += 1) {
          GC_ChunkMapLevel *level3 = level2->children[level2_index];
          if (level3 != nullptr) {
            uintptr_t level3_total_bits = GC_ChunkMapLevel3Bits+GC_ChunkMapLevel4Bits;
            uintptr_t level3_p = (level2_index << level3_total_bits) | level2_p;
            GC_printf("  - %p..%p\n", (void*)(level3_p), (void*)(level3_p + (UINT64_C(1)<<level3_total_bits)-1));

            for (size_t level3_index = 0; level3_index < UINT64_C(1)<< GC_ChunkMapLevel3Bits; level3_index += 1) {
              uintptr_t level4_total_bits = GC_ChunkMapLevel4Bits;
              uintptr_t level4_p = (level3_index << level4_total_bits) | level3_p;

              GC_Chunk *chunk = level3->chunks[level3_index];
              if (chunk != nullptr) {
                GC_printf("    - %p..%p  chunk=%p %p..%p\n",
                  (void*)(level4_p), (void*)(level4_p + (UINT64_C(1)<<level4_total_bits)-1),
                  chunk, chunk->start, chunk->end-1);
              }
            }
          }
        }
      }
    }
  }
  GC_printf("--- GC chunkmap dump end ---\n");
}

static size_t GC_size_class_for_size(size_t size, size_t alignment) {
  assert(size >= alignment);
  return stdc_bit_ceil(size);
}

static size_t GC_chunk_object_count_for_size_class(size_t size_class) {
  if (size_class <= (1<<GC_ChunkAllocationGranularityBits)) {
    return (1<<GC_ChunkAllocationGranularityBits)/size_class;
  }
  return 1;
}

static GC_Chunk *GC_find_chunk_for_pointer(void *p) {
  if ((uintptr_t)(p) < (1<<GC_ChunkAllocationGranularityBits)) {
    GC_unmanaged_low_count += 1;
    return nullptr;
  }
  if ((uintptr_t)(p) >= GC_ChunkAllocationPointerMax) {
    GC_unmanaged_high_count += 1;
    return nullptr;
  }
  GC_ChunkMapLevelIndexes indexes = GC_chunkmaplevel_indexes((uintptr_t)(p));

  GC_ChunkMapLevel *level2 = GC_chunkmap_level1->children[indexes.level1];
  if (level2 == nullptr) {
    GC_unmanaged_l1_count += 1;
    return nullptr;
  }
  GC_ChunkMapLevel *level3 = level2->children[indexes.level2];
  if (level3 == nullptr) {
    GC_unmanaged_l2_count += 1;
    return nullptr;
  }
  GC_Chunk *chunk = level3->chunks[indexes.level3];
  if (chunk == nullptr) {
    GC_unmanaged_l3_count += 1;
    return nullptr;
  }
  GC_managed_count += 1;
  return chunk;
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
  assert(chunk_total_size >= (1<<GC_ChunkAllocationGranularityBits));
  chunk->remaining_size = chunk_total_size;
  chunk->size_class = size_class;
  chunk->next = nullptr;
  chunk->start = GC_alloc_pages(chunk_total_size, "gcU");
  chunk->end = &chunk->start[chunk_total_size];
  *c = chunk;

  GC_ChunkMapLevelIndexes chunkmap_start_indexes = GC_chunkmaplevel_indexes((uintptr_t)(chunk->start));
  GC_ChunkMapLevelIndexes chunkmap_end_indexes = GC_chunkmaplevel_indexes((uintptr_t)(chunk->end) - 1);
  if (GC_chunkmap_level1 == nullptr) {
    GC_chunkmap_level1 = GC_alloc_pages(GC_chunkmaplevel_alloc_size(UINT64_C(1)<<GC_ChunkMapLevel1Bits), "gcC");
  }
  for (size_t level1_index = chunkmap_start_indexes.level1; level1_index <= chunkmap_end_indexes.level1; level1_index += 1) {
    GC_ChunkMapLevel **level2 = &GC_chunkmap_level1->children[level1_index];
    if (*level2 == nullptr) {
        *level2 = GC_alloc_pages(GC_chunkmaplevel_alloc_size(UINT64_C(1)<<GC_ChunkMapLevel2Bits), "gcC");
    }
    for (size_t level2_index = chunkmap_start_indexes.level2; level2_index <= chunkmap_end_indexes.level2; level2_index += 1) {
      GC_ChunkMapLevel **level3 = &(*level2)->children[level2_index];
      if (*level3 == nullptr) {
        *level3 = GC_alloc_pages(GC_chunkmaplevel_alloc_size(UINT64_C(1)<<GC_ChunkMapLevel3Bits), "gcC");
      }
      for (size_t level3_index = chunkmap_start_indexes.level3; level3_index <= chunkmap_end_indexes.level3; level3_index += 1) {
        assert((*level3)->chunks[level3_index] == nullptr);
        (*level3)->chunks[level3_index] = chunk;
      }
    }
  }

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
  GC_unmanaged_low_count = 0;
  GC_unmanaged_high_count = 0;
  GC_unmanaged_l1_count = 0;
  GC_unmanaged_l2_count = 0;
  GC_unmanaged_l3_count = 0;
  GC_managed_count = 0;

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

  // Set tags for all objects to GC_ObjectTag_Free.
  GC_Chunk *c = GC_chunks;
  while (c != nullptr) {
    size_t object_count = ((uintptr_t)(c->end) - (uintptr_t)(c->start)) / c->size_class;
    memset(c->tags, 0, (object_count+7)/8);
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

      GC_ObjectTag tag = GC_chunk_get_object_tag(c, object_index);
      if (tag == GC_ObjectTag_Free) {
        GC_chunk_set_object_tag(c, object_index, GC_ObjectTag_Used);
        GC_work_queue_add((GC_WorkQueueItem){
            .start = (void **)(object_start),
            .end = (void **)(object_start + c->size_class),
          });
      }
    }
  }

  if (false) {
    uint64_t unmanaged_count = GC_unmanaged_low_count + GC_unmanaged_high_count + GC_unmanaged_l1_count + GC_unmanaged_l2_count + GC_unmanaged_l3_count;
    GC_printf("scanned pointers: managed=%zu unmanaged=%zu (low=%zu high=%zu l1=%zu l2=%zu l3=%zu), %f%% unmanaged\n",
      GC_managed_count,
      unmanaged_count,
      GC_unmanaged_low_count,
      GC_unmanaged_high_count,
      GC_unmanaged_l1_count,
      GC_unmanaged_l2_count,
      GC_unmanaged_l3_count,
      100.0 * (double)(unmanaged_count) / (double)(GC_managed_count+unmanaged_count));
  }

  GC_gc_count += 1;
}

void *aligned_alloc(size_t alignment, size_t size) {
  if (size < alignment) size = alignment;
  size_t size_class = GC_size_class_for_size(size, alignment);
  GC_lock();
  GC_Chunk *chunk = GC_find_or_make_chunk(size_class, size);
  char *result = &chunk->end[-chunk->remaining_size];
  chunk->remaining_size -= size_class;

  GC_alloc_count += 1;
  if (GC_alloc_count % 64 == 0) {
    GC_mark_sweep();
  }
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
  if (c == nullptr) {
    __builtin_trap();
  }
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
