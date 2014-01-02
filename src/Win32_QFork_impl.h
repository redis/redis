#pragma once

#ifdef __cplusplus
extern "C" {
#endif

    void SetupGlobals(LPVOID globalData, size_t globalDataSize);
    int do_rdbSave(char* filename);
    int do_aofSave(char* filename);

#ifdef __cplusplus
}
#endif