#include <stddef.h>

extern void *malloc(size_t size);

extern void *calloc(size_t nmemb, size_t size);

extern void *realloc(void *ptr, size_t size);

extern void free(void *ptr);

size_t getMallocSize(void* ptr);

size_t getMetaSize();
