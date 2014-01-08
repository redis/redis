#pragma once

#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum operationType {
    otINVALID = 0,
    otRDB = 1,
    otAOF = 2
} OperationType;

typedef enum operationStatus {
    osUNSTARTED = 0,
    osINPROGRESS = 1,
    osCOMPLETE = 2,
    osFAILED = 3
} OperationStatus;

typedef enum startupStatus {
    ssFAILED = 0,                 // Something went wrong, exit program with error.
    ssCONTINUE_AS_MASTER = 1,     // Master qfork initialization complete, continue as master instance. Call QForkShutdown when exiting.
    ssSLAVE_EXIT = 2              // Slave completed operation. Call QForkShutdown and exit.
} StartupStatus;

#define MAX_GLOBAL_DATA 10000
typedef struct QForkBeginInfo {
    char filename[MAX_PATH];
    BYTE globalData[MAX_GLOBAL_DATA];
    size_t globalDataSize;
    unsigned __int32 dictHashSeed;
} QForkStartupInfo;
    
StartupStatus QForkStartup(int argc, char** argv);
BOOL QForkShutdown();

// For master process use only
BOOL BeginForkOperation(OperationType type, char* fileName, LPVOID globalData, int sizeOfGlobalData, DWORD* childPID, unsigned __int32 dictHashSeed);
OperationStatus GetForkOperationStatus();
BOOL EndForkOperation();
BOOL AbortForkOperation();

// For DLMalloc use only
LPVOID AllocHeapBlock(size_t size, BOOL allocateHigh);
int FreeHeapBlock(LPVOID block, size_t size);

#ifdef QFORK_MAIN_IMPL
int redis_main(int argc, char** argv);
#else
#define main redis_main
#endif

#ifdef __cplusplus
}
#endif
