// Optional host TCP bridge. Copyright (c) 2026 retrodores contributors.
// License: 2-clause BSD, see LICENSE. Network I/O stays on the worker thread.
#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif
#include "net_socket.h"
#include "net_serial.h"
#ifdef __EMSCRIPTEN__
struct net_socket { int unused; };
net_socket *net_socket_create(void) { return NULL; }
void net_socket_destroy(net_socket *n) { (void)n; }
bool net_socket_connect(net_socket *n,const char *h,const char *p) { (void)n;(void)h;(void)p;return false; }
bool net_socket_open(net_socket *n,const char *h,const char *p,int m,const char *u,const char *s) { (void)n;(void)h;(void)p;(void)m;(void)u;(void)s;return false; }
unsigned char *net_socket_download(net_socket *n,size_t *l) { (void)n;*l=0;return NULL; }
void net_socket_serial_config(net_socket *n,unsigned b,unsigned l,unsigned m) { (void)n;(void)b;(void)l;(void)m; }
unsigned net_socket_modem_lines(net_socket *n) { (void)n;return 0; }
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
#include <curl/curl.h>
#include <libssh2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_SIZE 16384
struct net_socket {
    SDL_mutex *lock;
    SDL_Thread *thread;
    bool stop, request, established;
    unsigned generation;
    int state;
    char host[2048], port[6], user[128], password[256];
    int mode;
    unsigned baud,lcr,mcr,modem_lines;
    bool serial_changed;
    unsigned char *download;
    size_t download_length;
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

static socket_handle dial(net_socket *n, const char *host, const char *port, unsigned generation, int mode)
{
    struct addrinfo hints, *addresses = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = mode == NET_UDP ? SOCK_DGRAM : SOCK_STREAM;
    if (mode == NET_LISTEN) hints.ai_flags = AI_PASSIVE;
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
        if (mode == NET_LISTEN) {
            int one=1;
            setsockopt(s,SOL_SOCKET,SO_REUSEADDR,(char *)&one,sizeof(one));
            if (!bind(s,a->ai_addr,(int)a->ai_addrlen) && !listen(s,1)) connected=s;
            else close_socket(s);
            if (connected != BAD_SOCKET) break;
            continue;
        }
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

typedef struct {
    net_socket *net;
    unsigned generation;
    unsigned char *data;
    size_t length;
} transfer;
static int progress(void *arg,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d)
{
    (void)a;(void)b;(void)c;(void)d;
    transfer *t=arg;
    return cancelled(t->net,t->generation);
}
static size_t download_data(char *data,size_t size,size_t count,void *arg)
{
    transfer *t=arg;
    if (size && count > (16*1024*1024-t->length)/size) return 0;
    size_t bytes=size*count;
    unsigned char *p=realloc(t->data,t->length+bytes+1);
    if (!p) return 0;
    t->data=p; memcpy(p+t->length,data,bytes); t->length+=bytes;
    return bytes;
}
static CURL *curl_open(transfer *t,const char *url,bool download)
{
    CURL *c=curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c,CURLOPT_URL,url);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,10L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,download ? 60L : 10L);
    curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(c,CURLOPT_XFERINFODATA,t);
    // Ignore proxy environment for a direct modem-style host bridge.
    curl_easy_setopt(c,CURLOPT_PROXY,"");
    const char *ca=getenv("X16_SERIAL_CA_FILE");
    if (ca && *ca) curl_easy_setopt(c,CURLOPT_CAINFO,ca);
#ifdef _WIN32
    else {
        char *base=SDL_GetBasePath();
        if (base) {
            char bundle[4096]; snprintf(bundle,sizeof(bundle),"%scurl-ca-bundle.crt",base);
            FILE *file=fopen(bundle,"rb");
            if (file) { fclose(file); curl_easy_setopt(c,CURLOPT_CAINFO,bundle); }
            SDL_free(base);
        }
    }
#endif
    if (download) {
        curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"http,https,ftp,ftps,gopher,gophers");
        curl_easy_setopt(c,CURLOPT_REDIR_PROTOCOLS_STR,"http,https,ftp,ftps,gopher,gophers");
        curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
        curl_easy_setopt(c,CURLOPT_MAXREDIRS,5L);
        curl_easy_setopt(c,CURLOPT_FAILONERROR,1L);
        curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,download_data);
        curl_easy_setopt(c,CURLOPT_WRITEDATA,t);
    } else {
        curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
        curl_easy_setopt(c,CURLOPT_CONNECT_ONLY,1L);
    }
    if (curl_easy_perform(c)!=CURLE_OK || cancelled(t->net,t->generation)) {
        curl_easy_cleanup(c); return NULL;
    }
    return c;
}
static bool ssh_wait(net_socket *n,unsigned generation,Uint32 deadline)
{
    if (cancelled(n,generation) || (Sint32)(SDL_GetTicks()-deadline)>=0) return false;
    SDL_Delay(2); return true;
}
static LIBSSH2_CHANNEL *ssh_open(net_socket *n,unsigned generation,socket_handle fd,
    const char *host,const char *port,const char *user,const char *password,LIBSSH2_SESSION **session)
{
    LIBSSH2_SESSION *s=libssh2_session_init();
    if (!s) return NULL;
    *session=s;
    libssh2_session_set_blocking(s,0);
    Uint32 deadline=SDL_GetTicks()+10000;
    int rc;
    do { rc=libssh2_session_handshake(s,fd); }
    while (rc==LIBSSH2_ERROR_EAGAIN && ssh_wait(n,generation,deadline));
    if (rc) return NULL;
    // Authenticate the host before ever transmitting a password.
    const char *file=getenv("X16_SERIAL_KNOWN_HOSTS");
    char path[2048];
    if (!file) {
        const char *home=getenv("HOME");
#ifdef _WIN32
        if (!home) home=getenv("USERPROFILE");
#endif
        snprintf(path,sizeof(path),"%s/.ssh/known_hosts",home ? home : ""); file=path;
    }
    LIBSSH2_KNOWNHOSTS *hosts=libssh2_knownhost_init(s);
    if (!hosts) return NULL;
    size_t length; int type;
    const char *key=libssh2_session_hostkey(s,&length,&type);
    struct libssh2_knownhost *match=NULL;
    int verified=LIBSSH2_KNOWNHOST_CHECK_FAILURE;
    if (key && libssh2_knownhost_readfile(hosts,file,LIBSSH2_KNOWNHOST_FILE_OPENSSH)>=0)
        verified=libssh2_knownhost_checkp(hosts,host,atoi(port),key,length,
            LIBSSH2_KNOWNHOST_TYPE_PLAIN|LIBSSH2_KNOWNHOST_KEYENC_RAW,&match);
    libssh2_knownhost_free(hosts);
    if (verified!=LIBSSH2_KNOWNHOST_CHECK_MATCH) return NULL;
    do { rc=libssh2_userauth_password(s,user,password); }
    while (rc==LIBSSH2_ERROR_EAGAIN && ssh_wait(n,generation,deadline));
    if (rc) return NULL;
    LIBSSH2_CHANNEL *ch;
    do { ch=libssh2_channel_open_session(s); }
    while (!ch && libssh2_session_last_errno(s)==LIBSSH2_ERROR_EAGAIN && ssh_wait(n,generation,deadline));
    if (!ch) return NULL;
    do { rc=libssh2_channel_request_pty(ch,"vt100"); }
    while (rc==LIBSSH2_ERROR_EAGAIN && ssh_wait(n,generation,deadline));
    if (!rc) {
        do { rc=libssh2_channel_shell(ch); }
        while (rc==LIBSSH2_ERROR_EAGAIN && ssh_wait(n,generation,deadline));
    }
    if (rc) { libssh2_channel_free(ch); return NULL; }
    libssh2_channel_handle_extended_data2(ch,LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE);
    return ch;
}
static void transport_close(socket_handle *fd,CURL **curl,LIBSSH2_SESSION **ssh,LIBSSH2_CHANNEL **channel)
{
    // Close the socket first: cleanup cannot block on an unresponsive peer.
    if (!*curl && *fd!=BAD_SOCKET) close_socket(*fd);
    *fd=BAD_SOCKET;
    if (*channel) libssh2_channel_free(*channel);
    if (*ssh) libssh2_session_free(*ssh);
    if (*curl) curl_easy_cleanup(*curl);
    *channel=NULL; *ssh=NULL; *curl=NULL;
}
static int worker(void *argument)
{
    net_socket *n=argument;
    socket_handle fd=BAD_SOCKET;
    CURL *curl=NULL;
    LIBSSH2_SESSION *ssh=NULL;
    LIBSSH2_CHANNEL *channel=NULL;
    net_serial *serial=NULL;
    unsigned generation=0;
    int mode=NET_TCP;
    for (;;) {
        char host[2048],port[6],user[128],password[256];
        SDL_LockMutex(n->lock);
        bool stop=n->stop, changed=generation!=n->generation;
        generation=n->generation;
        bool request=n->request;
        if (request) {
            memcpy(host,n->host,sizeof(host)); memcpy(port,n->port,sizeof(port));
            memcpy(user,n->user,sizeof(user)); memcpy(password,n->password,sizeof(password));
            memset(n->password,0,sizeof(n->password));
            mode=n->mode; n->request=false;
        }
        SDL_UnlockMutex(n->lock);
        if (changed || stop || request) { transport_close(&fd,&curl,&ssh,&channel); net_serial_close(serial); serial=NULL; }
        if (stop) break;
        if (request) {
            transfer t={n,generation,NULL,0};
            if (mode==NET_DOWNLOAD) {
                curl=curl_open(&t,host,true);
                SDL_LockMutex(n->lock);
                if (n->generation==generation && !n->stop) {
                    n->state=curl ? NET_CLOSED : NET_FAILED;
                    if (curl) {
                        n->download=t.data ? t.data : calloc(1,1); t.data=NULL;
                        n->download_length=t.length;
                    }
                }
                SDL_UnlockMutex(n->lock);
                free(t.data);
                transport_close(&fd,&curl,&ssh,&channel);
                continue;
            }
            if (mode==NET_SERIAL) {
                serial=net_serial_open(host);
                if (serial && !net_serial_config(serial,115200,3,0x27)) { net_serial_close(serial); serial=NULL; }
            } else if (mode==NET_TLS) {
                char url[2300];
                snprintf(url,sizeof(url),strchr(host,':') ? "https://[%s]:%s/" : "https://%s:%s/",host,port);
                curl=curl_open(&t,url,false);
                if (curl) { curl_easy_getinfo(curl,CURLINFO_ACTIVESOCKET,&fd); curl_easy_setopt(curl,CURLOPT_NOPROGRESS,1L); }
            } else {
                fd=dial(n,host,port,generation,mode);
                if (fd!=BAD_SOCKET && mode==NET_SSH) {
                    channel=ssh_open(n,generation,fd,host,port,user,password,&ssh);
                    if (!channel) transport_close(&fd,&curl,&ssh,&channel);
                }
            }
            memset(password,0,sizeof(password));
            SDL_LockMutex(n->lock);
            if (n->generation==generation && !n->stop) {
                n->state=fd==BAD_SOCKET && !serial ? NET_FAILED : mode==NET_LISTEN ? NET_LISTENING : NET_CONNECTED;
                n->established=(fd!=BAD_SOCKET || serial) && mode!=NET_LISTEN;
            }
            SDL_UnlockMutex(n->lock);
            continue;
        }
        if (fd==BAD_SOCKET && !serial) { SDL_Delay(1); continue; }
        if (serial) {
            SDL_LockMutex(n->lock);
            bool update=n->serial_changed; n->serial_changed=false;
            unsigned baud=n->baud,lcr=n->lcr,mcr=n->mcr;
            SDL_UnlockMutex(n->lock);
            if (update) net_serial_config(serial,baud,lcr,mcr);
            unsigned lines=net_serial_status(serial);
            SDL_LockMutex(n->lock); if (n->generation==generation) n->modem_lines=lines; SDL_UnlockMutex(n->lock);
        }
        SDL_LockMutex(n->lock);
        int state=n->state;
        bool want_input=QUEUE_SIZE-n->rx_count >= (mode==NET_UDP ? 8194 : 1);
        bool want_output=n->tx_count!=0;
        SDL_UnlockMutex(n->lock);
        if (state==NET_LISTENING) {
            socket_handle peer=accept(fd,NULL,NULL);
            if (peer!=BAD_SOCKET) {
#ifdef _WIN32
                u_long one=1; ioctlsocket(peer,FIONBIO,&one);
#else
                int f=fcntl(peer,F_GETFL,0);
                if (peer>=FD_SETSIZE || f<0 || fcntl(peer,F_SETFL,f|O_NONBLOCK)) { close_socket(peer); continue; }
#endif
                close_socket(fd); fd=peer;
                SDL_LockMutex(n->lock);
                if (n->generation==generation) { n->state=NET_CONNECTED; n->established=true; }
                SDL_UnlockMutex(n->lock);
            } else SDL_Delay(1);
            continue;
        }
        // These are nonblocking APIs, including curl and libssh2. All library
        // and socket calls occur without the mutex needed by the emulated CPU.
        unsigned char buffer[QUEUE_SIZE];
        size_t length=0;
        if (want_output) {
            SDL_LockMutex(n->lock);
            if (n->generation!=generation) { SDL_UnlockMutex(n->lock); continue; }
            length=n->tx_count;
            for (size_t i=0;i<length;i++) buffer[i]=n->tx[(n->tx_head+i)%QUEUE_SIZE];
            SDL_UnlockMutex(n->lock);
        }
        bool failed=false;
        size_t consumed=0;
        if (length) {
            int rc=-1;
            if (serial) {
                rc=net_serial_write(serial,buffer,length); if (rc>0) consumed=(size_t)rc; else failed=rc<0;
            } else if (curl) {
                CURLcode code=curl_easy_send(curl,buffer,length,&consumed);
                failed=code!=CURLE_OK && code!=CURLE_AGAIN;
            } else if (channel) {
                rc=(int)libssh2_channel_write(channel,(char *)buffer,length);
                if (rc>0) consumed=(size_t)rc;
                else failed=rc!=LIBSSH2_ERROR_EAGAIN;
            } else if (mode==NET_UDP) {
                // Emulator extension: 16-bit big-endian length + one datagram.
                if (length>=2) {
                    size_t bytes=((size_t)buffer[0]<<8)|buffer[1];
                    if (bytes>8192) failed=true;
                    else if (length>=bytes+2) {
                        rc=send(fd,(char *)buffer+2,(int)bytes,0);
                        if (rc==(int)bytes) consumed=bytes+2;
                        else if (rc<0 && !would_block()) failed=true;
                    }
                }
            } else {
#ifdef MSG_NOSIGNAL
                int flags=MSG_NOSIGNAL;
#else
                int flags=0;
#endif
                rc=send(fd,(char *)buffer,(int)length,flags);
                if (rc>0) consumed=(size_t)rc;
                else failed=rc==0 || !would_block();
            }
            SDL_LockMutex(n->lock);
            if (n->generation==generation) { n->tx_head=(n->tx_head+consumed)%QUEUE_SIZE; n->tx_count-=consumed; }
            SDL_UnlockMutex(n->lock);
        }
        size_t received=0;
        if (want_input && !failed) {
            SDL_LockMutex(n->lock); size_t space=QUEUE_SIZE-n->rx_count; SDL_UnlockMutex(n->lock);
            int rc=-1;
            if (serial) {
                rc=net_serial_read(serial,buffer,space); if (rc>0) received=(size_t)rc; else failed=rc<0;
            } else if (curl) {
                CURLcode code=curl_easy_recv(curl,buffer,space,&received);
                failed=(code!=CURLE_OK && code!=CURLE_AGAIN) || (code==CURLE_OK && received==0);
            } else if (channel) {
                rc=(int)libssh2_channel_read(channel,(char *)buffer,space);
                if (rc>0) received=(size_t)rc;
                else failed=rc==0 ? libssh2_channel_eof(channel)!=0 : rc!=LIBSSH2_ERROR_EAGAIN;
            } else if (mode==NET_UDP) {
                rc=recv(fd,(char *)buffer+2,8193,0);
                if (rc>=0 && rc<=8192) { buffer[0]=(unsigned char)(rc>>8); buffer[1]=(unsigned char)rc; received=(size_t)rc+2; }
                else if (rc<0) {
                    failed=!would_block();
#ifdef _WIN32
                    if (WSAGetLastError()==WSAEMSGSIZE) failed=false;
#endif
                } // Larger datagrams are discarded intact, never exposed truncated.
            } else {
                rc=recv(fd,(char *)buffer,(int)space,0);
                if (rc>0) received=(size_t)rc;
                else failed=rc==0 || !would_block();
            }
            SDL_LockMutex(n->lock);
            if (n->generation==generation) {
                for (size_t i=0;i<received;i++) n->rx[(n->rx_head+n->rx_count+i)%QUEUE_SIZE]=buffer[i];
                n->rx_count+=received;
            }
            SDL_UnlockMutex(n->lock);
        }
        if (failed) {
            transport_close(&fd,&curl,&ssh,&channel); net_serial_close(serial); serial=NULL;
            SDL_LockMutex(n->lock);
            if (n->generation==generation) { n->state=NET_CLOSED; n->tx_count=0; }
            SDL_UnlockMutex(n->lock);
        }
        SDL_Delay(1);
    }
    transport_close(&fd,&curl,&ssh,&channel); net_serial_close(serial);
    return 0;
}

