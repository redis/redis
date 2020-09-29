#define xstr(s) str(s)
#define str(s) #s

/*
 * Version are at the form of 0x00MMmmpp
 * So each number is at most 1 byte, this means that the max
 * value for each number is 255.
 */
#define REDIS_MAJOR_VERSION 255
#define REDIS_MINOR_VERSION 255
#define REDIS_PATCH_VERSION 255

#define REDIS_VERSION xstr(REDIS_MAJOR_VERSION) "."  xstr(REDIS_MINOR_VERSION) "." xstr(REDIS_PATCH_VERSION)
