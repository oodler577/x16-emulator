// Host COM/tty bridge. BSD-2-Clause. No port is opened without -serial2-device.
#ifndef _WIN32
#define _DEFAULT_SOURCE
#endif
#include "net_serial.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
net_serial *net_serial_open(const char *p) { (void)p; return NULL; }
void net_serial_close(net_serial *s) { (void)s; }
bool net_serial_config(net_serial *s,unsigned b,unsigned l,unsigned m) { (void)s;(void)b;(void)l;(void)m;return false; }
int net_serial_read(net_serial *s,unsigned char *p,size_t z) { (void)s;(void)p;(void)z;return -1; }
int net_serial_write(net_serial *s,const unsigned char *p,size_t z) { (void)s;(void)p;(void)z;return -1; }
unsigned net_serial_status(net_serial *s) { (void)s;return 0; }
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
struct net_serial { HANDLE handle; };
net_serial *net_serial_open(const char *path) {
    char name[512];
    snprintf(name,sizeof(name),strncmp(path,"\\\\.\\",4) ? "\\\\.\\%s" : "%s",path);
    HANDLE h=CreateFileA(name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    if (h==INVALID_HANDLE_VALUE) return NULL;
    net_serial *s=malloc(sizeof(*s));
    if (!s) { CloseHandle(h); return NULL; }
    s->handle=h;
    COMMTIMEOUTS timeouts={MAXDWORD,0,0,0,1}; // reads poll; writes bounded to 1ms
    if (!SetCommTimeouts(h,&timeouts)) { net_serial_close(s); return NULL; }
    return s;
}
void net_serial_close(net_serial *s) { if (s) { CloseHandle(s->handle); free(s); } }
bool net_serial_config(net_serial *s,unsigned baud,unsigned lcr,unsigned mcr) {
    DCB d={0}; d.DCBlength=sizeof(d);
    if (!GetCommState(s->handle,&d)) return false;
    d.BaudRate=baud; d.ByteSize=5+(lcr&3); d.fBinary=TRUE;
    d.Parity=!(lcr&8) ? NOPARITY : lcr&0x20 ? ((lcr&0x10) ? SPACEPARITY : MARKPARITY) : ((lcr&0x10) ? EVENPARITY : ODDPARITY);
    d.fParity=(lcr&8)!=0; d.StopBits=(lcr&4) ? ((lcr&3)==0 ? ONE5STOPBITS : TWOSTOPBITS) : ONESTOPBIT;
    d.fOutxCtsFlow=(mcr&0x20)!=0; d.fOutxDsrFlow=FALSE; d.fDsrSensitivity=FALSE;
    d.fInX=d.fOutX=FALSE; d.fAbortOnError=FALSE;
    d.fDtrControl=(mcr&1) ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    d.fRtsControl=(mcr&2) ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    if (!SetCommState(s->handle,&d)) return false;
    return (lcr&0x40) ? SetCommBreak(s->handle)!=0 : ClearCommBreak(s->handle)!=0;
}
int net_serial_read(net_serial *s,unsigned char *p,size_t z) {
    DWORD n=0; return ReadFile(s->handle,p,(DWORD)z,&n,NULL) ? (int)n : -1;
}
int net_serial_write(net_serial *s,const unsigned char *p,size_t z) {
    DWORD n=0; return WriteFile(s->handle,p,(DWORD)z,&n,NULL) ? (int)n : -1;
}
unsigned net_serial_status(net_serial *s) {
    DWORD bits=0; if (!GetCommModemStatus(s->handle,&bits)) return 0;
    return ((bits&MS_CTS_ON)?0x10:0)|((bits&MS_DSR_ON)?0x20:0)|((bits&MS_RING_ON)?0x40:0)|((bits&MS_RLSD_ON)?0x80:0);
}
#else
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
struct net_serial { int fd; struct termios saved; };
net_serial *net_serial_open(const char *path) {
    int fd=open(path,O_RDWR|O_NOCTTY|O_NONBLOCK);
    if (fd<0) return NULL;
    net_serial *s=malloc(sizeof(*s));
    if (!s) { close(fd); return NULL; }
    s->fd=fd;
    if (tcgetattr(fd,&s->saved)) { close(fd); free(s); return NULL; }
    return s;
}
void net_serial_close(net_serial *s) { if (s) { tcsetattr(s->fd,TCSANOW,&s->saved); close(s->fd); free(s); } }
bool net_serial_config(net_serial *s,unsigned baud,unsigned lcr,unsigned mcr) {
    speed_t speed;
    switch (baud) {
    case 50:speed=B50;break; case 75:speed=B75;break; case 110:speed=B110;break;
    case 300:speed=B300;break; case 600:speed=B600;break; case 1200:speed=B1200;break;
    case 2400:speed=B2400;break; case 4800:speed=B4800;break; case 9600:speed=B9600;break;
    case 19200:speed=B19200;break; case 38400:speed=B38400;break;
    case 57600:speed=B57600;break; case 115200:speed=B115200;break;
#ifdef B230400
    case 230400:speed=B230400;break;
#endif
#ifdef B460800
    case 460800:speed=B460800;break;
#endif
#ifdef B921600
    case 921600:speed=B921600;break;
#endif
    default:return false;
    }
    struct termios t=s->saved; cfmakeraw(&t);
    t.c_cflag|=CLOCAL|CREAD; t.c_cflag&=~(CSIZE|PARENB|PARODD|CSTOPB);
    const unsigned bits[]={CS5,CS6,CS7,CS8}; t.c_cflag|=bits[lcr&3];
    if (lcr&8) { t.c_cflag|=PARENB; if (!(lcr&0x10)) t.c_cflag|=PARODD; }
#ifdef CMSPAR
    if (lcr&0x20) t.c_cflag|=CMSPAR; else t.c_cflag&=~CMSPAR;
#endif
    if (lcr&4) t.c_cflag|=CSTOPB;
#ifdef CRTSCTS
    // UART AFE is modeled by the guest; use the guest RTS level at the host.
    t.c_cflag&=~CRTSCTS;
#endif
    cfsetispeed(&t,speed); cfsetospeed(&t,speed);
    if (tcsetattr(s->fd,TCSANOW,&t)) return false;
    int set=((mcr&1)?TIOCM_DTR:0)|((mcr&2)?TIOCM_RTS:0);
    int clear=((mcr&1)?0:TIOCM_DTR)|((mcr&2)?0:TIOCM_RTS);
    ioctl(s->fd,TIOCMBIS,&set); ioctl(s->fd,TIOCMBIC,&clear);
#ifdef TIOCSBRK
    ioctl(s->fd,(lcr&0x40)?TIOCSBRK:TIOCCBRK,0);
#endif
    return true;
}
int net_serial_read(net_serial *s,unsigned char *p,size_t z) {
    int n=(int)read(s->fd,p,z); return n<0 && (errno==EAGAIN || errno==EINTR) ? 0 : n;
}
int net_serial_write(net_serial *s,const unsigned char *p,size_t z) {
    int n=(int)write(s->fd,p,z); return n<0 && (errno==EAGAIN || errno==EINTR) ? 0 : n;
}
unsigned net_serial_status(net_serial *s) {
    int b=0;
    if (ioctl(s->fd,TIOCMGET,&b)) return 0xb0; // PTYs have no modem pins
    return ((b&TIOCM_CTS)?0x10:0)|((b&TIOCM_DSR)?0x20:0)|((b&TIOCM_RI)?0x40:0)|((b&TIOCM_CD)?0x80:0);
}
#endif
