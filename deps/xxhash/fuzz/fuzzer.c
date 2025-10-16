#include <stdint.h>
#include "xxhash.h"


int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  volatile XXH64_hash_t hash64 = XXH64(data, size, 0);
  volatile XXH32_hash_t hash32 = XXH32(data, size, 0);
  (void)hash64;
  (void)hash32;
  return 0;
}
