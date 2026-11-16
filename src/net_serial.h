// Host COM/tty transport. Called exclusively from the transport worker.
#ifndef X16_NET_SERIAL_H
#define X16_NET_SERIAL_H
#include <stddef.h>
#include <stdbool.h>
typedef struct net_serial net_serial;
net_serial *net_serial_open(const char *path);
void net_serial_close(net_serial *serial);
bool net_serial_config(net_serial *serial,unsigned baud,unsigned lcr,unsigned mcr);
int net_serial_read(net_serial *serial,unsigned char *data,size_t size);
int net_serial_write(net_serial *serial,const unsigned char *data,size_t size);
unsigned net_serial_status(net_serial *serial);
#endif
