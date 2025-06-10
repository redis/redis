/*
 * Class B Implementation
 * Demonstrates composition and interaction with Class A
 * Implements a data processor that uses Class A for storage
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

// Forward declarations - assuming class_a.c is available
typedef struct ClassA ClassA;

// External ClassA function declarations (from class_a.c)
extern ClassA* ClassA_new(size_t initial_capacity);
extern void ClassA_destroy(ClassA *obj);
extern int ClassA_appendData(ClassA *obj, const char *data, size_t len);
extern size_t ClassA_getSize(ClassA *obj);
extern const char* ClassA_getData(ClassA *obj);
extern int ClassA_verifyIntegrity(ClassA *obj);
extern void ClassA_clear(ClassA *obj);
extern uint32_t ClassA_calculateChecksum(ClassA *obj);
extern void ClassA_debug(ClassA *obj);

// Class B structure
typedef struct ClassB {
    ClassA *storage;        // Composition: ClassB has-a ClassA
    char *processing_buffer;
    size_t buffer_size;
    int processing_mode;    // 0=uppercase, 1=lowercase, 2=reverse
    int is_initialized;
    uint64_t operations_count;
} ClassB;

// Processing modes
#define MODE_UPPERCASE 0
#define MODE_LOWERCASE 1
#define MODE_REVERSE   2
#define MODE_ROT13     3

/**
 * Constructor for Class B
 * @param storage_capacity Initial capacity for internal storage
 * @param processing_mode Initial processing mode
 * @return Pointer to new ClassB instance or NULL on failure
 */
ClassB* ClassB_new(size_t storage_capacity, int processing_mode) {
    ClassB *obj = malloc(sizeof(ClassB));
    if (!obj) {
        return NULL;
    }
    
    // Create internal storage using ClassA
    obj->storage = ClassA_new(storage_capacity);
    if (!obj->storage) {
        free(obj);
        return NULL;
    }
    
    // Allocate processing buffer
    obj->buffer_size = storage_capacity;
    obj->processing_buffer = malloc(obj->buffer_size);
    if (!obj->processing_buffer) {
        ClassA_destroy(obj->storage);
        free(obj);
        return NULL;
    }
    
    obj->processing_mode = processing_mode;
    obj->is_initialized = 1;
    obj->operations_count = 0;
    
    return obj;
}

/**
 * Destructor for Class B
 * @param obj ClassB instance to destroy
 */
void ClassB_destroy(ClassB *obj) {
    if (obj) {
        if (obj->storage) {
            ClassA_destroy(obj->storage);
        }
        if (obj->processing_buffer) {
            free(obj->processing_buffer);
        }
        obj->is_initialized = 0;
        free(obj);
    }
}

/**
 * Set processing mode
 * @param obj ClassB instance
 * @param mode New processing mode
 * @return 1 on success, 0 on failure
 */
int ClassB_setProcessingMode(ClassB *obj, int mode) {
    if (!obj || !obj->is_initialized) {
        return 0;
    }
    
    if (mode < 0 || mode > 3) {
        return 0;  // Invalid mode
    }
    
    obj->processing_mode = mode;
    return 1;
}

/**
 * Process text according to current mode
 * @param obj ClassB instance
 * @param input Input text to process
 * @param len Length of input text
 * @return Pointer to processed text or NULL on failure
 */
char* ClassB_processText(ClassB *obj, const char *input, size_t len) {
    if (!obj || !obj->is_initialized || !input || len == 0) {
        return NULL;
    }
    
    // Ensure processing buffer is large enough
    if (len >= obj->buffer_size) {
        size_t new_size = len + 1;
        char *new_buffer = realloc(obj->processing_buffer, new_size);
        if (!new_buffer) {
            return NULL;
        }
        obj->processing_buffer = new_buffer;
        obj->buffer_size = new_size;
    }
    
    // Copy input to processing buffer
    memcpy(obj->processing_buffer, input, len);
    obj->processing_buffer[len] = '\0';
    
    // Process according to mode
    switch (obj->processing_mode) {
        case MODE_UPPERCASE:
            for (size_t i = 0; i < len; i++) {
                obj->processing_buffer[i] = toupper((unsigned char)obj->processing_buffer[i]);
            }
            break;
            
        case MODE_LOWERCASE:
            for (size_t i = 0; i < len; i++) {
                obj->processing_buffer[i] = tolower((unsigned char)obj->processing_buffer[i]);
            }
            break;
            
        case MODE_REVERSE:
            for (size_t i = 0; i < len / 2; i++) {
                char temp = obj->processing_buffer[i];
                obj->processing_buffer[i] = obj->processing_buffer[len - 1 - i];
                obj->processing_buffer[len - 1 - i] = temp;
            }
            break;
            
        case MODE_ROT13:
            for (size_t i = 0; i < len; i++) {
                char c = obj->processing_buffer[i];
                if (c >= 'a' && c <= 'z') {
                    obj->processing_buffer[i] = ((c - 'a' + 13) % 26) + 'a';
                } else if (c >= 'A' && c <= 'Z') {
                    obj->processing_buffer[i] = ((c - 'A' + 13) % 26) + 'A';
                }
            }
            break;
    }
    
    obj->operations_count++;
    return obj->processing_buffer;
}

/**
 * Store processed data in internal storage
 * **SUBTLE ERROR**: This function has a bug in how it calls ClassA_verifyIntegrity
 * @param obj ClassB instance
 * @param data Data to store
 * @param len Length of data
 * @return 1 on success, 0 on failure
 */
