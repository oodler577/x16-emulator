// Optional host TCP bridge. Copyright (c) 2026 retrodores contributors.
// License: 2-clause BSD, see LICENSE. No incoming listeners are created.
#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif
#include "net_socket.h"
#ifdef __EMSCRIPTEN__
struct net_socket { int unused; };
net_socket *net_socket_create(void) { return NULL; }
void net_socket_destroy(net_socket *n) { (void)n; }
bool net_socket_connect(net_socket *n,const char *h,const char *p) { (void)n;(void)h;(void)p;return false; }
void net_socket_disconnect(net_socket *n) { (void)n; }
int net_socket_state(net_socket *n) { (void)n;return NET_FAILED; }
bool net_socket_established(net_socket *n) { (void)n;return false; }
size_t net_socket_available(net_socket *n) { (void)n;return 0; }
size_t net_socket_space(net_socket *n) { (void)n;return 0; }
int net_socket_read(net_socket *n) { (void)n;return -1; }
bool net_socket_write(net_socket *n,unsigned char v) { (void)n;(void)v;return false; }
#else
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_handle;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
typedef int socket_handle;
#define BAD_SOCKET (-1)
#define close_socket close
#endif
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_SIZE 16384
struct net_socket {
    SDL_mutex *lock;
    SDL_Thread *thread;
    bool stop, request, established;
    unsigned generation;
    int state;
    char host[256], port[6];
    unsigned char rx[QUEUE_SIZE], tx[QUEUE_SIZE];
    size_t rx_head, rx_count, tx_head, tx_count;
};

static bool would_block(void)
{
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
#endif
}

static bool cancelled(net_socket *n, unsigned generation)
{
    SDL_LockMutex(n->lock);
    bool result = n->stop || n->generation != generation;
    SDL_UnlockMutex(n->lock);
    return result;
}

static socket_handle dial(net_socket *n, const char *host, const char *port, unsigned generation)
{
    struct addrinfo hints, *addresses = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    // DNS runs only on this worker, never on the CPU/UI thread.
    if (getaddrinfo(host, port, &hints, &addresses))
        return BAD_SOCKET;
    socket_handle connected = BAD_SOCKET;
    Uint32 deadline = SDL_GetTicks() + 10000;
    for (struct addrinfo *a = addresses; a && !cancelled(n,generation); a = a->ai_next) {
        socket_handle s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == BAD_SOCKET) continue;
#ifdef _WIN32
        u_long nonblocking = 1;
        if (ioctlsocket(s, FIONBIO, &nonblocking)) { close_socket(s); continue; }
#else
        int flags = fcntl(s, F_GETFL, 0);
        if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK)) { close_socket(s); continue; }
        if (s >= FD_SETSIZE) { close_socket(s); continue; }
#ifdef SO_NOSIGPIPE
        int enabled = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
#endif
        int rc = connect(s, a->ai_addr, (int)a->ai_addrlen);
        if (rc == 0) { connected = s; break; }
        if (!would_block()) { close_socket(s); continue; }
        while (!cancelled(n,generation) && (Sint32)(SDL_GetTicks()-deadline) < 0) {
            fd_set output, errors;
            FD_ZERO(&output); FD_ZERO(&errors);
            FD_SET(s,&output); FD_SET(s,&errors);
            struct timeval wait = {0,50000};
            rc = select((int)(s+1),NULL,&output,&errors,&wait);
            if (rc < 0) break;
            if (rc > 0) {
                int error = 0;
#ifdef _WIN32
                int length = sizeof(error);
#else
                socklen_t length = sizeof(error);
#endif
                if (getsockopt(s,SOL_SOCKET,SO_ERROR,(char *)&error,&length) == 0 && error == 0)
                    connected = s;
                break;
            }
        }
        if (connected != BAD_SOCKET) break;
        close_socket(s);
    }
    freeaddrinfo(addresses);
    return connected;
}

