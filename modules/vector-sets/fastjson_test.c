/* fastjson_test.c - Stress test for fastjson.c
 *
 * This performs boundary and corruption tests to ensure
 * the JSON parser handles edge cases without accessing
 * memory outside the bounds of the input.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <setjmp.h>

/* Page size constant - typically 4096 or 16k bytes (Apple Silicon).
 * We use 16k so that it will work on both, but not with Linux huge pages. */
#define PAGE_SIZE 4096*4
#define MAX_JSON_SIZE (PAGE_SIZE - 128)  /* Keep some margin */
#define MAX_FIELD_SIZE 64
#define NUM_TEST_ITERATIONS 100000
#define NUM_CORRUPTION_TESTS 10000
#define NUM_BOUNDARY_TESTS 10000

/* Test state tracking */
static char *safe_page = NULL;       /* Start of readable/writable page */
static char *unsafe_page = NULL;     /* Start of inaccessible guard page */
static int boundary_violation = 0;   /* Flag for boundary violations */
static jmp_buf jmpbuf;               /* For signal handling */
static int tests_passed = 0;
static int tests_failed = 0;
static int corruptions_passed = 0;
static int boundary_tests_passed = 0;

/* Test metadata for tracking */
typedef struct {
    char *json;
    size_t json_len;
    char field[MAX_FIELD_SIZE];
    size_t field_len;
    int expected_result;
} test_case_t;

/* Forward declarations for test JSON generation */
char *generate_random_json(size_t *len, char *field, size_t *field_len, int *has_field);
void corrupt_json(char *json, size_t len);
void setup_test_memory(void);
void cleanup_test_memory(void);
void run_normal_tests(void);
void run_corruption_tests(void);
void run_boundary_tests(void);
void print_test_summary(void);

/* Signal handler for segmentation violations */
static void sigsegv_handler(int sig) {
    boundary_violation = 1;
    printf("Boundary violation detected! Caught signal %d\n", sig);
    longjmp(jmpbuf, 1);
}

/* Wrapper for jsonExtractField to check for boundary violations */
exprtoken *safe_extract_field(const char *json, size_t json_len,
                             const char *field, size_t field_len) {
    boundary_violation = 0;

    if (setjmp(jmpbuf) == 0) {
        return jsonExtractField(json, json_len, field, field_len);
    } else {
        return NULL; /* Return NULL if boundary violation occurred */
    }
}

/* Setup two adjacent memory pages - one readable/writable, one inaccessible */
void setup_test_memory(void) {
    /* Request a page of memory, with specific alignment. We rely on the
     * fact that hopefully the page after that will cause a segfault if
     * accessed. */
    void *region = mmap(NULL, PAGE_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       -1, 0);

    if (region == MAP_FAILED) {
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }

    safe_page = (char*)region;
    unsafe_page = safe_page + PAGE_SIZE;
    // Uncomment to make sure it crashes :D
    // printf("%d\n", unsafe_page[5]);

    /* Set up signal handlers for memory access violations */
    struct sigaction sa;
    sa.sa_handler = sigsegv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

void cleanup_test_memory(void) {
    if (safe_page != NULL) {
        munmap(safe_page, PAGE_SIZE);
        safe_page = NULL;
        unsafe_page = NULL;
    }
}

/* Generate random strings with proper escaping for JSON */
void generate_random_string(char *buffer, size_t max_len) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t len = 1 + rand() % (max_len - 2); /* Ensure at least 1 char */

    for (size_t i = 0; i < len; i++) {
        buffer[i] = charset[rand() % (sizeof(charset) - 1)];
    }
    buffer[len] = '\0';
}

/* Generate random numbers as strings */
void generate_random_number(char *buffer, size_t max_len) {
    double num = (double)rand() / RAND_MAX * 1000.0;

    /* Occasionally make it negative or add decimal places */
    if (rand() % 5 == 0) num = -num;
    if (rand() % 3 != 0) num += (double)(rand() % 100) / 100.0;

    snprintf(buffer, max_len, "%.6g", num);
}

