/* -*- c-file-style: "ruby"; indent-tabs-mode: nil -*- */
/*
 * hook.c
 *
 * Copyright (C) 2015 Kubo Takehiro <kubo@jiubao.org>
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#define WINDOWS
#include <winsock2.h>
#endif
#include "oci8.h"
#include "plthook.h"
#ifdef __CYGWIN__
#undef boolean /* boolean defined in oratypes.h coflicts with that in windows.h */
#endif
#ifdef WINDOWS
#include <windows.h>
#include <mstcpip.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#ifdef WINDOWS
static CRITICAL_SECTION lock;
#define LOCK(lock) EnterCriticalSection(lock)
#define UNLOCK(lock) LeaveCriticalSection(lock)
typedef int socklen_t;
#else
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK(lock) pthread_mutex_lock(lock)
#define UNLOCK(lock) pthread_mutex_unlock(lock)
#define SOCKET int
#define INVALID_SOCKET (-1)
#define WSAAPI
#endif

#if defined(__APPLE__) && defined(TCP_KEEPALIVE) /* macOS */
#define USE_TCP_KEEPALIVE
#define SUPPORT_TCP_KEEPALIVE_TIME
#elif defined(__sun) && defined(TCP_KEEPALIVE_THRESHOLD) /* Solaris */
#define USE_TCP_KEEPALIVE_THRESHOLD
#define SUPPORT_TCP_KEEPALIVE_TIME
#elif defined(TCP_KEEPIDLE) /* Linux, etc */
#define USE_TCP_KEEPIDLE
#define SUPPORT_TCP_KEEPALIVE_TIME
#elif defined(WINDOWS)
#define SUPPORT_TCP_KEEPALIVE_TIME
#endif

int oci8_cancel_read_at_exit = 0;

#ifdef SUPPORT_TCP_KEEPALIVE_TIME
int oci8_tcp_keepalive_time = 0;
static int WSAAPI hook_setsockopt(SOCKET sockfd, int level, int optname, const void *optval, socklen_t optlen);
#else
int oci8_tcp_keepalive_time = -1;
#endif


typedef struct {
    const char *func_name;
    void *func_addr;
    void *old_func_addr;
} hook_func_entry_t;

typedef struct socket_entry {
    struct socket_entry *next;
    struct socket_entry *prev;
    SOCKET sock;
} socket_entry_t;

static socket_entry_t sockets_in_use = {
    &sockets_in_use, &sockets_in_use, INVALID_SOCKET,
};

static void socket_entry_set(socket_entry_t *entry, SOCKET sock)
{
    LOCK(&lock);
    entry->next = sockets_in_use.next;
    entry->prev = &sockets_in_use;
    sockets_in_use.next->prev = entry;
    sockets_in_use.next = entry;
    entry->sock = sock;
    UNLOCK(&lock);
}

static void socket_entry_clear(socket_entry_t *entry)
{
    LOCK(&lock);
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    UNLOCK(&lock);
}

static int replace_functions(const char * const *files, hook_func_entry_t *functions)
{
    int i;

    for (i = 0; files[i] != NULL; i++) {
        const char *file = files[i];
        plthook_t *ph;
        if (plthook_open(&ph, file) == 0) {
            int j;
            int rv = 0;

            /* install hooks */
            for (j = 0; functions[j].func_name != NULL ; j++) {
                hook_func_entry_t *function = &functions[j];
                rv = plthook_replace(ph, function->func_name, function->func_addr, &function->old_func_addr);
                if (rv != 0) {
                    while (--j >= 0) {
                        /*restore hooked fuction address */
                        plthook_replace(ph, functions[j].func_name, functions[j].old_func_addr, NULL);
                    }
                    plthook_close(ph);
                    rb_raise(rb_eRuntimeError, "Could not replace function %s in %s", function->func_name, file);
                }
            }
            plthook_close(ph);
            return 0;
        }
    }
    return -1;
}

#ifdef WINDOWS

#ifndef _MSC_VER
/* setsockopt() in ws2_32.dll */
#define setsockopt rboci_setsockopt
typedef int (WSAAPI *setsockopt_t)(SOCKET, int, int, const void *, int);
static setsockopt_t setsockopt;
#endif

/* system-wide keepalive interval */
static DWORD keepalive_interval;

static int locK_is_initialized;

static int WSAAPI hook_WSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

static const char * const tcp_func_files[] = {
    /* full client */
    "orantcp12.dll",
    "orantcp11.dll",
    "orantcp10.dll",
    "orantcp9.dll",
    /* instant client basic */
    "oraociei12.dll",
    "oraociei11.dll",
    "oraociei10.dll",
    /* instant client basic lite */
    "oraociicus12.dll",
    "oraociicus11.dll",
    "oraociicus10.dll",
    NULL,
};

static hook_func_entry_t tcp_functions[] = {
    {"WSARecv", (void*)hook_WSARecv, NULL},
    {"setsockopt", (void*)hook_setsockopt, NULL},
    {NULL, NULL, NULL},
};

/* WSARecv() is used for TCP connections */
static int WSAAPI hook_WSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    socket_entry_t entry;
    int enable_cancel = oci8_cancel_read_at_exit;
    int rv;

    if (enable_cancel > 0) {
        socket_entry_set(&entry, s);
    }
    rv = WSARecv(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpOverlapped, lpCompletionRoutine);
    if (enable_cancel > 0) {
        socket_entry_clear(&entry);
    }
    return rv;
}

