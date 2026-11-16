// Threaded host transport for the optional serial card. BSD-2-Clause.
#ifndef X16_NET_SOCKET_H
#define X16_NET_SOCKET_H
#include <stddef.h>
#include <stdbool.h>
typedef struct net_socket net_socket;
enum { NET_IDLE, NET_DIALING, NET_CONNECTED, NET_CLOSED, NET_FAILED, NET_LISTENING };
enum { NET_TCP, NET_TLS, NET_SSH, NET_UDP, NET_LISTEN, NET_DOWNLOAD, NET_SERIAL };
net_socket *net_socket_create(void);
void net_socket_destroy(net_socket *net);
bool net_socket_connect(net_socket *net, const char *host, const char *port);
bool net_socket_open(net_socket *net, const char *host, const char *port, int mode,
                     const char *user, const char *password);
void net_socket_serial_config(net_socket *net,unsigned baud,unsigned lcr,unsigned mcr);
unsigned net_socket_modem_lines(net_socket *net);
void net_socket_disconnect(net_socket *net);
int net_socket_state(net_socket *net);
bool net_socket_established(net_socket *net);
size_t net_socket_available(net_socket *net);
size_t net_socket_space(net_socket *net);
int net_socket_read(net_socket *net);
bool net_socket_write(net_socket *net, unsigned char value);
// Downloads are bounded to 16 MiB; ownership transfers to the caller on success.
unsigned char *net_socket_download(net_socket *net, size_t *length);
#endif