int ClassB_storeData(ClassB *obj, const char *data, size_t len) {
    if (!obj || !obj->is_initialized || !data || len == 0) {
        return 0;
    }
    
    // Store data using ClassA
    if (!ClassA_appendData(obj->storage, data, len)) {
        return 0;
    }
    
    // **BUG**: Calling verifyIntegrity with wrong parameters
    // Should be: ClassA_verifyIntegrity(obj->storage)
    // But we're passing extra wrong parameter:
    if (!ClassA_verifyIntegrity(obj->storage, len)) {  // ERROR: Wrong number of parameters!
        printf("Warning: Data integrity check failed\n");
        return 0;
    }
    
    return 1;
}

/**
 * Process and store text in one operation
 * @param obj ClassB instance
 * @param input Input text
 * @param len Length of input
 * @return 1 on success, 0 on failure
 */
int ClassB_processAndStore(ClassB *obj, const char *input, size_t len) {
    if (!obj || !obj->is_initialized) {
        return 0;
    }
    
    char *processed = ClassB_processText(obj, input, len);
    if (!processed) {
        return 0;
    }
    
    return ClassB_storeData(obj, processed, strlen(processed));
}

/**
 * Get current stored data
 * @param obj ClassB instance
 * @return Pointer to stored data or NULL
 */
const char* ClassB_getStoredData(ClassB *obj) {
    if (!obj || !obj->is_initialized) {
        return NULL;
    }
    
    return ClassA_getData(obj->storage);
}

/**
 * Get size of stored data
 * @param obj ClassB instance
 * @return Size of stored data
 */
size_t ClassB_getStoredSize(ClassB *obj) {
    if (!obj || !obj->is_initialized) {
        return 0;
    }
    
    return ClassA_getSize(obj->storage);
}

/**
 * Clear all stored data
 * @param obj ClassB instance
 */
void ClassB_clear(ClassB *obj) {
    if (obj && obj->is_initialized && obj->storage) {
        ClassA_clear(obj->storage);
        obj->operations_count = 0;
    }
}

/**
 * Get statistics about operations
 * @param obj ClassB instance
 * @return Number of operations performed
 */
uint64_t ClassB_getOperationsCount(ClassB *obj) {
    if (!obj || !obj->is_initialized) {
        return 0;
    }
    
    return obj->operations_count;
}

/**
 * Get current processing mode name
 * @param obj ClassB instance
 * @return String representation of current mode
 */
const char* ClassB_getModeName(ClassB *obj) {
    if (!obj || !obj->is_initialized) {
        return "INVALID";
    }
    
    switch (obj->processing_mode) {
        case MODE_UPPERCASE: return "UPPERCASE";
        case MODE_LOWERCASE: return "lowercase";
        case MODE_REVERSE:   return "REVERSE";
        case MODE_ROT13:     return "ROT13";
        default:             return "UNKNOWN";
    }
}

/**
 * Print debug information for ClassB
 * @param obj ClassB instance
 */
void ClassB_debug(ClassB *obj) {
    if (!obj) {
        printf("ClassB: NULL object\n");
        return;
    }
    
    printf("ClassB Debug Info:\n");
    printf("  Initialized: %s\n", obj->is_initialized ? "Yes" : "No");
    printf("  Processing mode: %s (%d)\n", ClassB_getModeName(obj), obj->processing_mode);
    printf("  Operations count: %llu\n", (unsigned long long)obj->operations_count);
    printf("  Buffer size: %zu\n", obj->buffer_size);
    printf("  Storage size: %zu\n", ClassB_getStoredSize(obj));
    
    if (obj->storage) {
        printf("  Internal ClassA storage:\n");
        ClassA_debug(obj->storage);
    }
}

// Example usage function
#ifdef DEMO_CLASS_B
int main() {
    printf("=== Class B Demo ===\n");
    
    ClassB *processor = ClassB_new(256, MODE_UPPERCASE);
    if (!processor) {
        printf("Failed to create ClassB object\n");
        return 1;
    }
    
    // Test different processing modes
    const char *test_text = "Hello, World! This is a test.";
    
    printf("Original text: %s\n", test_text);
    
    // Test uppercase mode
    ClassB_processAndStore(processor, test_text, strlen(test_text));
    printf("Uppercase result: %s\n", ClassB_getStoredData(processor));
    
    ClassB_clear(processor);
    
    // Test lowercase mode
    ClassB_setProcessingMode(processor, MODE_LOWERCASE);
    ClassB_processAndStore(processor, test_text, strlen(test_text));
    printf("Lowercase result: %s\n", ClassB_getStoredData(processor));
    
    ClassB_clear(processor);
    
    // Test reverse mode
    ClassB_setProcessingMode(processor, MODE_REVERSE);
    ClassB_processAndStore(processor, test_text, strlen(test_text));
    printf("Reverse result: %s\n", ClassB_getStoredData(processor));
    
    ClassB_clear(processor);
    
    // Test ROT13 mode
    ClassB_setProcessingMode(processor, MODE_ROT13);
    ClassB_processAndStore(processor, test_text, strlen(test_text));
    printf("ROT13 result: %s\n", ClassB_getStoredData(processor));
    
    printf("\nFinal debug information:\n");
    ClassB_debug(processor);
    
    ClassB_destroy(processor);
    printf("\nProcessor destroyed successfully\n");
    
    return 0;
}
#endif 
