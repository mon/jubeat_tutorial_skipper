#include <windows.h>

int init(void);

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if(reason == DLL_PROCESS_ATTACH) {
        return init() == 0;
    }
    return TRUE;
}
