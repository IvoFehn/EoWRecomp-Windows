#include "discord_presence.h"

#include <windows.h>
#include <process.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef EOWRECOMP_DISCORD_APP_ID
#define EOWRECOMP_DISCORD_APP_ID ""
#endif

// The trailing digit selects an IPC instance. Tests substitute a private prefix at compile time.
#ifndef EOW_DISCORD_PIPE_NAME
#define EOW_DISCORD_PIPE_NAME L"\\\\?\\pipe\\discord-ipc-0"
#endif

enum { RPC_HANDSHAKE, RPC_FRAME, RPC_CLOSE, RPC_PING, RPC_PONG };
enum { RPC_PAYLOAD_MAX = 16384, RPC_IO_MS = 150, RPC_READY_MS = 750, RPC_RETRY_MS = 5000 };

static SRWLOCK presence_lock = SRWLOCK_INIT;
static HANDLE wake_event, worker_thread;
// Zero disables presence and -1 shuts down. Active requests combine Unix seconds and a 16-bit serial
// so an off/on transition in the same second still replaces the previous session.
static volatile LONG64 start_request;
static uint16_t request_serial;
static bool stopped;
static char application_id[21];

static const char *configured_id(void) {
    const char *id = getenv("EOWRECOMP_DISCORD_APP_ID");
    if (!id) id = EOWRECOMP_DISCORD_APP_ID;
    uint64_t value = 0;
    for (const char *p = id; *p; p++) {
        if (*p < '0' || *p > '9' || p - id >= 20) return NULL;
        unsigned digit = (unsigned)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10) return NULL;
        value = value * 10 + digit;
    }
    return value ? id : NULL;
}

bool discord_presence_available(void) { return configured_id() != NULL; }

static LONG64 request_state(void) { return InterlockedCompareExchange64(&start_request, 0, 0); }

static HANDLE open_pipe(LONG64 requested) {
    wchar_t name[] = EOW_DISCORD_PIPE_NAME;
    wchar_t *digit = name + sizeof name / sizeof name[0] - 2;
    for (*digit = L'0'; *digit <= L'9' && request_state() == requested; ++*digit) {
        HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
    }
    return INVALID_HANDLE_VALUE;
}

static bool transfer(HANDLE pipe, HANDLE complete, void *data, DWORD size, bool write,
                     bool interruptible, ULONGLONG deadline) {
    unsigned char *bytes = data;
    while (size) {
        ULONGLONG now = GetTickCount64();
        if (deadline && now >= deadline) return false;
        OVERLAPPED io = {.hEvent = complete};
        ResetEvent(complete);
        BOOL done = write ? WriteFile(pipe, bytes, size, NULL, &io)
                          : ReadFile(pipe, bytes, size, NULL, &io);
        bool canceled = false;
        if (!done) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            HANDLE events[] = {complete, wake_event};
            DWORD wait = deadline ? (DWORD)(deadline - now) : INFINITE;
            if (WaitForMultipleObjects(interruptible ? 2 : 1, events, FALSE, wait) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &io);
                canceled = true;
            }
        }
        DWORD count;
        // Cancellation still owns io and its buffer until the local pipe completes it. If I/O
        // completed before cancellation, preserve the result (the peer may have received it).
        if (!GetOverlappedResult(pipe, &io, &count, canceled) || !count) return false;
        bytes += count;
        size -= count;
        if (canceled && size) return false;
    }
    return true;
}

static bool send_frame(HANDLE pipe, HANDLE complete, uint32_t opcode, const char *body, uint32_t size) {
    if (size > RPC_PAYLOAD_MAX) return false;
    struct {
        uint32_t header[2];
        char payload[RPC_PAYLOAD_MAX];
    } frame;
    frame.header[0] = opcode; // Win32 RPC uses little-endian 32-bit fields.
    frame.header[1] = size;
    memcpy(frame.payload, body, size);
    return transfer(pipe, complete, &frame, sizeof frame.header + size, true, false, GetTickCount64() + RPC_IO_MS);
}

static bool read_frame(HANDLE pipe, HANDLE complete, uint32_t header[2], char *body,
                       bool interruptible, ULONGLONG deadline) {
    // Bound the rest of a frame once the first byte arrives, even during the steady-state wait.
    if (!transfer(pipe, complete, header, 1, false, interruptible, deadline)) return false;
    if (!deadline) deadline = GetTickCount64() + RPC_READY_MS;
    if (!transfer(pipe, complete, (char *)header + 1, 7, false, interruptible, deadline) ||
        header[1] > RPC_PAYLOAD_MAX ||
        !transfer(pipe, complete, body, header[1], false, interruptible, deadline)) return false;
    body[header[1]] = '\0';
    return true;
}

// Only inspect top-level string members. READY also contains unrelated nested user data.
static bool json_member_is(const char *body, const char *key, const char *value) {
    unsigned depth = 0;
    size_t key_size = strlen(key), value_size = strlen(value);
    for (const char *p = body; *p; p++) {
        if (*p == '{' || *p == '[') depth++;
        else if ((*p == '}' || *p == ']') && depth) depth--;
        else if (*p == '"') {
            const char *start = ++p;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (!*p) return false;
            if (depth != 1 || (size_t)(p - start) != key_size || memcmp(start, key, key_size)) continue;
            const char *next = p + 1;
            while (isspace((unsigned char)*next)) next++;
            if (*next++ != ':') continue;
            while (isspace((unsigned char)*next)) next++;
            return *next++ == '"' && !strncmp(next, value, value_size) && next[value_size] == '"';
        }
    }
    return false;
}