void oci8_install_hook_functions()
{
    static int hook_functions_installed = 0;
    HKEY hKey;
    DWORD type;
    DWORD data;
    DWORD cbData = sizeof(data);
    const char *reg_key = "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters";

    if (hook_functions_installed) {
        return;
    }

    InitializeCriticalSectionAndSpinCount(&lock, 5000);
    locK_is_initialized = 1;

#ifndef _MSC_VER
    /* Get setsockopt in ws2_32.dll.
     * setsockopt used by mingw compiler isn't same with that in ws2_32.dll.
     */
    setsockopt = (setsockopt_t)GetProcAddress(GetModuleHandleA("WS2_32.DLL"), "setsockopt");
    if (setsockopt == NULL){
        rb_raise(rb_eRuntimeError, "setsockopt isn't found in WS2_32.DLL");
    }
#endif

    /* Get system-wide keepalive interval parameter.
     *  https://technet.microsoft.com/en-us/library/cc957548.aspx
     */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, reg_key, 0, KEY_QUERY_VALUE, &hKey) != 0) {
        rb_raise(rb_eRuntimeError, "failed to open the registry key HKLM\\%s", reg_key);
    }
    keepalive_interval = 1000; /* default value when the following entry isn't found. */
    if (RegQueryValueEx(hKey, "KeepAliveInterval", NULL, &type, (LPBYTE)&data, &cbData) == 0) {
        if (type == REG_DWORD) {
            keepalive_interval = data;
        }
    }
    RegCloseKey(hKey);

    if (replace_functions(tcp_func_files, tcp_functions) != 0) {
        rb_raise(rb_eRuntimeError, "No DLL is found to hook.");
    }
    hook_functions_installed = 1;
}

static void shutdown_socket(socket_entry_t *entry)
{
    /* This is dangerous. But I don't know how to cancel WSARecv().
     * This technique is usable only at the process termination.
     * Otherwise, Oracle client library may close sockets used by
     * others.
     */
    closesocket(entry->sock);
}

#else
static ssize_t hook_read(int fd, void *buf, size_t count);

#ifdef __APPLE__
#define SO_EXT "dylib"
#else
#define SO_EXT "so"
#endif

static const char * const files[] = {
    "libclntsh." SO_EXT ".12.1",
    "libclntsh." SO_EXT ".11.1",
    "libclntsh." SO_EXT ".10.1",
    "libclntsh." SO_EXT ".9.0",
    NULL,
};

static hook_func_entry_t functions[] = {
    {"read", (void*)hook_read, NULL},
#ifdef SUPPORT_TCP_KEEPALIVE_TIME
    {"setsockopt", (void*)hook_setsockopt, NULL},
#endif
    {NULL, NULL, NULL},
};

static ssize_t hook_read(int fd, void *buf, size_t count)
{
    socket_entry_t entry;
    int enable_cancel = oci8_cancel_read_at_exit;
    ssize_t rv;

    if (enable_cancel > 0) {
        socket_entry_set(&entry, fd);
    }
    rv = read(fd, buf, count);
    if (enable_cancel > 0) {
        socket_entry_clear(&entry);
    }
    return rv;
}

void oci8_install_hook_functions(void)
{
    static int hook_functions_installed = 0;

    if (hook_functions_installed) {
        return;
    }
    if (replace_functions(files, functions) != 0) {
        rb_raise(rb_eRuntimeError, "No shared library is found to hook.");
    }
    hook_functions_installed = 1;
}

static void shutdown_socket(socket_entry_t *entry)
{
    shutdown(entry->sock, SHUT_RDWR);
}
#endif

#ifdef SUPPORT_TCP_KEEPALIVE_TIME
static int WSAAPI hook_setsockopt(SOCKET sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    int rv = setsockopt(sockfd, level, optname, optval, optlen);

    if (rv == 0 && level == SOL_SOCKET && optname == SO_KEEPALIVE
        && optlen == sizeof(int) && *(const int*)optval != 0) {
        /* If Oracle client libraries enables keepalive by (ENABLE=BROKEN),
         * set per-connection keepalive socket options to overwrite
         * system-wide setting.
         */
        if (oci8_tcp_keepalive_time > 0) {
#if defined(USE_TCP_KEEPALIVE) /* macOS */
            setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPALIVE, &oci8_tcp_keepalive_time, sizeof(int));
#elif defined(USE_TCP_KEEPALIVE_THRESHOLD) /* Solaris */
            unsigned int millisec = oci8_tcp_keepalive_time * 1000;
            setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPALIVE_THRESHOLD, &millisec, sizeof(millisec));
#elif defined(USE_TCP_KEEPIDLE) /* Linux, etc */
            setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &oci8_tcp_keepalive_time, sizeof(int));
#elif defined(WINDOWS)
            struct tcp_keepalive vals;
            DWORD dummy;

            vals.onoff = 1;
            vals.keepalivetime = oci8_tcp_keepalive_time * 1000;
            vals.keepaliveinterval = keepalive_interval;
            WSAIoctl(sockfd, SIO_KEEPALIVE_VALS, &vals, sizeof(vals), NULL, 0,
                     &dummy, NULL, NULL);
#endif
        }
    }
    return rv;
}
#endif

void oci8_shutdown_sockets(void)
{
    socket_entry_t *entry;

#ifdef WINDOWS
    if (!locK_is_initialized) {
        return;
    }
#endif

    LOCK(&lock);
    for (entry = sockets_in_use.next; entry != &sockets_in_use; entry = entry->next) {
        shutdown_socket(entry);
    }
    UNLOCK(&lock);
}
