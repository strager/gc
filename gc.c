#define _XOPEN_SOURCE
#include "gc.h"
#include "procmaps.inc.c"
#include <linux/prctl.h>
#include <stdlib.h>
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

static GC_State GC = {};
static pthread_mutex_t GC_mutex = PTHREAD_MUTEX_INITIALIZER;

enum {
  GC_ChunkAllocationGranularityBits = 12, // Decided by the operating system.
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

static size_t GC_chunkmaplevel_alloc_size(size_t child_count) {
  return sizeof(GC_Chunk *) * child_count;
}

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
  GC_Chunk *c = GC.chunks;
  while (c != nullptr) {
    GC_printf("%p: % 10zu  %p..%p\n", c, c->size_class, c->start, c->end);
    c = c->next;
  }
  GC_printf("--- GC chunk dump end ---\n");
  dump_counter += 1;
}

static void GC_dump_chunk_objects(GC_Chunk *c) {
  GC_printf("--- GC chunk object dump %p ---\n", c);
  size_t i = 0;
  char *p = c->start;
  while (i < c->object_count) {
    GC_ObjectTag tag = GC_chunk_get_object_tag(c, i);
    GC_printf("%p: %c\n", p, tag == GC_ObjectTag_Used ? 'M' : ' ');
    i += 1;
    p += c->size_class;
  }
  GC_printf("--- GC chunk object dump end ---\n");
}

static void GC_dump_chunkmap() {
  GC_printf("--- GC chunkmap dump ---\n");
  GC_ChunkMapLevel *level1 = GC.chunkmap_level1;
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
  size_t min_chunk_size = 1<<16;
  if (size_class <= min_chunk_size) {
    return min_chunk_size/size_class;
  }
  return 1;
}