static bool await_ready(HANDLE pipe, HANDLE complete, LONG64 requested, char *body) {
    char hello[64];
    int size = snprintf(hello, sizeof hello, "{\"v\":1,\"client_id\":\"%s\"}", application_id);
    if (size < 0 || (size_t)size >= sizeof hello || !send_frame(pipe, complete, RPC_HANDSHAKE, hello, (uint32_t)size))
        return false;
    ULONGLONG deadline = GetTickCount64() + RPC_READY_MS;
    uint32_t header[2];
    while (request_state() == requested && read_frame(pipe, complete, header, body, true, deadline)) {
        if (header[0] == RPC_FRAME && json_member_is(body, "cmd", "DISPATCH") && json_member_is(body, "evt", "READY"))
            return true;
        if (header[0] != RPC_PING || !send_frame(pipe, complete, RPC_PONG, body, header[1])) return false;
    }
    return false;
}

static bool set_activity(HANDLE pipe, HANDLE complete, LONG64 started) {
    char body[192];
    unsigned long pid = GetCurrentProcessId();
    int size = started > 0
        ? snprintf(body, sizeof body, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{\"type\":0,"
                   "\"details\":\"EoWRecomp\",\"timestamps\":{\"start\":%lld}}},\"nonce\":\"1\"}", pid, (long long)started)
        // Discord's original client clears presence by sending SET_ACTIVITY with only the PID.
        : snprintf(body, sizeof body, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu},\"nonce\":\"2\"}", pid);
    return size >= 0 && (size_t)size < sizeof body && send_frame(pipe, complete, RPC_FRAME, body, (uint32_t)size);
}

static void clear_activity(HANDLE pipe, HANDLE complete, char *body) {
    if (!set_activity(pipe, complete, 0)) return;
    // Leave the pipe open for the clear reply briefly, even during host shutdown.
    ULONGLONG deadline = GetTickCount64() + RPC_IO_MS;
    uint32_t header[2];
    while (read_frame(pipe, complete, header, body, false, deadline)) {
        if (header[0] == RPC_CLOSE || (header[0] == RPC_FRAME && json_member_is(body, "nonce", "2"))) return;
    }
}

static unsigned __stdcall presence_worker(void *unused) {
    (void)unused;
    HANDLE complete = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!complete) return 0;
    char body[RPC_PAYLOAD_MAX + 1];
    for (;;) {
        ResetEvent(wake_event);
        LONG64 requested = request_state();
        if (requested < 0) break;
        if (!requested) {
            WaitForSingleObject(wake_event, INFINITE);
            continue;
        }
        HANDLE pipe = open_pipe(requested);
        if (pipe != INVALID_HANDLE_VALUE) {
            if (await_ready(pipe, complete, requested, body) && request_state() == requested &&
                set_activity(pipe, complete, requested >> 16)) {
                uint32_t header[2];
                while (request_state() == requested && read_frame(pipe, complete, header, body, true, 0)) {
                    if (header[0] == RPC_CLOSE || (header[0] == RPC_FRAME && json_member_is(body, "evt", "ERROR"))) break;
                    if (header[0] == RPC_PING && !send_frame(pipe, complete, RPC_PONG, body, header[1])) break;
                }
                if (request_state() != requested) clear_activity(pipe, complete, body);
            }
            CloseHandle(pipe);
        }
        if (request_state() == requested) WaitForSingleObject(wake_event, RPC_RETRY_MS);
    }
    CloseHandle(complete);
    return 0;
}

void discord_presence_update(bool enabled, bool playing) {
    bool active = enabled && playing;
    AcquireSRWLockExclusive(&presence_lock);
    if (!stopped && active != (request_state() > 0)) {
        const char *id = active && !worker_thread ? configured_id() : NULL;
        if (worker_thread || id) {
            LONG64 started = active ? (LONG64)time(NULL) : 0;
            LONG64 request = started > 0 ? (started << 16) | ++request_serial : 0;
            InterlockedExchange64(&start_request, request);
            if (worker_thread) {
                SetEvent(wake_event);
            } else if (atexit(discord_presence_shutdown) == 0) {
                strcpy(application_id, id);
                wake_event = CreateEventW(NULL, TRUE, FALSE, NULL);
                if (wake_event) worker_thread = (HANDLE)_beginthreadex(NULL, 0, presence_worker, NULL, 0, NULL);
                if (!worker_thread && wake_event) CloseHandle(wake_event);
            }
            if (!worker_thread) {
                InterlockedExchange64(&start_request, 0);
                stopped = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&presence_lock);
}

void discord_presence_shutdown(void) {
    AcquireSRWLockExclusive(&presence_lock);
    stopped = true;
    if (worker_thread) {
        InterlockedExchange64(&start_request, -1);
        SetEvent(wake_event);
        // Bound the host exit even if the other process stopped responding mid-frame.
        if (WaitForSingleObject(worker_thread, RPC_READY_MS) == WAIT_OBJECT_0) {
            CloseHandle(worker_thread);
            CloseHandle(wake_event);
            worker_thread = wake_event = NULL;
        }
    }
    ReleaseSRWLockExclusive(&presence_lock);
}