/* Generate a random field name */
void generate_random_field(char *field, size_t *field_len) {
    generate_random_string(field, MAX_FIELD_SIZE / 2);
    *field_len = strlen(field);
}

/* Generate a random JSON object with fields */
char *generate_random_json(size_t *len, char *field, size_t *field_len, int *has_field) {
    char *json = malloc(MAX_JSON_SIZE);
    if (json == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    char buffer[MAX_JSON_SIZE / 4]; /* Buffer for generating values */
    int pos = 0;
    int num_fields = 1 + rand() % 10; /* Random number of fields */
    int target_field_index = rand() % num_fields; /* Which field to return */

    /* Start the JSON object */
    pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "{");

    /* Generate random field/value pairs */
    for (int i = 0; i < num_fields; i++) {
        /* Add a comma if not the first field */
        if (i > 0) {
            pos += snprintf(json + pos, MAX_JSON_SIZE - pos, ", ");
        }

        /* Generate a field name */
        if (i == target_field_index) {
            /* This is our target field - save it for the caller */
            generate_random_field(field, field_len);
            pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "\"%s\": ", field);
            *has_field = 1;
            /* Sometimes change the last char so that it will not match. */
            if (rand() % 2) {
                *has_field = 0;
                field[*field_len-1] = '!';
            }
        } else {
            generate_random_string(buffer, MAX_FIELD_SIZE / 4);
            pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "\"%s\": ", buffer);
        }

        /* Generate a random value type */
        int value_type = rand() % 5;
        switch (value_type) {
            case 0: /* String */
                generate_random_string(buffer, MAX_JSON_SIZE / 8);
                pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "\"%s\"", buffer);
                break;

            case 1: /* Number */
                generate_random_number(buffer, MAX_JSON_SIZE / 8);
                pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "%s", buffer);
                break;

            case 2: /* Boolean: true */
                pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "true");
                break;

            case 3: /* Boolean: false */
                pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "false");
                break;

            case 4: /* Null */
                pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "null");
                break;

            case 5: /* Array (simple) */
                pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "[");
                int array_items = 1 + rand() % 5;
                for (int j = 0; j < array_items; j++) {
                    if (j > 0) pos += snprintf(json + pos, MAX_JSON_SIZE - pos, ", ");

                    /* Array items - either number or string */
                    if (rand() % 2) {
                        generate_random_number(buffer, MAX_JSON_SIZE / 16);
                        pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "%s", buffer);
                    } else {
                        generate_random_string(buffer, MAX_JSON_SIZE / 16);
                        pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "\"%s\"", buffer);
                    }
                }
                pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "]");
                break;
        }
    }

    /* Close the JSON object */
    pos += snprintf(json + pos, MAX_JSON_SIZE - pos, "}");
    *len = pos;

    return json;
}

/* Corrupt JSON by replacing random characters */
void corrupt_json(char *json, size_t len) {
    if (len < 2) return;  /* Too short to corrupt safely */

    /* Corrupt 1-3 characters */
    int num_corruptions = 1 + rand() % 3;
    for (int i = 0; i < num_corruptions; i++) {
        size_t pos = rand() % len;
        char corruption = " \t\n{}[]\":,0123456789abcdefXYZ"[rand() % 30];
        json[pos] = corruption;
    }
}

/* Run standard parser tests with generated valid JSON */
void run_normal_tests(void) {
    printf("Running normal JSON extraction tests...\n");

    for (int i = 0; i < NUM_TEST_ITERATIONS; i++) {
        char field[MAX_FIELD_SIZE] = {0};
        size_t field_len = 0;
        size_t json_len = 0;
        int has_field = 0;

        /* Generate random JSON */
        char *json = generate_random_json(&json_len, field, &field_len, &has_field);

        /* Use valid field to test parser */
        exprtoken *token = safe_extract_field(json, json_len, field, field_len);

        /* Check if we got a token as expected */
        if (has_field && token != NULL) {
            exprTokenRelease(token);
            tests_passed++;
        } else if (!has_field && token == NULL) {
            tests_passed++;
        } else {
            tests_failed++;
        }

        /* Test with a non-existent field */
        char nonexistent_field[MAX_FIELD_SIZE] = "nonexistent_field";
        token = safe_extract_field(json, json_len, nonexistent_field, strlen(nonexistent_field));

        if (token == NULL) {
            tests_passed++;
        } else {
            exprTokenRelease(token);
            tests_failed++;
        }

        free(json);
    }
}