static GC_Chunk *GC_find_chunk_for_pointer(void *p) {
  if ((uintptr_t)(p) < (1<<GC_ChunkAllocationGranularityBits)) {
    GC.unmanaged_low_count += 1;
    return nullptr;
  }
  if ((uintptr_t)(p) >= GC_ChunkAllocationPointerMax) {
    GC.unmanaged_high_count += 1;
    return nullptr;
  }
  GC_ChunkMapLevelIndexes indexes = GC_chunkmaplevel_indexes((uintptr_t)(p));

  GC_ChunkMapLevel *level2 = GC.chunkmap_level1->children[indexes.level1];
  if (level2 == nullptr) {
    GC.unmanaged_l1_count += 1;
    return nullptr;
  }
  GC_ChunkMapLevel *level3 = level2->children[indexes.level2];
  if (level3 == nullptr) {
    GC.unmanaged_l2_count += 1;
    return nullptr;
  }
  GC_Chunk *chunk = level3->chunks[indexes.level3];
  if (chunk == nullptr) {
    GC.unmanaged_l3_count += 1;
    return nullptr;
  }
  GC.managed_count += 1;
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

static GC_WorkQueueItem GC_work_queue_take() {
  GC_WorkQueue *q = GC.work_queue;
  while (q != nullptr && q->count == 0) {
    GC.work_queue = q->next;
    GC_dealloc_pages(q, sizeof(GC_WorkQueue) + sizeof(void*)*GC_WorkQueueCapacity);
    q = GC.work_queue;
  }
  if (q == nullptr) {
    return (GC_WorkQueueItem){};
  }
  q->count -= 1;
  return q->items[q->count];
}

static void GC_work_queue_add(GC_WorkQueueItem item) {
  GC_WorkQueue *q = GC.work_queue;
  if (q == nullptr || q->count == GC_WorkQueueCapacity) {
    q = GC_alloc_pages(sizeof(GC_WorkQueue) + sizeof(void*)*GC_WorkQueueCapacity, "gcW");
    q->next = GC.work_queue;
    q->count = 0;
    GC.work_queue = q;
  }
  q->items[q->count] = item;
  q->count += 1;
}

static void *GC_object_start_from_chunk_pointer(GC_Chunk *c, void *p) {
  uintptr_t start_misalignment = (uintptr_t)(c->start) & (c->size_class - 1);
  return (void *)((((uintptr_t)(p) - start_misalignment) & ~(c->size_class - 1)) + start_misalignment);
}

static size_t GC_object_index_from_object_start(GC_Chunk *c, void *p) {
  return ((uintptr_t)(p) - (uintptr_t)(c->start)) / c->size_class;
}

static void GC_mark_sweep() {
  if (GC.chunkmap_level1 == nullptr || GC.chunks == nullptr) {
    return;
  }

  struct timespec time_start;
  clock_gettime(CLOCK_MONOTONIC, &time_start);

  size_t live_bytes = 0;

  GC.unmanaged_low_count = 0;
  GC.unmanaged_high_count = 0;
  GC.unmanaged_l1_count = 0;
  GC.unmanaged_l2_count = 0;
  GC.unmanaged_l3_count = 0;
  GC.managed_count = 0;

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
  GC_Chunk *c = GC.chunks;
  while (c != nullptr) {
    memset(c->tags, 0, (c->object_count+7)/8);
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

      void *object_start = GC_object_start_from_chunk_pointer(c, p);
      size_t object_index = GC_object_index_from_object_start(c, object_start);

      GC_ObjectTag tag = GC_chunk_get_object_tag(c, object_index);
      if (tag == GC_ObjectTag_Free) {
        live_bytes += c->size_class;
        GC_chunk_set_object_tag(c, object_index, GC_ObjectTag_Used);
        GC_work_queue_add((GC_WorkQueueItem){
            .start = (void **)(object_start),
            .end = (void **)(object_start + c->size_class),
          });
      }
    }
  }

  if (false) {
    uint64_t unmanaged_count = GC.unmanaged_low_count + GC.unmanaged_high_count + GC.unmanaged_l1_count + GC.unmanaged_l2_count + GC.unmanaged_l3_count;
    GC_printf("scanned pointers: managed=%zu unmanaged=%zu (low=%zu high=%zu l1=%zu l2=%zu l3=%zu), %f%% unmanaged\n",
      GC.managed_count,
      unmanaged_count,
      GC.unmanaged_low_count,
      GC.unmanaged_high_count,
      GC.unmanaged_l1_count,
      GC.unmanaged_l2_count,
      GC.unmanaged_l3_count,
      100.0 * (double)(unmanaged_count) / (double)(GC.managed_count+unmanaged_count));
  }

  if (live_bytes < GC.total_user_bytes*.25) {
    GC.alloced_page_bytes_until_next_gc = 0;
  } else {
    GC.alloced_page_bytes_until_next_gc = live_bytes*.50;
  }

  GC.gc_count += 1;
  struct timespec time_end;
  clock_gettime(CLOCK_MONOTONIC, &time_end);

  struct timespec duration = {};
  duration.tv_sec = time_end.tv_sec - time_start.tv_sec;
  if (time_end.tv_nsec >= time_start.tv_nsec) {
    duration.tv_nsec = time_end.tv_nsec - time_start.tv_nsec;
  } else {
    duration.tv_nsec = 1*1000*1000*1000 - (time_end.tv_nsec - time_start.tv_nsec);
    duration.tv_sec -= 1;
  }
  if (false) {
    GC_printf("GC #%zu took %lu.%06lu seconds after %zu allocations\n",
      GC.gc_count,
      (unsigned long)(duration.tv_sec),
      (unsigned long)(duration.tv_nsec / 1000),
      GC.alloc_count);

    GC_printf("%zu/%zu bytes (%f%%) live\n", live_bytes, GC.total_user_bytes, 100.0 * (double)(live_bytes) / (double)(GC.total_user_bytes));
  }
}

static void *GC_alloc_nogrow(size_t size_class) {
  GC_Chunk *c = GC.chunks;
  while (c != nullptr) {
    if (c->size_class == size_class) {
      for (size_t i = 0; i < (c->object_count+7)/8; i += 1) {
        uint8_t tag_byte = c->tags[i];
        _Static_assert(GC_ObjectTag_Free == 0);
        _Static_assert(GC_ObjectTag_Used == 1);
        if (tag_byte != 0xff) {
          uint8_t bit = stdc_first_trailing_zero(tag_byte) - 1;
          size_t object_index = i*8 + bit;
          if (object_index >= c->object_count) {
            // This happens if object_size==1, for example.
            break;  // Try the next chunk.
          }
          GC_chunk_set_object_tag(c, object_index, GC_ObjectTag_Used);
          return c->start + object_index*size_class;
        }
      }
    }
    c = c->next;
  }
  return nullptr;
}

