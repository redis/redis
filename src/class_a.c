/*
 * Class A Implementation
 * Demonstrates object-oriented programming in C
 * Manages a simple data buffer with operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Forward declaration
typedef struct ClassA ClassA;

// Class A structure (private data)
struct ClassA {
    char *data;
    size_t size;
    size_t capacity;
    uint32_t checksum;
    int initialized;
};

// Class A Methods

/**
 * Constructor for Class A
 * @param initial_capacity Initial buffer capacity
 * @return Pointer to new ClassA instance or NULL on failure
 */
ClassA* ClassA_new(size_t initial_capacity) {
    ClassA *obj = malloc(sizeof(ClassA));
    if (!obj) {
        return NULL;
    }
    
    obj->data = malloc(initial_capacity);
    if (!obj->data) {
        free(obj);
        return NULL;
    }
    
    obj->size = 0;
    obj->capacity = initial_capacity;
    obj->checksum = 0;
    obj->initialized = 1;
    
    memset(obj->data, 0, initial_capacity);
    
    return obj;
}

/**
 * Destructor for Class A
 * @param obj ClassA instance to destroy
 */
void ClassA_destroy(ClassA *obj) {
    if (obj) {
        if (obj->data) {
            free(obj->data);
        }
        obj->initialized = 0;
        free(obj);
    }
}

/**
 * Calculate simple checksum for data integrity
 * @param obj ClassA instance
 * @return Updated checksum value
 */
uint32_t ClassA_calculateChecksum(ClassA *obj) {
    if (!obj || !obj->initialized || !obj->data) {
        return 0;
    }
    
    uint32_t sum = 0;
    for (size_t i = 0; i < obj->size; i++) {
        sum += (uint32_t)(unsigned char)obj->data[i];
    }
    
    obj->checksum = sum;
    return sum;
}

/**
 * Append data to the buffer
 * @param obj ClassA instance
 * @param data Data to append
 * @param len Length of data
 * @return 1 on success, 0 on failure
 */
int ClassA_appendData(ClassA *obj, const char *data, size_t len) {
    if (!obj || !obj->initialized || !data || len == 0) {
        return 0;
    }
    
    // Check if we need to resize
    if (obj->size + len > obj->capacity) {
        size_t new_capacity = (obj->size + len) * 2;
        char *new_data = realloc(obj->data, new_capacity);
        if (!new_data) {
            return 0;
        }
        obj->data = new_data;
        obj->capacity = new_capacity;
    }
    
    memcpy(obj->data + obj->size, data, len);
    obj->size += len;
    
    ClassA_calculateChecksum(obj);
    return 1;
}

/**
 * Get current data size
 * @param obj ClassA instance
 * @return Current size of data
 */
size_t ClassA_getSize(ClassA *obj) {
    if (!obj || !obj->initialized) {
        return 0;
    }
    return obj->size;
}

/**
 * Get data pointer (read-only access)
 * @param obj ClassA instance
 * @return Pointer to data or NULL
 */
const char* ClassA_getData(ClassA *obj) {
    if (!obj || !obj->initialized) {
        return NULL;
    }
    return obj->data;
}

/**
 * Verify data integrity using checksum
 * @param obj ClassA instance
 * @return 1 if data is valid, 0 otherwise
 */
int ClassA_verifyIntegrity(ClassA *obj) {
    if (!obj || !obj->initialized) {
        return 0;
    }
    
    uint32_t current_checksum = ClassA_calculateChecksum(obj);
    return (current_checksum == obj->checksum);
}

/**
 * Clear all data from buffer
 * @param obj ClassA instance
 */
void ClassA_clear(ClassA *obj) {
    if (obj && obj->initialized && obj->data) {
        memset(obj->data, 0, obj->capacity);
        obj->size = 0;
        obj->checksum = 0;
    }
}

/**
 * Print object information for debugging
 * @param obj ClassA instance
 */
void ClassA_debug(ClassA *obj) {
    if (!obj) {
        printf("ClassA: NULL object\n");
        return;
    }
    
    printf("ClassA Debug Info:\n");
    printf("  Initialized: %s\n", obj->initialized ? "Yes" : "No");
    printf("  Size: %zu\n", obj->size);
    printf("  Capacity: %zu\n", obj->capacity);
    printf("  Checksum: 0x%08x\n", obj->checksum);
    printf("  Data pointer: %p\n", (void*)obj->data);
    
    if (obj->data && obj->size > 0) {
        printf("  Data preview: \"");
        for (size_t i = 0; i < obj->size && i < 50; i++) {
            char c = obj->data[i];
            if (c >= 32 && c <= 126) {
                printf("%c", c);
            } else {
                printf("\\x%02x", (unsigned char)c);
            }
        }
        if (obj->size > 50) {
            printf("...");
        }
        printf("\"\n");
    }
}

// Example usage function
#ifdef DEMO_CLASS_A
int main() {
    printf("=== Class A Demo ===\n");
    
    ClassA *obj = ClassA_new(64);
    if (!obj) {
        printf("Failed to create ClassA object\n");
        return 1;
    }
    
    // Test basic operations
    ClassA_appendData(obj, "Hello, ", 7);
    ClassA_appendData(obj, "World!", 6);
    
    printf("After appending data:\n");
    ClassA_debug(obj);
    
    printf("\nData content: %s\n", ClassA_getData(obj));
    printf("Data size: %zu\n", ClassA_getSize(obj));
    printf("Integrity check: %s\n", ClassA_verifyIntegrity(obj) ? "PASS" : "FAIL");
    
    // Test large data append
    char large_data[1000];
    memset(large_data, 'X', sizeof(large_data));
    large_data[999] = '\0';
    
    ClassA_appendData(obj, large_data, 999);
    printf("\nAfter appending large data:\n");
    ClassA_debug(obj);
    
    // Clear and test
    ClassA_clear(obj);
    printf("\nAfter clearing:\n");
    ClassA_debug(obj);
    
    ClassA_destroy(obj);
    printf("\nObject destroyed successfully\n");
    
    return 0;
}
#endif 