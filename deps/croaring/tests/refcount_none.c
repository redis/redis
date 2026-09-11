#include <roaring/portability.h>

int main(void) {
    croaring_refcount_t refcount = 2;

    if (croaring_refcount_dec(&refcount)) return 1;
    if (croaring_refcount_get(&refcount) != 1) return 2;

    croaring_refcount_inc(&refcount);
    if (croaring_refcount_get(&refcount) != 2) return 3;

    if (croaring_refcount_dec(&refcount)) return 4;
    if (!croaring_refcount_dec(&refcount)) return 5;
    if (croaring_refcount_get(&refcount) != 0) return 6;

    return 0;
}
