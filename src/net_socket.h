// Host TCP backend for the optional serial card. BSD-2-Clause.
#ifndef X16_NET_SOCKET_H
#define X16_NET_SOCKET_H
#include <stddef.h>
#include <stdbool.h>
typedef struct net_socket net_socket;
enum { NET_IDLE, NET_DIALING, NET_CONNECTED, NET_CLOSED, NET_FAILED };
net_socket *net_socket_create(void);
void net_socket_destroy(net_socket *net);
bool net_socket_connect(net_socket *net, const char *host, const char *port);
void net_socket_disconnect(net_socket *net);
int net_socket_state(net_socket *net);
bool net_socket_established(net_socket *net);
size_t net_socket_available(net_socket *net);
size_t net_socket_space(net_socket *net);
int net_socket_read(net_socket *net);
bool net_socket_write(net_socket *net, unsigned char value);
#endif
