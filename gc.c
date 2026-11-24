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
} GC_Chunk;

static GC_Chunk *GC_chunks = nullptr;
static pthread_mutex_t GC_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    if (c->start == nullptr) __builtin_trap();  // @@@
    c = c->next;
  }
  GC_printf("--- GC chunk dump end ---\n");
  dump_counter += 1;
}

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

static GC_Chunk *GC_find_chunk_for_pointer(void *p) {
  GC_Chunk *c = GC_chunks;
  while (c != nullptr) {
    if ((uintptr_t)(c->start) <= (uintptr_t)(p) && (uintptr_t)(p) < (uintptr_t)(c->end)) {
      return c;
    }
    c = c->next;
  }
  return nullptr;
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
  if (chunk == MAP_FAILED) {
    __builtin_trap();
  }
  size_t chunk_total_size = GC_chunk_size_for_size_class(size_class);
  chunk->remaining_size = chunk_total_size;
  chunk->size_class = size_class;
  chunk->next = nullptr;
  chunk->start = mmap(/*addr=*/nullptr, /*length=*/chunk_total_size, /*prot=*/PROT_READ | PROT_WRITE, /*flags=*/MAP_PRIVATE | MAP_ANONYMOUS, /*fd=*/-1, /*offset=*/0);
  if (chunk->start == MAP_FAILED) {
    __builtin_trap();
  }
  chunk->end = &chunk->start[chunk_total_size];
  *c = chunk;
  return chunk;
}

void *aligned_alloc(size_t alignment, size_t size) {
  if (size < alignment) size = alignment;
  size_t size_class = GC_size_class_for_size(size, alignment);
  GC_lock();
  GC_Chunk *chunk = GC_find_or_make_chunk(size_class, size);
  char *result = &chunk->end[-chunk->remaining_size];
  chunk->remaining_size -= size_class;
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