static void GC_viz_if_needed() {
  static bool checked = false;
  if (checked) return;
  checked = true;

  const char *gcviz = getenv("GCVIZ");
  bool enable_gcviz = gcviz != nullptr && gcviz[0] != '\0' && !!strcmp(gcviz, "0");
  if (enable_gcviz) {
    int memfd = open("/proc/self/mem", O_RDONLY); // inherited
    if (memfd == -1) {
      GC_printf("warn: failed to open our own memory for gcviz\n");
      return;
    }

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if (pid == -1) {
      GC_printf("warn: failed to fork gcviz\n");
      return;
    }
    if (pid == 0) {
      // Child.
      putenv("LD_PRELOAD=");
      putenv("GCVIZ=");
      char memfd_s[32];
      snprintf(memfd_s, sizeof(memfd_s), "%lx", (unsigned long)(memfd));
      char state_s[32];
      snprintf(state_s, sizeof(state_s), "%lx", (unsigned long)(&GC));
      char parent_pid_s[32];
      snprintf(parent_pid_s, sizeof(parent_pid_s), "%lx", (unsigned long)(parent_pid));
      execlp("./gcviz", "gcviz", memfd_s, state_s, parent_pid_s);
      exit(1);
    }
    // Parent.
    close(memfd);
  }
}

void *aligned_alloc(size_t alignment, size_t size) {
  if (size < alignment) size = alignment;
  size_t size_class = GC_size_class_for_size(size, alignment);
  GC_lock();
  GC_viz_if_needed();
  GC.alloc_count += 1;
  char *result = GC_alloc_nogrow(size_class);
  if (result == nullptr) {
    size_t object_count = GC_chunk_object_count_for_size_class(size_class);
    size_t chunk_total_size = object_count * size_class;
    if (chunk_total_size >= GC.alloced_page_bytes_until_next_gc) {
      GC_mark_sweep();  // Updates GC_alloced_page_bytes_until_next_gc.
      result = GC_alloc_nogrow(size_class);
    } else {
      GC.alloced_page_bytes_until_next_gc -= chunk_total_size;
    }
  }
  if (result == nullptr) {
    size_t object_count = GC_chunk_object_count_for_size_class(size_class);
    size_t chunk_total_size = object_count * size_class;
    GC_Chunk *chunk = GC_alloc_pages(GC_chunk_header_size(object_count), "gcC");
    // chunk->tags is filled with GC_ObjectTag_Free.

    assert(chunk_total_size >= (1<<GC_ChunkAllocationGranularityBits));
    chunk->total_size = chunk_total_size;
    chunk->object_count = object_count;
    chunk->size_class = size_class;
    chunk->start = GC_alloc_pages(chunk_total_size, "gcU");
    chunk->end = &chunk->start[chunk_total_size];

    chunk->next = GC.chunks;
    GC.chunks = chunk;
    GC.total_user_bytes += chunk_total_size;

    result = chunk->start;
    GC_chunk_set_object_tag(chunk, 0, GC_ObjectTag_Used);

    GC_ChunkMapLevelIndexes chunkmap_start_indexes = GC_chunkmaplevel_indexes((uintptr_t)(chunk->start));
    GC_ChunkMapLevelIndexes chunkmap_end_indexes = GC_chunkmaplevel_indexes((uintptr_t)(chunk->end) - 1);
    if (GC.chunkmap_level1 == nullptr) {
      GC.chunkmap_level1 = GC_alloc_pages(GC_chunkmaplevel_alloc_size(UINT64_C(1)<<GC_ChunkMapLevel1Bits), "gcC");
    }
    for (size_t level1_index = chunkmap_start_indexes.level1; level1_index <= chunkmap_end_indexes.level1; level1_index += 1) {
      GC_ChunkMapLevel **level2 = &GC.chunkmap_level1->children[level1_index];
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
  }

  assert(result != nullptr);
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
