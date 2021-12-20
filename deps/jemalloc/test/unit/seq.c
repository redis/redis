#include "test/jemalloc_test.h"

#include "jemalloc/internal/seq.h"

typedef struct data_s data_t;
struct data_s {
	int arr[10];
};

static void
set_data(data_t *data, int num) {
	for (int i = 0; i < 10; i++) {
		data->arr[i] = num;
	}
}

static void
assert_data(data_t *data) {
	int num = data->arr[0];
	for (int i = 0; i < 10; i++) {
		assert_d_eq(num, data->arr[i], "Data consistency error");
	}
}

seq_define(data_t, data)

typedef struct thd_data_s thd_data_t;
struct thd_data_s {
	seq_data_t data;
};

static void *
seq_reader_thd(void *arg) {
	thd_data_t *thd_data = (thd_data_t *)arg;
	int iter = 0;
	data_t local_data;
	while (iter < 1000 * 1000 - 1) {
		bool success = seq_try_load_data(&local_data, &thd_data->data);
		if (success) {
			assert_data(&local_data);
			assert_d_le(iter, local_data.arr[0],
			    "Seq read went back in time.");
			iter = local_data.arr[0];
		}
	}
	return NULL;
}

static void *
seq_writer_thd(void *arg) {
	thd_data_t *thd_data = (thd_data_t *)arg;
	data_t local_data;
	memset(&local_data, 0, sizeof(local_data));
	for (int i = 0; i < 1000 * 1000; i++) {
		set_data(&local_data, i);
		seq_store_data(&thd_data->data, &local_data);
	}
	return NULL;
}

TEST_BEGIN(test_seq_threaded) {
	thd_data_t thd_data;
	memset(&thd_data, 0, sizeof(thd_data));

	thd_t reader;
	thd_t writer;

	thd_create(&reader, seq_reader_thd, &thd_data);
	thd_create(&writer, seq_writer_thd, &thd_data);

	thd_join(reader, NULL);
	thd_join(writer, NULL);
}
TEST_END

TEST_BEGIN(test_seq_simple) {
	data_t data;
	seq_data_t seq;
	memset(&seq, 0, sizeof(seq));
	for (int i = 0; i < 1000 * 1000; i++) {
		set_data(&data, i);
		seq_store_data(&seq, &data);
		set_data(&data, 0);
		bool success = seq_try_load_data(&data, &seq);
		assert_b_eq(success, true, "Failed non-racing read");
		assert_data(&data);
	}
}
TEST_END

int main(void) {
	return test_no_reentrancy(
	    test_seq_simple,
	    test_seq_threaded);
}
