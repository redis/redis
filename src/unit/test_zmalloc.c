#include "../zmalloc.c"

int zmalloc_test(int argc, char **argv, int flags) {
    void *ptr;

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    printf("Malloc prefix size: %d\n", (int) PREFIX_SIZE);
    printf("Initial used memory: %zu\n", zmalloc_used_memory());
    ptr = zmalloc(123);
    printf("Allocated 123 bytes; used: %zu\n", zmalloc_used_memory());
    ptr = zrealloc(ptr, 456);
    printf("Reallocated to 456 bytes; used: %zu\n", zmalloc_used_memory());
    zfree(ptr);
    printf("Freed pointer; used: %zu\n", zmalloc_used_memory());
    return 0;
}