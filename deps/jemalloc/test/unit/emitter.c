#include "test/jemalloc_test.h"
#include "jemalloc/internal/emitter.h"

/*
 * This is so useful for debugging and feature work, we'll leave printing
 * functionality committed but disabled by default.
 */
/* Print the text as it will appear. */
static bool print_raw = false;
/* Print the text escaped, so it can be copied back into the test case. */
static bool print_escaped = false;

typedef struct buf_descriptor_s buf_descriptor_t;
struct buf_descriptor_s {
	char *buf;
	size_t len;
	bool mid_quote;
};

/*
 * Forwards all writes to the passed-in buf_v (which should be cast from a
 * buf_descriptor_t *).
 */
static void
forwarding_cb(void *buf_descriptor_v, const char *str) {
	buf_descriptor_t *buf_descriptor = (buf_descriptor_t *)buf_descriptor_v;

	if (print_raw) {
		malloc_printf("%s", str);
	}
	if (print_escaped) {
		const char *it = str;
		while (*it != '\0') {
			if (!buf_descriptor->mid_quote) {
				malloc_printf("\"");
				buf_descriptor->mid_quote = true;
			}
			switch (*it) {
			case '\\':
				malloc_printf("\\");
				break;
			case '\"':
				malloc_printf("\\\"");
				break;
			case '\t':
				malloc_printf("\\t");
				break;
			case '\n':
				malloc_printf("\\n\"\n");
				buf_descriptor->mid_quote = false;
				break;
			default:
				malloc_printf("%c", *it);
			}
			it++;
		}
	}

	size_t written = malloc_snprintf(buf_descriptor->buf,
	    buf_descriptor->len, "%s", str);
	assert_zu_eq(written, strlen(str), "Buffer overflow!");
	buf_descriptor->buf += written;
	buf_descriptor->len -= written;
	assert_zu_gt(buf_descriptor->len, 0, "Buffer out of space!");
}

static void
assert_emit_output(void (*emit_fn)(emitter_t *),
    const char *expected_json_output, const char *expected_table_output) {
	emitter_t emitter;
	char buf[MALLOC_PRINTF_BUFSIZE];
	buf_descriptor_t buf_descriptor;

	buf_descriptor.buf = buf;
	buf_descriptor.len = MALLOC_PRINTF_BUFSIZE;
	buf_descriptor.mid_quote = false;

	emitter_init(&emitter, emitter_output_json, &forwarding_cb,
	    &buf_descriptor);
	(*emit_fn)(&emitter);
	assert_str_eq(expected_json_output, buf, "json output failure");

	buf_descriptor.buf = buf;
	buf_descriptor.len = MALLOC_PRINTF_BUFSIZE;
	buf_descriptor.mid_quote = false;

	emitter_init(&emitter, emitter_output_table, &forwarding_cb,
	    &buf_descriptor);
	(*emit_fn)(&emitter);
	assert_str_eq(expected_table_output, buf, "table output failure");
}

static void
emit_dict(emitter_t *emitter) {
	bool b_false = false;
	bool b_true = true;
	int i_123 = 123;
	const char *str = "a string";

	emitter_begin(emitter);
	emitter_dict_begin(emitter, "foo", "This is the foo table:");
	emitter_kv(emitter, "abc", "ABC", emitter_type_bool, &b_false);
	emitter_kv(emitter, "def", "DEF", emitter_type_bool, &b_true);
	emitter_kv_note(emitter, "ghi", "GHI", emitter_type_int, &i_123,
	    "note_key1", emitter_type_string, &str);
	emitter_kv_note(emitter, "jkl", "JKL", emitter_type_string, &str,
	    "note_key2", emitter_type_bool, &b_false);
	emitter_dict_end(emitter);
	emitter_end(emitter);
}
static const char *dict_json =
"{\n"
"\t\"foo\": {\n"
"\t\t\"abc\": false,\n"
"\t\t\"def\": true,\n"
"\t\t\"ghi\": 123,\n"
"\t\t\"jkl\": \"a string\"\n"
"\t}\n"
"}\n";
static const char *dict_table =
"This is the foo table:\n"
"  ABC: false\n"
"  DEF: true\n"
"  GHI: 123 (note_key1: \"a string\")\n"
"  JKL: \"a string\" (note_key2: false)\n";

TEST_BEGIN(test_dict) {
	assert_emit_output(&emit_dict, dict_json, dict_table);
}
TEST_END

static void
emit_table_printf(emitter_t *emitter) {
	emitter_begin(emitter);
	emitter_table_printf(emitter, "Table note 1\n");
	emitter_table_printf(emitter, "Table note 2 %s\n",
	    "with format string");
	emitter_end(emitter);
}

