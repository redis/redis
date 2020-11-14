#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "sds.h"
#include "util.h"

char *get_null_terminated(size_t size, const uint8_t **data,
                          size_t *total_data_size)
{
  char *tmp = malloc(size+1);
  memcpy(tmp, *data, size);
  tmp[size] = '\0';

  /* Modify the fuzz variables */
  *total_data_size -= size;
  *data += size;

  return tmp;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size){
    // Fuzz sds functions
	char *data_tmp1 = data;
	size_t size_tmp1 = size;
	char *new_str =  get_null_terminated(size_tmp1, &data_tmp1, &size_tmp1);

	sds s = sdsnew(new_str);
	if (s != NULL) {
		sdsfree(s);
	}

	int val;
	sds *s2 = sdssplitargs(new_str, &val);
	if (s2 != NULL) {
		sdsfreesplitres(s2, val);
	}
	if (size > 20) {
		char *data_tmp = data;
		size_t size_tmp = size;
		char *new_str2 = get_null_terminated(19, &data_tmp, &size_tmp);
		char *new_str3 = get_null_terminated(size_tmp, &data_tmp, &size_tmp);

		stringmatch(new_str2, new_str3, 0);
		stringmatch(new_str2, new_str3, 1);
		free(new_str2);
		free(new_str3);
	}

    // Fuzz utility functions
	long long_target;
	long long long_long_target;
	unsigned long long unsigned_target;
	long double long_double_target;
	double double_target;

	string2l((const char*)data, size, &long_long_target);
	string2ll((const char*)data, size, &long_long_target);
	string2ull(new_str, &unsigned_target);
	string2ld((const char*)data, size, &long_double_target);
	string2d((const char*)data, size, &double_target);

	free(new_str);
	return 0;
}
