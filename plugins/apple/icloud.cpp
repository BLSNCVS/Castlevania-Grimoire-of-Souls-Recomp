// iCloudBundleWrapper: CloudKit save sync. Offline on Windows: iCloud reports
// "no account" so the game keeps its saves local. The small key/value store
// (NSUserDefaults) is kept as files in savedata\userdefaults next to the game.
#include "common.h"

namespace
{
    constexpr int kAccountStatusNoAccount = 3;  // CKAccountStatusNoAccount

    bool KeyPath(const char* name, wchar_t* out, DWORD size, bool create)
    {
        if (!name || !*name)
            return false;
        wchar_t dir[MAX_PATH];
        gosr::GamePath(dir, MAX_PATH, L"savedata");
        if (create)
            CreateDirectoryW(dir, nullptr);
        lstrcatW(dir, L"\\userdefaults");
        if (create)
            CreateDirectoryW(dir, nullptr);
        // keep the key readable but file-name safe
        wchar_t key[128];
        int n = 0;
        for (const char* p = name; *p && n < 120; p++)
        {
            char ch = *p;
            bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
            key[n++] = ok ? wchar_t(ch) : L'_';
        }
        key[n] = 0;
        if (lstrlenW(dir) + n + 6 >= int(size))
            return false;
        lstrcpyW(out, dir);
        lstrcatW(out, L"\\");
        lstrcatW(out, key);
        lstrcatW(out, L".bin");
        return true;
    }
}

GOSR_EXPORT void CrowdKitManager_Initialize() {}
GOSR_EXPORT void CrowdKitManager_Terminate() {}
GOSR_EXPORT int CrowdKitManager_IsAvailableCloud() { return 0; }
GOSR_EXPORT int CrowdKitManager_GetAccountStatus() { return kAccountStatusNoAccount; }
GOSR_EXPORT void CrowdKitManager_RequestUpdteAccountStatus() {}
GOSR_EXPORT int CrowdKitManager_IsUpdatedAccountStatus() { return 1; }
GOSR_EXPORT void CrowdKitManager_FetchAtUploadSlotSaveData(int) {}
GOSR_EXPORT void CrowdKitManager_UploadSlotSaveData(const char*, int) {}
GOSR_EXPORT int CrowdKitManager_DownloadSlotSaveData(int) { return 0; }
GOSR_EXPORT int CrowdKitManager_DeleteSlotSaveData(int) { return 0; }
GOSR_EXPORT void CrowdKitManager_FetchAtUploadSystemSaveData() {}
GOSR_EXPORT void CrowdKitManager_UploadSystemSaveData(const char*) {}
GOSR_EXPORT int CrowdKitManager_DownloadSystemSaveData() { return 0; }
GOSR_EXPORT int CrowdKitManager_DeleteSystemSaveData() { return 0; }
GOSR_EXPORT void CrowdKitManager_SetUploadSlotSaveDataCallback(void*) {}
GOSR_EXPORT void CrowdKitManager_SetFetchAtUploadSlotSaveDataCallback(void*) {}
GOSR_EXPORT void CrowdKitManager_SetDownloadSlotSaveDataCallback(void*) {}
GOSR_EXPORT void CrowdKitManager_SetUploadSystemSaveDataCallback(void*) {}
GOSR_EXPORT void CrowdKitManager_SetFetchAtUploadSystemSaveDataCallback(void*) {}
GOSR_EXPORT void CrowdKitManager_SetDownloadSystemSaveDataCallback(void*) {}
GOSR_EXPORT int CrowdKitManager_GetDownloadSlotSaveDataSlotIndex() { return -1; }

GOSR_EXPORT void CrowdKitManager_WriteToUserDefaultData(const char* name, const unsigned char* data, int size)
{
    wchar_t path[MAX_PATH], temp[MAX_PATH];
    if (!KeyPath(name, path, MAX_PATH, true) || size < 0 || (size > 0 && !data))
        return;
    lstrcpyW(temp, path);
    lstrcatW(temp, L".tmp");
    HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    BOOL ok = size == 0 || (WriteFile(f, data, DWORD(size), &written, nullptr) && written == DWORD(size));
    CloseHandle(f);
    if (ok)
        MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING);
    else
        DeleteFileW(temp);
}

GOSR_EXPORT int CrowdKitManager_ReadFromUserDefaultData(void** outData, int* outSize, const char* name)
{
    if (outData) *outData = nullptr;
    if (outSize) *outSize = 0;
    wchar_t path[MAX_PATH];
    if (!outData || !outSize || !KeyPath(name, path, MAX_PATH, false))
        return 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER size{};
    int result = 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart < (64 << 20))
    {
        DWORD n = DWORD(size.QuadPart), read = 0;
        // the caller copies the bytes out; CoTaskMem matches what a marshaled
        // buffer is freed with, if it ever is
        void* buffer = CoTaskMemAlloc(n ? n : 1);
        if (buffer && (n == 0 || (ReadFile(f, buffer, n, &read, nullptr) && read == n)))
        {
            *outData = buffer;
            *outSize = int(n);
            result = 1;
        }
        else if (buffer)
            CoTaskMemFree(buffer);
    }
    CloseHandle(f);
    return result;
}

GOSR_EXPORT int CrowdKitManager_DeleteUserDefaultData(const char* name)
{
    wchar_t path[MAX_PATH];
    return KeyPath(name, path, MAX_PATH, false) && DeleteFileW(path) ? 1 : 0;
}