static const char *table_printf_json =
"{\n"
"}\n";

static const char *table_printf_table =
"Table note 1\n"
"Table note 2 with format string\n";

TEST_BEGIN(test_table_printf) {
	assert_emit_output(&emit_table_printf, table_printf_json,
	    table_printf_table);
}
TEST_END

static void emit_nested_dict(emitter_t *emitter) {
	int val = 123;
	emitter_begin(emitter);
	emitter_dict_begin(emitter, "json1", "Dict 1");
	emitter_dict_begin(emitter, "json2", "Dict 2");
	emitter_kv(emitter, "primitive", "A primitive", emitter_type_int, &val);
	emitter_dict_end(emitter); /* Close 2 */
	emitter_dict_begin(emitter, "json3", "Dict 3");
	emitter_dict_end(emitter); /* Close 3 */
	emitter_dict_end(emitter); /* Close 1 */
	emitter_dict_begin(emitter, "json4", "Dict 4");
	emitter_kv(emitter, "primitive", "Another primitive",
	    emitter_type_int, &val);
	emitter_dict_end(emitter); /* Close 4 */
	emitter_end(emitter);
}

static const char *nested_dict_json =
"{\n"
"\t\"json1\": {\n"
"\t\t\"json2\": {\n"
"\t\t\t\"primitive\": 123\n"
"\t\t},\n"
"\t\t\"json3\": {\n"
"\t\t}\n"
"\t},\n"
"\t\"json4\": {\n"
"\t\t\"primitive\": 123\n"
"\t}\n"
"}\n";

static const char *nested_dict_table =
"Dict 1\n"
"  Dict 2\n"
"    A primitive: 123\n"
"  Dict 3\n"
"Dict 4\n"
"  Another primitive: 123\n";

TEST_BEGIN(test_nested_dict) {
	assert_emit_output(&emit_nested_dict, nested_dict_json,
	    nested_dict_table);
}
TEST_END

static void
emit_types(emitter_t *emitter) {
	bool b = false;
	int i = -123;
	unsigned u = 123;
	ssize_t zd = -456;
	size_t zu = 456;
	const char *str = "string";
	uint32_t u32 = 789;
	uint64_t u64 = 10000000000ULL;

	emitter_begin(emitter);
	emitter_kv(emitter, "k1", "K1", emitter_type_bool, &b);
	emitter_kv(emitter, "k2", "K2", emitter_type_int, &i);
	emitter_kv(emitter, "k3", "K3", emitter_type_unsigned, &u);
	emitter_kv(emitter, "k4", "K4", emitter_type_ssize, &zd);
	emitter_kv(emitter, "k5", "K5", emitter_type_size, &zu);
	emitter_kv(emitter, "k6", "K6", emitter_type_string, &str);
	emitter_kv(emitter, "k7", "K7", emitter_type_uint32, &u32);
	emitter_kv(emitter, "k8", "K8", emitter_type_uint64, &u64);
	/*
	 * We don't test the title type, since it's only used for tables.  It's
	 * tested in the emitter_table_row tests.
	 */
	emitter_end(emitter);
}

static const char *types_json =
"{\n"
"\t\"k1\": false,\n"
"\t\"k2\": -123,\n"
"\t\"k3\": 123,\n"
"\t\"k4\": -456,\n"
"\t\"k5\": 456,\n"
"\t\"k6\": \"string\",\n"
"\t\"k7\": 789,\n"
"\t\"k8\": 10000000000\n"
"}\n";

static const char *types_table =
"K1: false\n"
"K2: -123\n"
"K3: 123\n"
"K4: -456\n"
"K5: 456\n"
"K6: \"string\"\n"
"K7: 789\n"
"K8: 10000000000\n";

TEST_BEGIN(test_types) {
	assert_emit_output(&emit_types, types_json, types_table);
}
TEST_END

static void
emit_modal(emitter_t *emitter) {
	int val = 123;
	emitter_begin(emitter);
	emitter_dict_begin(emitter, "j0", "T0");
	emitter_json_dict_begin(emitter, "j1");
	emitter_kv(emitter, "i1", "I1", emitter_type_int, &val);
	emitter_json_kv(emitter, "i2", emitter_type_int, &val);
	emitter_table_kv(emitter, "I3", emitter_type_int, &val);
	emitter_table_dict_begin(emitter, "T1");
	emitter_kv(emitter, "i4", "I4", emitter_type_int, &val);
	emitter_json_dict_end(emitter); /* Close j1 */
	emitter_kv(emitter, "i5", "I5", emitter_type_int, &val);
	emitter_table_dict_end(emitter); /* Close T1 */
	emitter_kv(emitter, "i6", "I6", emitter_type_int, &val);
	emitter_dict_end(emitter); /* Close j0 / T0 */
	emitter_end(emitter);
}

