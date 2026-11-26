#include <stddef.h>
#include <stdint.h>

typedef struct GC_Chunk {
  char *start;
  char *end;              // &start[total_size] (one past the end)
  size_t total_size;      // In bytes.
  size_t object_count;    // total_size / size_class
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

typedef struct {
  uint64_t unmanaged_low_count;
  uint64_t unmanaged_high_count;
  uint64_t unmanaged_l1_count;
  uint64_t unmanaged_l2_count;
  uint64_t unmanaged_l3_count;
  uint64_t managed_count;

  uint64_t alloc_count;
  uint64_t gc_count;
  uint64_t alloced_page_bytes_until_next_gc;

  uint64_t total_user_bytes;

  GC_ChunkMapLevel *chunkmap_level1;
  GC_Chunk *chunks;

  GC_WorkQueue *work_queue;
} GC_State;

inline static GC_ObjectTag GC_chunktags_get_object_tag(uint8_t *tags, size_t i) {
  return (GC_ObjectTag)((tags[i/8] >> (i & 7)) & 1);
}

inline static GC_ObjectTag GC_chunk_get_object_tag(GC_Chunk *c, size_t i) {
  return GC_chunktags_get_object_tag(&c->tags[0], i);
}

inline static void GC_chunk_set_object_tag(GC_Chunk *c, size_t i, GC_ObjectTag tag) {
  uint8_t mask = UINT8_C(1) << (i&7);
  uint8_t bit = (uint8_t)(tag) << (i&7);
  c->tags[i/8] = (c->tags[i/8] & ~mask) | bit;
}

inline static size_t GC_chunk_header_size(size_t object_count) {
    return sizeof(GC_Chunk) + (sizeof(uint8_t)*object_count+7)/8;
}