/* Run tests with corrupted JSON */
void run_corruption_tests(void) {
    printf("Running JSON corruption tests...\n");

    for (int i = 0; i < NUM_CORRUPTION_TESTS; i++) {
        char field[MAX_FIELD_SIZE] = {0};
        size_t field_len = 0;
        size_t json_len = 0;
        int has_field = 0;

        /* Generate random JSON */
        char *json = generate_random_json(&json_len, field, &field_len, &has_field);

        /* Make a copy and corrupt it */
        char *corrupted = malloc(json_len + 1);
        if (!corrupted) {
            perror("malloc");
            free(json);
            exit(EXIT_FAILURE);
        }

        memcpy(corrupted, json, json_len + 1);
        corrupt_json(corrupted, json_len);

        /* Test with corrupted JSON */
        exprtoken *token = safe_extract_field(corrupted, json_len, field, field_len);

        /* We're just testing that it doesn't crash or access invalid memory */
        if (boundary_violation) {
            printf("Boundary violation with corrupted JSON!\n");
            tests_failed++;
        } else {
            if (token != NULL) {
                exprTokenRelease(token);
            }
            corruptions_passed++;
        }

        free(corrupted);
        free(json);
    }
}

/* Run tests at memory boundaries */
void run_boundary_tests(void) {
    printf("Running memory boundary tests...\n");

    for (int i = 0; i < NUM_BOUNDARY_TESTS; i++) {
        char field[MAX_FIELD_SIZE] = {0};
        size_t field_len = 0;
        size_t json_len = 0;
        int has_field = 0;

        /* Generate random JSON */
        char *temp_json = generate_random_json(&json_len, field, &field_len, &has_field);

        /* Truncate the JSON to a random length */
        size_t truncated_len = 1 + rand() % json_len;

        /* Place at the edge of the safe page */
        size_t offset = PAGE_SIZE - truncated_len;
        memcpy(safe_page + offset, temp_json, truncated_len);

        /* Test parsing with non-existent field (forcing it to scan to end) */
        char nonexistent_field[MAX_FIELD_SIZE] = "nonexistent_field";
        exprtoken *token = safe_extract_field(safe_page + offset, truncated_len,
                                             nonexistent_field, strlen(nonexistent_field));

        /* We're just testing that it doesn't access memory beyond the boundary */
        if (boundary_violation) {
            printf("Boundary violation at edge of memory page!\n");
            tests_failed++;
        } else {
            if (token != NULL) {
                exprTokenRelease(token);
            }
            boundary_tests_passed++;
        }

        free(temp_json);
    }
}

/* Print summary of test results */
void print_test_summary(void) {
    printf("\n===== FASTJSON PARSER TEST SUMMARY =====\n");
    printf("Normal tests passed: %d/%d\n", tests_passed, NUM_TEST_ITERATIONS * 2);
    printf("Corruption tests passed: %d/%d\n", corruptions_passed, NUM_CORRUPTION_TESTS);
    printf("Boundary tests passed: %d/%d\n", boundary_tests_passed, NUM_BOUNDARY_TESTS);
    printf("Failed tests: %d\n", tests_failed);

    if (tests_failed == 0) {
        printf("\nALL TESTS PASSED! The JSON parser appears to be robust.\n");
    } else {
        printf("\nSome tests FAILED. The JSON parser may be vulnerable.\n");
    }
}

/* Entry point for fastjson parser test */
void run_fastjson_test(void) {
    printf("Starting fastjson parser stress test...\n");

    /* Seed the random number generator */
    srand(time(NULL));

    /* Setup test memory environment */
    setup_test_memory();

    /* Run the various test phases */
    run_normal_tests();
    run_corruption_tests();
    run_boundary_tests();

    /* Print summary */
    print_test_summary();

    /* Cleanup */
    cleanup_test_memory();
}