net_socket *net_socket_create(void)
{
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2,2),&data)) return NULL;
#endif
    static bool initialized=false; // creation/destruction is confined to the CPU thread
    if (!initialized) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK || libssh2_init(0)) goto fail;
        initialized=true;
    }
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
    free(n->download);
    free(n);
#ifdef _WIN32
    WSACleanup();
#endif
}
bool net_socket_connect(net_socket *n,const char *host,const char *port)
{
    return net_socket_open(n,host,port,NET_TCP,NULL,NULL);
}
bool net_socket_open(net_socket *n,const char *host,const char *port,int mode,const char *user,const char *password)
{
    if (!n || !host || !port || strlen(host)>=sizeof(n->host) || strlen(port)>=sizeof(n->port) ||
        (user && strlen(user)>=sizeof(n->user)) || (password && strlen(password)>=sizeof(n->password))) return false;
    SDL_LockMutex(n->lock);
    n->established=false; n->generation++; n->request=true; n->state=NET_DIALING; n->mode=mode;
    n->baud=115200; n->lcr=3; n->mcr=0x27; n->modem_lines=0; n->serial_changed=false;
    strcpy(n->host,host); strcpy(n->port,port);
    snprintf(n->user,sizeof(n->user),"%s",user ? user : "");
    snprintf(n->password,sizeof(n->password),"%s",password ? password : "");
    free(n->download); n->download=NULL; n->download_length=0;
    n->rx_head=n->rx_count=n->tx_head=n->tx_count=0;
    SDL_UnlockMutex(n->lock);
    return true;
}
unsigned char *net_socket_download(net_socket *n,size_t *length)
{
    SDL_LockMutex(n->lock);
    unsigned char *data=n->download;
    *length=n->download_length; n->download=NULL; n->download_length=0;
    SDL_UnlockMutex(n->lock);
    return data;
}

void net_socket_serial_config(net_socket *n,unsigned baud,unsigned lcr,unsigned mcr) {
    if (!n) return;
    SDL_LockMutex(n->lock);
    if (n->baud!=baud || n->lcr!=lcr || n->mcr!=mcr) {
        n->baud=baud; n->lcr=lcr; n->mcr=mcr; n->serial_changed=true;
    }
    SDL_UnlockMutex(n->lock);
}
unsigned net_socket_modem_lines(net_socket *n) {
    if (!n) return 0;
    SDL_LockMutex(n->lock);
    unsigned value=n->mode==NET_SERIAL ? n->modem_lines : n->state==NET_CONNECTED ? 0xb0 : 0;
    SDL_UnlockMutex(n->lock); return value;
}

void net_socket_disconnect(net_socket *n)
{
    if (!n) return;
    SDL_LockMutex(n->lock);
    n->established=false; n->generation++; n->request=false; n->state=NET_IDLE;
    n->rx_head=n->rx_count=n->tx_head=n->tx_count=0;
    memset(n->password,0,sizeof(n->password));
    free(n->download); n->download=NULL; n->download_length=0;
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
