// Native single-download stub. Python creates the bounded overlay in build_single_exe.py.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum {
    FOOTER_SIZE = 56, ENTRY_SIZE = 44, DIGEST_SIZE = 32, MAX_FILES = 20000,
    MAX_INDEX = 8 * 1024 * 1024, MAX_PAYLOAD = 1024 * 1024 * 1024,
    MAX_RELATIVE = 1023, MAX_PATH_CHARS = 32767, CHUNK_SIZE = 1024 * 1024
};

typedef struct {
    wchar_t *path;
    uint64_t size;
    unsigned char digest[DIGEST_SIZE];
} Entry;

typedef struct {
    HANDLE image;
    BCRYPT_ALG_HANDLE alg;
    uint64_t files_offset;
    DWORD count;
    Entry *entries;
    wchar_t id[2 * DIGEST_SIZE + 1];
    unsigned char *chunk;
} Payload;

static uint64_t le(const unsigned char *bytes, unsigned size) {
    uint64_t value = 0;
    while (size) value = (value << 8) | bytes[--size];
    return value;
}

static int valid_handle(HANDLE h) {
    return h != INVALID_HANDLE_VALUE && h != NULL;
}

static void close_handle(HANDLE h) {
    if (valid_handle(h)) CloseHandle(h);
}

static int seek_to(HANDLE file, uint64_t offset) {
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(file, position, NULL, FILE_BEGIN) != 0;
}

static int read_exact(HANDLE file, void *data, DWORD size) {
    DWORD read = 0;
    return ReadFile(file, data, size, &read, NULL) && read == size;
}

static int ordinary(HANDLE file, int directory) {
    BY_HANDLE_FILE_INFORMATION info;
    return GetFileInformationByHandle(file, &info) &&
           !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           !!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
}

static const wchar_t hex_digits[] = L"0123456789abcdef";

static void hex_string(const unsigned char *bytes, unsigned size, wchar_t *out) {
    for (unsigned i = 0; i < size; ++i) {
        *out++ = hex_digits[bytes[i] >> 4];
        *out++ = hex_digits[bytes[i] & 15];
    }
    *out = 0;
}

static wchar_t *join(const wchar_t *parent, const wchar_t *relative) {
    size_t a = wcslen(parent), b = wcslen(relative);
    int slash = a && parent[a - 1] != L'\\';
    if (a + b + slash + 1 > MAX_PATH_CHARS) return NULL;
    wchar_t *out = malloc((a + b + slash + 1) * sizeof(*out));
    if (!out) return NULL;
    memcpy(out, parent, a * sizeof(*out));
    if (slash) out[a++] = L'\\';
    for (size_t i = 0; i <= b; ++i) out[a + i] = relative[i] == L'/' ? L'\\' : relative[i];
    return out;
}

// Turn an absolute Windows path into an extended path for file-system operations.
static wchar_t *extended(const wchar_t *path) {
    if (!wcsncmp(path, L"\\\\?\\", 4)) return _wcsdup(path);
    const wchar_t *prefix = L"\\\\?\\";
    if (!wcsncmp(path, L"\\\\", 2)) {
        path += 2;
        prefix = L"\\\\?\\UNC\\";
    }
    size_t a = wcslen(prefix), b = wcslen(path);
    if (a + b + 1 > MAX_PATH_CHARS) return NULL;
    wchar_t *result = malloc((a + b + 1) * sizeof(*result));
    if (result) {
        memcpy(result, prefix, a * sizeof(*result));
        memcpy(result + a, path, (b + 1) * sizeof(*result));
    }
    return result;
}