const char *modal_json =
"{\n"
"\t\"j0\": {\n"
"\t\t\"j1\": {\n"
"\t\t\t\"i1\": 123,\n"
"\t\t\t\"i2\": 123,\n"
"\t\t\t\"i4\": 123\n"
"\t\t},\n"
"\t\t\"i5\": 123,\n"
"\t\t\"i6\": 123\n"
"\t}\n"
"}\n";

const char *modal_table =
"T0\n"
"  I1: 123\n"
"  I3: 123\n"
"  T1\n"
"    I4: 123\n"
"    I5: 123\n"
"  I6: 123\n";

TEST_BEGIN(test_modal) {
	assert_emit_output(&emit_modal, modal_json, modal_table);
}
TEST_END

static void
emit_json_arr(emitter_t *emitter) {
	int ival = 123;

	emitter_begin(emitter);
	emitter_json_dict_begin(emitter, "dict");
	emitter_json_arr_begin(emitter, "arr");
	emitter_json_arr_obj_begin(emitter);
	emitter_json_kv(emitter, "foo", emitter_type_int, &ival);
	emitter_json_arr_obj_end(emitter); /* Close arr[0] */
	/* arr[1] and arr[2] are primitives. */
	emitter_json_arr_value(emitter, emitter_type_int, &ival);
	emitter_json_arr_value(emitter, emitter_type_int, &ival);
	emitter_json_arr_obj_begin(emitter);
	emitter_json_kv(emitter, "bar", emitter_type_int, &ival);
	emitter_json_kv(emitter, "baz", emitter_type_int, &ival);
	emitter_json_arr_obj_end(emitter); /* Close arr[3]. */
	emitter_json_arr_end(emitter); /* Close arr. */
	emitter_json_dict_end(emitter); /* Close dict. */
	emitter_end(emitter);
}

static const char *json_arr_json =
"{\n"
"\t\"dict\": {\n"
"\t\t\"arr\": [\n"
"\t\t\t{\n"
"\t\t\t\t\"foo\": 123\n"
"\t\t\t},\n"
"\t\t\t123,\n"
"\t\t\t123,\n"
"\t\t\t{\n"
"\t\t\t\t\"bar\": 123,\n"
"\t\t\t\t\"baz\": 123\n"
"\t\t\t}\n"
"\t\t]\n"
"\t}\n"
"}\n";

static const char *json_arr_table = "";

TEST_BEGIN(test_json_arr) {
	assert_emit_output(&emit_json_arr, json_arr_json, json_arr_table);
}
TEST_END

static void
emit_table_row(emitter_t *emitter) {
	emitter_begin(emitter);
	emitter_row_t row;
	emitter_col_t abc = {emitter_justify_left, 10, emitter_type_title};
	abc.str_val = "ABC title";
	emitter_col_t def = {emitter_justify_right, 15, emitter_type_title};
	def.str_val = "DEF title";
	emitter_col_t ghi = {emitter_justify_right, 5, emitter_type_title};
	ghi.str_val = "GHI";

	emitter_row_init(&row);
	emitter_col_init(&abc, &row);
	emitter_col_init(&def, &row);
	emitter_col_init(&ghi, &row);

	emitter_table_row(emitter, &row);

	abc.type = emitter_type_int;
	def.type = emitter_type_bool;
	ghi.type = emitter_type_int;

	abc.int_val = 123;
	def.bool_val = true;
	ghi.int_val = 456;
	emitter_table_row(emitter, &row);

	abc.int_val = 789;
	def.bool_val = false;
	ghi.int_val = 1011;
	emitter_table_row(emitter, &row);

	abc.type = emitter_type_string;
	abc.str_val = "a string";
	def.bool_val = false;
	ghi.type = emitter_type_title;
	ghi.str_val = "ghi";
	emitter_table_row(emitter, &row);

	emitter_end(emitter);
}

static const char *table_row_json =
"{\n"
"}\n";

static const char *table_row_table =
"ABC title       DEF title  GHI\n"
"123                  true  456\n"
"789                 false 1011\n"
"\"a string\"          false  ghi\n";

TEST_BEGIN(test_table_row) {
	assert_emit_output(&emit_table_row, table_row_json, table_row_table);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_dict,
	    test_table_printf,
	    test_nested_dict,
	    test_types,
	    test_modal,
	    test_json_arr,
	    test_table_row);
}