static int worker(void *argument)
{
    net_socket *n = argument;
    socket_handle socket_fd = BAD_SOCKET;
    unsigned generation = 0;
    for (;;) {
        char host[256], port[6];
        bool request;
        SDL_LockMutex(n->lock);
        if (n->stop) { SDL_UnlockMutex(n->lock); break; }
        if (generation != n->generation) {
            if (socket_fd != BAD_SOCKET) close_socket(socket_fd);
            socket_fd = BAD_SOCKET;
            generation = n->generation;
        }
        request = n->request;
        if (request) {
            memcpy(host,n->host,sizeof(host)); memcpy(port,n->port,sizeof(port));
            n->request = false;
        }
        SDL_UnlockMutex(n->lock);
        if (request) {
            socket_fd = dial(n,host,port,generation);
            SDL_LockMutex(n->lock);
            if (n->generation == generation && !n->stop) {
                n->state = socket_fd == BAD_SOCKET ? NET_FAILED : NET_CONNECTED;
                n->established = socket_fd != BAD_SOCKET;
            }
            SDL_UnlockMutex(n->lock);
            continue;
        }
        if (socket_fd == BAD_SOCKET) { SDL_Delay(1); continue; }

        fd_set input, output;
        FD_ZERO(&input); FD_ZERO(&output);
        SDL_LockMutex(n->lock);
        bool want_input=n->rx_count < QUEUE_SIZE;
        bool want_output=n->tx_count != 0;
        if (want_input) FD_SET(socket_fd,&input);
        if (want_output) FD_SET(socket_fd,&output);
        SDL_UnlockMutex(n->lock);
        // Winsock rejects select with no descriptors. A full RX queue needs
        // guest backpressure, not a synthetic connection failure.
        if (!want_input && !want_output) { SDL_Delay(1); continue; }
        struct timeval wait = {0,1000};
        int ready = select((int)(socket_fd+1),&input,&output,NULL,&wait);
        if (cancelled(n,generation)) continue;
        bool failed = ready < 0;
        SDL_LockMutex(n->lock);
        // A reset/dial can supersede this worker between the two lock sections.
        if (n->generation != generation) { SDL_UnlockMutex(n->lock); continue; }
        if (ready > 0 && FD_ISSET(socket_fd,&output) && n->tx_count) {
            size_t length = QUEUE_SIZE - n->tx_head;
            if (length > n->tx_count) length = n->tx_count;
#ifdef MSG_NOSIGNAL
            int flags = MSG_NOSIGNAL;
#else
            int flags = 0;
#endif
            int sent = send(socket_fd,(const char *)&n->tx[n->tx_head],(int)length,flags);
            if (sent > 0) { n->tx_head=(n->tx_head+(size_t)sent)%QUEUE_SIZE; n->tx_count-=(size_t)sent; }
            else if (sent == 0 || !would_block()) failed = true;
        }
        if (ready > 0 && FD_ISSET(socket_fd,&input) && n->rx_count < QUEUE_SIZE) {
            size_t tail=(n->rx_head+n->rx_count)%QUEUE_SIZE;
            size_t length=QUEUE_SIZE-tail;
            if (length > QUEUE_SIZE-n->rx_count) length=QUEUE_SIZE-n->rx_count;
            int got=recv(socket_fd,(char *)&n->rx[tail],(int)length,0);
            if (got > 0) n->rx_count+=(size_t)got;
            else if (got == 0 || !would_block()) failed=true;
        }
        if (failed) {
            close_socket(socket_fd);
            socket_fd=BAD_SOCKET;
            n->state=NET_CLOSED; // preserve queued RX through EOF
            n->tx_count=0;
        }
        SDL_UnlockMutex(n->lock);
    }
    if (socket_fd != BAD_SOCKET) close_socket(socket_fd);
    return 0;
}

net_socket *net_socket_create(void)
{
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2,2),&data)) return NULL;
#endif
    net_socket *n=calloc(1,sizeof(*n));
    if (!n) goto fail;
    n->lock=SDL_CreateMutex();
    if (!n->lock) { free(n); goto fail; }
    n->thread=SDL_CreateThread(worker,"x16-serial-tcp",n);
    if (!n->thread) { SDL_DestroyMutex(n->lock); free(n); goto fail; }
    return n;
fail:
#ifdef _WIN32
    WSACleanup();
#endif
    return NULL;
}
void net_socket_destroy(net_socket *n)
{
    if (!n) return;
    SDL_LockMutex(n->lock); n->stop=true; n->generation++; SDL_UnlockMutex(n->lock);
    SDL_WaitThread(n->thread,NULL);
    SDL_DestroyMutex(n->lock);
    free(n);
#ifdef _WIN32
    WSACleanup();
#endif
}
bool net_socket_connect(net_socket *n,const char *host,const char *port)
{
    if (!n || !host || !port || strlen(host)>=sizeof(n->host) || strlen(port)>=sizeof(n->port)) return false;
    SDL_LockMutex(n->lock);
    n->established=false; n->generation++; n->request=true; n->state=NET_DIALING;
    strcpy(n->host,host); strcpy(n->port,port);
    n->rx_head=n->rx_count=n->tx_head=n->tx_count=0;
    SDL_UnlockMutex(n->lock);
    return true;
}
void net_socket_disconnect(net_socket *n)
{
    if (!n) return;
    SDL_LockMutex(n->lock);
    n->established=false; n->generation++; n->request=false; n->state=NET_IDLE;
    n->rx_head=n->rx_count=n->tx_head=n->tx_count=0;
    SDL_UnlockMutex(n->lock);
}
int net_socket_state(net_socket *n)
{
    if (!n) return NET_IDLE;
    SDL_LockMutex(n->lock); int v=n->state; SDL_UnlockMutex(n->lock); return v;
}
bool net_socket_established(net_socket *n)
{
    if (!n) return false;
    SDL_LockMutex(n->lock); bool v=n->established; SDL_UnlockMutex(n->lock); return v;
}
size_t net_socket_available(net_socket *n)
{
    if (!n) return 0;
    SDL_LockMutex(n->lock); size_t v=n->rx_count; SDL_UnlockMutex(n->lock); return v;
}
size_t net_socket_space(net_socket *n)
{
    if (!n) return 0;
    SDL_LockMutex(n->lock); size_t v=QUEUE_SIZE-n->tx_count; SDL_UnlockMutex(n->lock); return v;
}
int net_socket_read(net_socket *n)
{
    if (!n) return -1;
    SDL_LockMutex(n->lock);
    int value=-1;
    if (n->rx_count) {
        value=n->rx[n->rx_head]; n->rx_head=(n->rx_head+1)%QUEUE_SIZE; n->rx_count--;
    }
    SDL_UnlockMutex(n->lock); return value;
}
bool net_socket_write(net_socket *n,unsigned char value)
{
    if (!n) return false;
    SDL_LockMutex(n->lock);
    bool ok=n->state==NET_CONNECTED && n->tx_count<QUEUE_SIZE;
    if (ok) { n->tx[(n->tx_head+n->tx_count)%QUEUE_SIZE]=value; n->tx_count++; }
    SDL_UnlockMutex(n->lock); return ok;
}
#endif