static int is_device(const wchar_t *part, size_t length) {
    size_t stem = 0;
    while (stem < length && part[stem] != L'.') ++stem;
    while (stem && part[stem - 1] == L' ') --stem;
    const wchar_t *labels[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    for (unsigned i = 0; i < sizeof(labels) / sizeof(*labels); ++i) {
        if (wcslen(labels[i]) == stem && !_wcsnicmp(part, labels[i], stem)) return 1;
    }
    return stem == 4 && (!_wcsnicmp(part, L"COM", 3) || !_wcsnicmp(part, L"LPT", 3)) &&
           wcschr(L"123456789\u00b9\u00b2\u00b3", part[3]) != NULL;
}

static int safe_relative(const wchar_t *path) {
    const wchar_t *part = path;
    unsigned depth = 0;
    for (const wchar_t *p = path; ; ++p) {
        wchar_t c = *p;
        if (c == L'/' || !c) {
            size_t length = (size_t)(p - part);
            if (!length || length > 255 || ++depth > 64 || p[-1] == L'.' || p[-1] == L' ' ||
                is_device(part, length)) return 0;
            if (!c) return 1;
            part = p + 1;
        } else if (c < 32 || c == 127 || wcschr(L"<>:\"|?*\\", c)) {
            return 0;
        }
    }
}

static int index_hash(Payload *p, unsigned char *index, ULONG length, const unsigned char *expected) {
    unsigned char digest[DIGEST_SIZE];
    BCRYPT_HASH_HANDLE hash = NULL;
    int ok = BCryptCreateHash(p->alg, &hash, NULL, 0, NULL, 0, 0) >= 0 &&
             BCryptHashData(hash, index, length, 0) >= 0 &&
             BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0 &&
             !memcmp(digest, expected, sizeof(digest));
    if (hash) BCryptDestroyHash(hash);
    return ok;
}

static int read_entries(Payload *p, const unsigned char *index, size_t length, uint64_t remaining) {
    size_t cursor = 0;
    int launcher = 0;
    for (DWORD i = 0; i < p->count; ++i) {
        Entry *entry = &p->entries[i];
        if (length - cursor < ENTRY_SIZE) return 0;
        const unsigned char *row = index + cursor;
        size_t raw = (size_t)le(row, 2);
        entry->size = le(row + 4, 8);
        if (!raw || raw > MAX_RELATIVE * 3 || le(row + 2, 2) ||
            raw > length - cursor - ENTRY_SIZE || entry->size > remaining) return 0;
        memcpy(entry->digest, row + 12, sizeof(entry->digest));
        const char *name = (const char *)row + ENTRY_SIZE;
        if (memchr(name, '\0', raw)) return 0;
        int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, (int)raw, NULL, 0);
        if (!chars || chars > MAX_RELATIVE) return 0;
        entry->path = calloc((size_t)chars + 1, sizeof(*entry->path));
        if (!entry->path || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, (int)raw,
                                                entry->path, chars) != chars ||
            !safe_relative(entry->path) || (i && wcscmp(p->entries[i - 1].path, entry->path) >= 0)) return 0;
        launcher |= !wcscmp(entry->path, L"eowrecomp.exe");
        remaining -= entry->size;
        cursor += ENTRY_SIZE + raw;
    }
    return launcher && cursor == length && remaining == 0;
}

