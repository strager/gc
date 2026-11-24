#include <stdlib.h>
#include <unistd.h>
#include <sys/errno.h>
#include <sys/mman.h>

void *aligned_alloc(size_t alignment, size_t size) {
  return mmap(/*addr=*/nullptr, /*length=*/size, /*prot=*/PROT_READ | PROT_WRITE, /*flags=*/MAP_PRIVATE | MAP_ANONYMOUS, /*fd=*/-1, /*offset=*/0);
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
