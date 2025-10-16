#define XXH_INLINE_ALL

#include <inttypes.h>
#include <stdio.h>
#include "xxhash.h"

int main() {
	// it seems this has to be exactly 24 bytes.
	union {
		char x[24];
		// force 8-byte alignment without making
		// aliasable with uint64_t.
		void *y[3];
	} data = {.x = "garblegarblegarblegarble"};
	uint64_t hash = XXH64(&data, sizeof(data), 0);
	printf("%016"PRIx64"\n", hash);
	return 0;
}