static int payload_open(Payload *p, const wchar_t *self) {
    p->image = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    LARGE_INTEGER size;
    unsigned char footer[FOOTER_SIZE];
    if (!ordinary(p->image, 0) || !GetFileSizeEx(p->image, &size) || size.QuadPart < FOOTER_SIZE ||
        !seek_to(p->image, (uint64_t)size.QuadPart - FOOTER_SIZE) ||
        !read_exact(p->image, footer, sizeof(footer)) || memcmp(footer, "EOWBOOT1", 8)) return 0;
    uint64_t payload_size = le(footer + 8, 8);
    DWORD index_size = (DWORD)le(footer + 16, 4);
    p->count = (DWORD)le(footer + 20, 4);
    if (!p->count || p->count > MAX_FILES || !index_size || index_size > MAX_INDEX ||
        index_size > payload_size || payload_size > MAX_PAYLOAD ||
        (uint64_t)size.QuadPart < payload_size + FOOTER_SIZE + 64) return 0;
    uint64_t start = (uint64_t)size.QuadPart - FOOTER_SIZE - payload_size;
    p->files_offset = start + index_size;
    p->entries = calloc(p->count, sizeof(*p->entries));
    p->chunk = malloc(CHUNK_SIZE);
    unsigned char *index = malloc(index_size);
    int ok = p->entries && p->chunk && index &&
             BCryptOpenAlgorithmProvider(&p->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
             seek_to(p->image, start) && read_exact(p->image, index, index_size) &&
             index_hash(p, index, index_size, footer + 24) &&
             read_entries(p, index, index_size, payload_size - index_size);
    if (ok) hex_string(footer + 24, DIGEST_SIZE, p->id);
    free(index);
    return ok;
}

static void payload_close(Payload *p) {
    close_handle(p->image);
    if (p->alg) BCryptCloseAlgorithmProvider(p->alg, 0);
    if (p->entries) {
        for (DWORD i = 0; i < p->count; ++i) free(p->entries[i].path);
        free(p->entries);
    }
    free(p->chunk);
}

static int transfer_entry(Payload *p, HANDLE source, HANDLE destination, const Entry *entry) {
    unsigned char digest[DIGEST_SIZE];
    BCRYPT_HASH_HANDLE hash = NULL;
    int ok = BCryptCreateHash(p->alg, &hash, NULL, 0, NULL, 0, 0) >= 0;
    for (uint64_t left = entry->size; ok && left; ) {
        DWORD size = left > CHUNK_SIZE ? CHUNK_SIZE : (DWORD)left;
        DWORD written = 0;
        ok = read_exact(source, p->chunk, size) && BCryptHashData(hash, p->chunk, size, 0) >= 0 &&
             (!valid_handle(destination) || (WriteFile(destination, p->chunk, size, &written, NULL) && written == size));
        left -= size;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0 &&
                 !memcmp(digest, entry->digest, sizeof(digest));
    if (hash) BCryptDestroyHash(hash);
    return ok;
}

static int directory(const wchar_t *path, int create) {
    if (create && !CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    int ok = ordinary(handle, 1);
    close_handle(handle);
    return ok;
}

static int parents(wchar_t *target, size_t base) {
    for (wchar_t *p = target + base + 1; *p; ++p) {
        if (*p != L'\\') continue;
        *p = 0;
        int ok = directory(target, 1);
        *p = L'\\';
        if (!ok) return 0;
    }
    return 1;
}

static int entry_search(const Payload *p, const wchar_t *relative, int dir) {
    DWORD lo = 0, hi = p->count;
    while (lo < hi) {
        DWORD mid = lo + (hi - lo) / 2;
        if (wcscmp(p->entries[mid].path, relative) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo == p->count) return 0;
    return dir ? !wcsncmp(p->entries[lo].path, relative, wcslen(relative)) :
                 !wcscmp(p->entries[lo].path, relative);
}

static int check_tree(const Payload *p, const wchar_t *root, const wchar_t *prefix, unsigned depth) {
    if (depth > 64) return 0;
    wchar_t *folder = *prefix ? join(root, prefix) : _wcsdup(root);
    wchar_t *wildcard = folder ? join(folder, L"*") : NULL;
    WIN32_FIND_DATAW data;
    HANDLE find = wildcard ? FindFirstFileW(wildcard, &data) : INVALID_HANDLE_VALUE;
    free(wildcard);
    free(folder);
    if (!valid_handle(find)) return 0;
    int ok = 1;
    do {
        const wchar_t *name = data.cFileName;
        if (!wcscmp(name, L".") || !wcscmp(name, L"..")) continue;
        size_t a = wcslen(prefix), b = wcslen(name);
        int dir = !!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (a + b + dir > MAX_RELATIVE || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            ok = 0;
            break;
        }
        wchar_t relative[MAX_RELATIVE + 1];
        memcpy(relative, prefix, a * sizeof(*relative));
        memcpy(relative + a, name, b * sizeof(*relative));
        if (dir) relative[a + b++] = L'/';
        relative[a + b] = 0;
        ok = entry_search(p, relative, dir) && (!dir || check_tree(p, root, relative, depth + 1));
    } while (ok && FindNextFileW(find, &data));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) ok = 0;
    FindClose(find);
    return ok;
}

static int check_runtime(Payload *p, const wchar_t *root) {
    if (!directory(root, 0) || !check_tree(p, root, L"", 0)) return 0;
    for (DWORD i = 0; i < p->count; ++i) {
        wchar_t *path = join(root, p->entries[i].path);
        HANDLE file = path ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL) : INVALID_HANDLE_VALUE;
        free(path);
        LARGE_INTEGER size;
        int ok = ordinary(file, 0) && GetFileSizeEx(file, &size) &&
                 (uint64_t)size.QuadPart == p->entries[i].size &&
                 transfer_entry(p, file, INVALID_HANDLE_VALUE, &p->entries[i]);
        close_handle(file);
        if (!ok) return 0;
    }
    return 1;
}

static int stage_runtime(Payload *p, const wchar_t *folder) {
    if (!seek_to(p->image, p->files_offset)) return 0;
    for (DWORD i = 0; i < p->count; ++i) {
        const Entry *entry = &p->entries[i];
        wchar_t *path = join(folder, entry->path);
        if (!path || !parents(path, wcslen(folder))) {
            free(path);
            return 0;
        }
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        free(path);
        if (!valid_handle(file)) return 0;
        int ok = transfer_entry(p, p->image, file, entry);
        close_handle(file);
        if (!ok) return 0;
    }
    return 1;
}

// Delete only our randomly named staging tree. Move an invalid prior runtime aside intact.
static void delete_stage(const wchar_t *path, unsigned depth) {
    if (depth > 64 || !directory(path, 0)) return;
    wchar_t *wildcard = join(path, L"*");
    WIN32_FIND_DATAW data;
    HANDLE find = wildcard ? FindFirstFileW(wildcard, &data) : INVALID_HANDLE_VALUE;
    free(wildcard);
    if (valid_handle(find)) {
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            wchar_t *child = join(path, data.cFileName);
            if (!child) continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(child);
                else delete_stage(child, depth + 1);
            } else DeleteFileW(child);
            free(child);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(path);
}

static wchar_t *random_path(const wchar_t *root, const wchar_t *prefix) {
    unsigned char nonce[16];
    wchar_t name[48];
    if (BCryptGenRandom(NULL, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return NULL;
    wcscpy(name, prefix);
    hex_string(nonce, sizeof(nonce), name + wcslen(prefix));
    return join(root, name);
}

static int missing(const wchar_t *path) {
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return 0;
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

static int install_runtime(Payload *p, const wchar_t *root, const wchar_t *version) {
    if (check_runtime(p, version)) return 1;
    wchar_t *stage = random_path(root, L".stage-");
    wchar_t *previous = NULL;
    int owned = stage && CreateDirectoryW(stage, NULL);
    int ok = owned && stage_runtime(p, stage);
    if (ok && !missing(version)) {
        previous = random_path(root, L".replaced-");
        ok = previous && MoveFileExW(version, previous, MOVEFILE_WRITE_THROUGH);
    }
    if (ok) {
        ok = MoveFileExW(stage, version, MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok && previous) MoveFileExW(previous, version, MOVEFILE_WRITE_THROUGH);
    }
    if (!ok && owned) delete_stage(stage, 0);
    free(previous);
    free(stage);
    return ok;
}

static HANDLE lock_runtime(const wchar_t *runtime) {
    wchar_t *path = join(runtime, L".install.lock");
    if (!path) return INVALID_HANDLE_VALUE;
    HANDLE lock = INVALID_HANDLE_VALUE;
    ULONGLONG started = GetTickCount64();
    for (;;) {
        lock = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (valid_handle(lock) || GetLastError() != ERROR_SHARING_VIOLATION ||
            GetTickCount64() - started >= 60000) break;
        Sleep(100);
    }
    free(path);
    if (!ordinary(lock, 0)) {
        close_handle(lock);
        return INVALID_HANDLE_VALUE;
    }
    return lock;
}

static HANDLE launch(const wchar_t *version, const wchar_t *base, const wchar_t *persistent,
                     const wchar_t *args) {
    wchar_t *exe = join(version, L"eowrecomp.exe");
    size_t n = exe ? wcslen(exe) + wcslen(args) + 4 : 0;
    wchar_t *command = n && n <= MAX_PATH_CHARS ? malloc(n * sizeof(*command)) : NULL;
    if (!command) {
        free(exe);
        return NULL;
    }
    swprintf(command, n, L"\"%ls\" %ls", exe, args);
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION child;
    startup.cb = sizeof(startup);
    int created = SetEnvironmentVariableW(L"EOWRECOMP_GAME_SOURCE_DIR", base) &&
                  SetEnvironmentVariableW(L"EOWRECOMP_DATA_ROOT", persistent) &&
                  CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, version, &startup, &child);
    free(command);
    free(exe);
    if (!created) return NULL;
    CloseHandle(child.hThread);
    return child.hProcess;
}

static int failure(const wchar_t *reason, int quiet) {
    if (!quiet) MessageBoxW(NULL, reason, L"EoWRecomp", MB_OK | MB_ICONERROR);
    return 2;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show) {
    (void)instance;
    (void)previous;
    (void)show;
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int verify = argv && argc == 2 && !wcscmp(argv[1], L"--bootstrap-verify");
    if (argv) LocalFree(argv);

    wchar_t *self = calloc(MAX_PATH_CHARS, sizeof(*self));
    DWORD length = self ? GetModuleFileNameW(NULL, self, MAX_PATH_CHARS) : 0;
    wchar_t *self_fs = length && length < MAX_PATH_CHARS ? extended(self) : NULL;
    Payload payload = {0};
    const wchar_t *error = L"The embedded release is damaged or incomplete. Download the executable again.";
    if (!self_fs || !payload_open(&payload, self_fs)) {
        payload_close(&payload);
        free(self_fs);
        free(self);
        return failure(error, verify);
    }
    free(self_fs);
    wchar_t *end = wcsrchr(self, L'\\');
    if (end && end > self && end[-1] == L':') ++end;
    if (end) *end = 0;
    wchar_t *base = end ? extended(self) : NULL;
    wchar_t *runtime = base ? join(base, L"EoWRecomp-runtime") : NULL;
    wchar_t *data = end ? join(self, L"EoWRecomp-data") : NULL;
    wchar_t *data_fs = data ? extended(data) : NULL;
    wchar_t *version_fs = runtime ? join(runtime, payload.id) : NULL;
    wchar_t *runtime_native = end ? join(self, L"EoWRecomp-runtime") : NULL;
    wchar_t *version = runtime_native ? join(runtime_native, payload.id) : NULL;

    error = L"Cannot prepare the adjacent runtime or data folder. Use a writable folder and close any running copies.";
    HANDLE lock = runtime && directory(runtime, 1) ? lock_runtime(runtime) : INVALID_HANDLE_VALUE;
    int ready = valid_handle(lock) && data_fs && directory(data_fs, 1) && version_fs &&
                version && install_runtime(&payload, runtime, version_fs);
    payload_close(&payload);
    HANDLE child = ready && !verify ? launch(version, self, data, args) : NULL;
    close_handle(lock);

    int code = 2;
    DWORD child_code;
    if (!ready) code = failure(error, verify);
    else if (verify) code = 0;
    else if (child && WaitForSingleObject(child, INFINITE) == WAIT_OBJECT_0 &&
             GetExitCodeProcess(child, &child_code)) code = (int)child_code;
    else code = failure(L"Cannot start the prepared EoWRecomp launcher.", verify);
    close_handle(child);

    free(version);
    free(runtime_native);
    free(version_fs);
    free(data_fs);
    free(data);
    free(runtime);
    free(base);
    free(self);
    return code;
}
