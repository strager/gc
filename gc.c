#include <stdlib.h>
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
} GC_Chunk;

static GC_Chunk *GC_chunks = nullptr;

static size_t GC_size_class_for_size(size_t size, size_t alignment) {
  assert(size >= alignment);
  return stdc_bit_ceil(size);
}

static size_t GC_chunk_size_for_size_class(size_t size_class) {
  if (size_class <= 1024) {
    return 1<<16;
  }
  return size_class;
}

static GC_Chunk *GC_find_or_make_chunk(size_t size_class, size_t size_needed) {
  GC_Chunk **c = &GC_chunks;
  while (*c != nullptr) {
    if ((*c)->size_class == size_class && (*c)->remaining_size >= size_needed) {
      return *c;
    }
    c = &(*c)->next;
  }

  GC_Chunk *chunk = mmap(/*addr=*/nullptr, /*length=*/sizeof(GC_Chunk), /*prot=*/PROT_READ | PROT_WRITE, /*flags=*/MAP_PRIVATE | MAP_ANONYMOUS, /*fd=*/-1, /*offset=*/0);
  size_t chunk_total_size = GC_chunk_size_for_size_class(size_class);
  chunk->remaining_size = chunk_total_size;
  chunk->size_class = size_class;
  chunk->next = nullptr;
  chunk->start = mmap(/*addr=*/nullptr, /*length=*/chunk_total_size, /*prot=*/PROT_READ | PROT_WRITE, /*flags=*/MAP_PRIVATE | MAP_ANONYMOUS, /*fd=*/-1, /*offset=*/0);
  chunk->end = &chunk->start[chunk_total_size];
  *c = chunk->next;
  return chunk;
}

void *aligned_alloc(size_t alignment, size_t size) {
  if (size < alignment) size = alignment;
  size_t size_class = GC_size_class_for_size(size, alignment);
  GC_Chunk *chunk = GC_find_or_make_chunk(size_class, size);
  char *result = &chunk->end[-chunk->remaining_size];
  chunk->remaining_size -= size;
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

void *calloc(size_t a, size_t b) {
  // TODO(strager): Bounds check.
  return malloc(a * b);
}

void *realloc(void *p, size_t new_size) {
  __builtin_trap();
}

void *reallocarray(void *p, size_t a, size_t b) {
  __builtin_trap();
}

void free(void *p) {
}
