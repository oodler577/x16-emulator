// Optional dual-UART serial/network card. Copyright (c) 2026 retrodores contributors.
// License: 2-clause BSD, see LICENSE. Independent model, not ESP32 firmware.
#include "netcard.h"
#include "net_socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define RESPONSE_SIZE 4096
typedef struct {
    uint8_t dll, dlm, ier, fcr, lcr, mcr, scratch, errors, delta, msr;
    uint8_t rx[16], tx[16], shift;
    unsigned rx_head, rx_count, tx_head, tx_count;
    uint64_t rx_clock, tx_clock, rx_idle;
    bool shifting, thre_irq;
} uart_state;
static uart_state uarts[2];
static net_socket *network, *serial_peer;
static bool escaped, incoming, listening, listen_pending, downloading, telnet_mode;
static unsigned config_menu;
static unsigned sregs[256], escape_count, telnet_state, telnet_op, telnet_sub;
static unsigned char telnet_local[256], telnet_remote[256];
static uint64_t silence_clock, escape_clock, ring_clock;
static unsigned ring_count, listener_port;
static char peer_name[256], mock_ssid[64]="HOST-BRIDGE";
static unsigned char *download_data;
static size_t download_length, download_pos;
static unsigned download_column;
static unsigned char telnet_queue[512];
static unsigned telnet_head, telnet_count;
static char serial_target[256];
static bool enabled, streaming, dialing, quiet, echo_on, numeric, hardware_flow;
static unsigned base_address, eol_mode, hangup_mode, command_length;
static unsigned long modem_baud=115200;
static bool command_overflow;
static char command[256];
static uint8_t responses[RESPONSE_SIZE];
static unsigned response_head, response_count;
static void restart_listener(void);
static uint64_t peer_clock[2], carrier_hold;

static unsigned fifo_size(const uart_state *u) { return u->fcr & 1 ? 16 : 1; }
static unsigned trigger(const uart_state *u) {
    static const unsigned levels[4]={1,4,8,14};
    return u->fcr & 1 ? levels[u->fcr >> 6] : 1;
}
static void queue_byte(uint8_t value) {
    if (response_count < RESPONSE_SIZE) {
        responses[(response_head+response_count)%RESPONSE_SIZE]=value;
        response_count++;
    } else uarts[0].errors |= 2;
}
static void queue_text(const char *text) { while (*text) queue_byte((uint8_t)*text++); }
static void queue_eol(void) { queue_byte(13); if (eol_mode) queue_byte(10); }
enum { RESULT_OK, RESULT_CONNECT, RESULT_CARRIER, RESULT_ERROR, RESULT_ANSWER, RESULT_RING };
static void result(unsigned code) {
    static const char *words[]={"OK","CONNECT","NO CARRIER","ERROR","NO ANSWER","RING"};
    static const char *numbers[]={"0","1","3","4","8","2"};
    if (quiet) return;
    queue_eol(); queue_text(numeric ? numbers[code] : words[code]); queue_eol();
}
static void hangup(bool notice) {
    bool was_connected=streaming || dialing;
    net_socket_disconnect(network);
    streaming=dialing=escaped=incoming=downloading=false; carrier_hold=0;
    free(download_data); download_data=NULL; download_length=download_pos=0;
    escape_count=0; telnet_count=telnet_head=telnet_state=0;
    if (notice && was_connected) result(RESULT_CARRIER);
}
static void modem_reset(void) {
    hangup(false);
    listening=listen_pending=false; listener_port=0; config_menu=false;
    memset(sregs,0,sizeof(sregs)); sregs[2]=43; sregs[12]=50; sregs[63]=2;
    quiet=false; echo_on=true; numeric=false; hardware_flow=true;
    eol_mode=1; hangup_mode=2; command_length=0; command_overflow=false;
}
bool netcard_enable(uint16_t address) {
    if (address < 0x9f60 || address > 0x9ff0 || (address & 15) || address == 0x9fb0)
        return false;
    if (!network) network=net_socket_create();
    if (!network) return false;
    enabled=true; base_address=address;
    netcard_reset();
    return true;
}
bool netcard_matches(uint16_t address) {
    return enabled && (address & 0xfff0) == base_address;
}
void netcard_reset(void) {
    if (!enabled) return;
    memset(uarts,0,sizeof(uarts));
    uarts[0].dll=uarts[1].dll=8;
    uarts[0].thre_irq=uarts[1].thre_irq=true;
    response_head=response_count=0; peer_clock[0]=peer_clock[1]=0;
    modem_baud=115200;
    modem_reset();
}
void netcard_close(void) {
    enabled=false;
    net_socket_destroy(network); network=NULL;
    net_socket_destroy(serial_peer); serial_peer=NULL; serial_target[0]=0;
}
static bool rts(const uart_state *u) {
    return (u->mcr & 2) && (!(u->mcr & 0x20) || u->rx_count < trigger(u));
}
static bool cts(unsigned port) {
    if (port) return (net_socket_modem_lines(serial_peer)&0x10) && net_socket_space(serial_peer)>=16;
    return response_count < RESPONSE_SIZE-512 &&
        (!streaming || net_socket_space(network) >= 16) && !dialing;
}
static void update_msr(unsigned port) {
    uart_state *u=&uarts[port];
    uint8_t value;
    if (u->mcr & 0x10) {
        value=((u->mcr & 2) ? 0x10 : 0) | ((u->mcr & 1) ? 0x20 : 0) |
              ((u->mcr & 4) ? 0x40 : 0) | ((u->mcr & 8) ? 0x80 : 0);
    } else {
        value=(cts(port) ? 0x10 : 0) | (port ? 0 : 0x20) |
              ((!port && (streaming || escaped)) || (port && serial_peer && net_socket_state(serial_peer)==NET_CONNECTED) ? 0x80 : 0) | ((!port && incoming) ? 0x40 : 0);
    }
    if (port && !(u->mcr&0x10)) value=(uint8_t)net_socket_modem_lines(serial_peer);
    uint8_t changed=u->msr ^ value;
    if (changed & 0x10) u->delta |= 1;
    if (changed & 0x20) u->delta |= 2;
    if ((changed & 0x40) && !(value & 0x40)) u->delta |= 4;
    if (changed & 0x80) u->delta |= 8;
    u->msr=value;
}
static uint64_t character_clocks(const uart_state *u,unsigned mhz) {
    unsigned divisor=(unsigned)u->dll | (unsigned)u->dlm << 8;
    if (!divisor) divisor=65536;
    unsigned bits=1+5+(u->lcr & 3)+((u->lcr & 8) ? 1 : 0)+((u->lcr & 4) ? 2 : 1);
    uint64_t clocks=((uint64_t)mhz*1000000*16*divisor*bits+14745600-1)/14745600;
    return clocks ? clocks : 1;
}
static void receive(uart_state *u,uint8_t value) {
    if (u->rx_count < fifo_size(u)) {
        u->rx[(u->rx_head+u->rx_count)%16]=value;
        u->rx_count++;
    } else u->errors |= 2;
    u->rx_idle=0; u->rx_clock=0;
}
static unsigned interrupt_id(uart_state *u) {
    if ((u->ier & 4) && u->errors) return 6;
    if ((u->ier & 1) && u->rx_count >= trigger(u)) return 4;
    if ((u->ier & 1) && (u->fcr & 1) && u->rx_count && u->rx_idle >= 4) return 12;
    if ((u->ier & 2) && u->thre_irq) return 2;
    if ((u->ier & 8) && u->delta) return 0;
    return 1;
}
bool netcard_irq(void) {
    return enabled && (!(interrupt_id(&uarts[0]) & 1) || !(interrupt_id(&uarts[1]) & 1));
}
uint8_t netcard_read(uint8_t reg,bool debug) {
    uart_state *u=&uarts[(reg >> 3) & 1];
    unsigned offset=reg & 7;
    if ((u->lcr & 0x80) && offset < 2) return offset ? u->dlm : u->dll;
    switch (offset) {
    case 0: {
        uint8_t value=u->rx_count ? u->rx[u->rx_head] : 0;
        if (!debug && u->rx_count) { u->rx_head=(u->rx_head+1)%16; u->rx_count--; u->rx_idle=0; }
        return value;
    }
    case 1: return u->ier;
    case 2: {
        unsigned id=interrupt_id(u);
        if (!debug && id == 2) u->thre_irq=false;
        return (uint8_t)(id | ((u->fcr & 1) ? 0xc0 : 0));
    }
    case 3: return u->lcr;
    case 4: return u->mcr;
    case 5: {
        uint8_t value=(u->rx_count ? 1 : 0) | u->errors |
            (u->tx_count == 0 ? 0x20 : 0) | ((!u->tx_count && !u->shifting) ? 0x40 : 0);
        if (!debug) u->errors=0;
        return value;
    }
    case 6: {
        if (!debug) update_msr((reg >> 3) & 1);
        uint8_t value=u->msr | u->delta;
        if (!debug) u->delta=0;
        return value;
    }
    default: return u->scratch;
    }
}

static unsigned long number(const char **cursor,bool *found) {
    unsigned long value=0;
    *found=false;
    while (**cursor >= '0' && **cursor <= '9') {
        *found=true;
        if (value < 100000000UL) value=value*10+(unsigned)(*(*cursor)-'0');
        (*cursor)++;
    }
    return value;
}
static bool same(const char *a,const char *b) {
    while (*a && *b) if (toupper((unsigned char)*a++)!=toupper((unsigned char)*b++)) return false;
    return !*a && !*b;
}
static bool unquote(const char *text,char *out,size_t size) {
    size_t len=strlen(text);
    if (len<2 || text[0]!='"' || text[len-1]!='"' || len-2>=size) return false;
    memcpy(out,text+1,len-2); out[len-2]=0; return true;
}
static bool split_endpoint(const char *text,char *host,size_t size,char *port) {
    const char *colon=strrchr(text,':');
    if (!colon || colon==text || !colon[1] || (size_t)(colon-text)>=size) return false;
    unsigned long n=0;
    for (const char *p=colon+1;*p;p++) {
        if (!isdigit((unsigned char)*p)) return false;
        n=n*10+(unsigned)(*p-'0'); if (n>65535) return false;
    }
    if (!n) return false;
    memcpy(host,text,(size_t)(colon-text)); host[colon-text]=0;
    size_t len=strlen(host);
    if (host[0]=='[') {
        if (len<3 || host[len-1]!=']') return false;
        memmove(host,host+1,len-2); host[len-2]=0;
    }
    snprintf(port,6,"%lu",n); return true;
}
static bool start_dial(const char *text) {
    int mode=NET_TCP;
    telnet_mode=sregs[62]!=0;
    while (*text && *text!='"') {
        char c=(char)toupper((unsigned char)*text++);
        if (c=='T') telnet_mode=true;
        else if (c=='S') mode=NET_TLS;
        else if (c=='U') mode=NET_UDP;
        else return false;
    }
    char endpoint[256],host[256],port[6],user[128]="",password[256]="";
    if (!unquote(text,endpoint,sizeof(endpoint))) return false;
    char *address=endpoint;
    if (mode==NET_TLS && strchr(endpoint,'@')) {
        char *at=strrchr(endpoint,'@'); *at=0; address=at+1;
        char *colon=strchr(endpoint,':'); if (!colon) return false;
        *colon++=0;
        if (strlen(endpoint)>=sizeof(user)) return false;
        strcpy(user,endpoint); strcpy(password,colon); mode=NET_SSH;
    }
    if (!split_endpoint(address,host,sizeof(host),port)) return false;
    if (mode==NET_UDP) telnet_mode=false;
    bool ok=net_socket_open(network,host,port,mode,user,password);
    memset(password,0,sizeof(password)); memset(endpoint,0,sizeof(endpoint));
    if (!ok) return false;
    snprintf(peer_name,sizeof(peer_name),"%.240s:%s",host,port);
    dialing=true; escaped=incoming=false; silence_clock=0; escape_count=0;
    memset(telnet_local,0,sizeof(telnet_local)); memset(telnet_remote,0,sizeof(telnet_remote));
    telnet_head=telnet_count=telnet_state=0;
    return true;
}
static void restart_listener(void) {
    if (!listening || !listener_port) return;
    char port[6]; snprintf(port,sizeof(port),"%u",listener_port);
    const char *bind_host=getenv("X16_SERIAL_BIND");
    net_socket_open(network,bind_host && *bind_host ? bind_host : "127.0.0.1",port,NET_LISTEN,NULL,NULL);
}
static bool start_download(const char *text) {
    char url[2048],value[1800];
    if (!unquote(text,value,sizeof(value)) || !*value) return false;
    snprintf(url,sizeof(url),strstr(value,"://") ? "%s" : "http://%s",value);
    if (!net_socket_open(network,url,"0",NET_DOWNLOAD,NULL,NULL)) return false;
    downloading=true; download_pos=download_length=0; return true;
}
static void download_header(void) {
    unsigned sum=0;
    for (size_t i=0;i<download_length;i++) sum=(sum+download_data[i])&255;
    char header[96];
    if (sregs[45]==3) snprintf(header,sizeof(header),"[ 0 %zu ]",download_length);
    else if (sregs[45]==1) snprintf(header,sizeof(header),"[ 00 %02zX %02X ]",download_length,sum);
    else if (sregs[45]==2) {
        queue_byte('['); queue_eol(); queue_byte('0'); queue_eol();
        snprintf(header,sizeof(header),"%zu",download_length); queue_text(header); queue_eol();
        snprintf(header,sizeof(header),"%u",sum); queue_text(header); queue_eol(); queue_byte(']'); queue_eol(); return;
    } else snprintf(header,sizeof(header),"[ 0 %zu %u ]",download_length,sum);
    queue_text(header); queue_eol();
}
static void telnet_send(const unsigned char *bytes,unsigned size) {
    for (unsigned i=0;i<size && telnet_count<sizeof(telnet_queue);i++) {
        telnet_queue[(telnet_head+telnet_count)%sizeof(telnet_queue)]=bytes[i]; telnet_count++;
    }
}
static void telnet_reply(unsigned op,unsigned option) {
    unsigned char bytes[3]={255,(unsigned char)op,(unsigned char)option}; telnet_send(bytes,3);
}
static int telnet_read(void) {
    if (telnet_count>sizeof(telnet_queue)-64) return -1;
    int value=net_socket_read(network);
    if (value<0) return -1;
    unsigned char v=(unsigned char)value;
    if (!telnet_state) { if (v==255) { telnet_state=1; return -1; } return v; }
    if (telnet_state==1) {
        if (v==255) { telnet_state=0; return 255; }
        if (v>=251 && v<=254) { telnet_op=v; telnet_state=2; }
        else if (v==250) { telnet_state=3; telnet_sub=0; }
        else telnet_state=0;
    } else if (telnet_state==2) {
        if (telnet_op==251) { // WILL: accept remote BINARY, ECHO, SUPPRESS-GO-AHEAD
            bool supported=v==0 || v==1 || v==3;
            if (!telnet_remote[v]) { telnet_reply(supported ? 253 : 254,v); telnet_remote[v]=supported ? 1 : 2; }
        } else if (telnet_op==253) {
            bool supported=v==0 || v==3 || v==24 || v==31;
            if (!telnet_local[v]) {
                telnet_reply(supported ? 251 : 252,v); telnet_local[v]=supported ? 1 : 2;
                if (v==31) { const unsigned char size[]={255,250,31,0,80,0,25,255,240}; telnet_send(size,sizeof(size)); }
            }
        } else if (telnet_op==252) { if (telnet_remote[v]==1) telnet_reply(254,v); telnet_remote[v]=0; }
        else { if (telnet_local[v]==1) telnet_reply(252,v); telnet_local[v]=0; }
        telnet_state=0;
    } else if (telnet_state==3) {
        telnet_sub=v; telnet_state=4;
    } else if (telnet_state==4) {
        if (telnet_sub==24 && v==1 && telnet_local[24]==1) {
            const unsigned char terminal[]={255,250,24,0,'V','T','1','0','0',255,240}; telnet_send(terminal,sizeof(terminal));
            telnet_sub=0;
        }
        if (v==255) telnet_state=5;
    } else if (telnet_state==5) telnet_state=v==240 ? 0 : 4;
    return -1;
}
static bool stream_write(uint8_t value) {
    if (telnet_count || net_socket_space(network)<(telnet_mode && value==255 ? 2u : 1u)) return false;
    if (!net_socket_write(network,value)) return false;
    if (telnet_mode && value==255) return net_socket_write(network,value);
    return true;
}
static void execute_command(void) {
    const char *p=command;
    if (toupper((unsigned char)p[0]) != 'A' || toupper((unsigned char)p[1]) != 'T') {
        result(RESULT_ERROR); return;
    }
    p+=2;
    bool valid=true;
    while (*p && valid) {
        unsigned char op=(unsigned char)toupper((unsigned char)*p++);
        if (op == ' ') continue;
        bool found;
        unsigned long value;
        if (op == '+') {
            if (same(p,"CONFIG")) {
                config_menu=true;
                queue_text("\r\nWi-Fi configuration MOCK - host network bridge\r\n"
                    "No radio scan or Wi-Fi credentials are used.\r\n"
                    "1) Show host bridge status\r\n2) Set mock SSID label\r\nQ) Exit\r\nChoice: ");
                return;
            }
            valid=false; break;
        }
        if (op == 'W') {
            if (*p=='"') {
                char settings[256];
                valid=unquote(p,settings,sizeof(settings));
                if (valid) { char *comma=strchr(settings,','); if (comma) *comma=0;
                    snprintf(mock_ssid,sizeof(mock_ssid),"%.63s",settings);
                    memset(settings,0,sizeof(settings)); }
                p+=strlen(p);
            } else { number(&p,&found); }
            if (valid) { queue_text("\r\nMOCK Wi-Fi: "); queue_text(mock_ssid);
                queue_text(" - host network bridge; no radio or credential use"); queue_eol(); }
            continue;
        }
        if (op == 'D') {
            if ((!*p || same(p,"1")) && escaped) { escaped=false; streaming=true; result(RESULT_CONNECT); return; }
            valid=start_dial(p);
            if (valid) return; // worker supplies CONNECT/NO ANSWER later
            break;
        }
        if (op == '&') {
            op=(unsigned char)toupper((unsigned char)*p++);
            if (!op) { valid=false; break; }
            if (op=='G') { valid=start_download(p); if (valid) return; break; }
            if (op=='F') { modem_reset(); continue; }
            value=number(&p,&found);
            if (op == 'W') { valid=!found || value == 0; }
            else if (op == 'P') valid=found && value == 0;
            else if (op == 'K' && found && (value == 0 || value == 3)) hardware_flow=value == 3;
            else valid=false;
            continue;
        }
        if (op == 'S') {
            unsigned long index=number(&p,&found);
            if (!found || index>255) { valid=false; break; }
            if (*p=='?') { char v[32]; p++; snprintf(v,sizeof(v),"%u",sregs[index]); queue_eol(); queue_text(v); queue_eol(); continue; }
            if (*p != '=') { valid=false; break; } p++;
            value=number(&p,&found);
            if (!found || value>65535) { valid=false; break; }
            if (index==63 && (value==0 || value==2 || value==3)) hangup_mode=(unsigned)value;
            else if (index==0 || index==2 || index==12 || index==41 || index==62) { if (value>255) { valid=false; break; } }
            else if (index==45 && value<=3) { }
            else if ((index==46 || index==48 || index==50 || index==54) && value==0) { }
            else { valid=false; break; }
            sregs[index]=(unsigned)value;
            continue;
        }
        value=number(&p,&found);
        switch (op) {
        case 'E': valid=found && value <= 1; if (valid) echo_on=value != 0; break;
        case 'Q': valid=found && value <= 1; if (valid) quiet=value != 0; break;
        case 'V': valid=found && value <= 1; if (valid) numeric=value == 0; break;
        case 'X': valid=found && value <= 1; break;
        case 'R': valid=found && value <= 1; if (valid) eol_mode=(unsigned)value; break;
        case 'F': valid=found && (value == 0 || value == 4); if (valid) hardware_flow=value == 0; break;
        case 'B': valid=found && value > 0 && value <= 921600; if (valid) modem_baud=value; break;
        case 'N': valid=found && value<=1; if (valid) { listening=false; listener_port=0; if (!streaming && !escaped) hangup(false); } break;
        case 'A':
            if (found && value>0 && value<=65535) {
                char port[6]; snprintf(port,sizeof(port),"%lu",value);
                const char *bind_host=getenv("X16_SERIAL_BIND");
                valid=net_socket_open(network,bind_host && *bind_host ? bind_host : "127.0.0.1",port,NET_LISTEN,NULL,NULL);
                if (valid) { listening=listen_pending=true; listener_port=(unsigned)value; incoming=false; streaming=escaped=dialing=false; return; }
            } else if ((!found || !value) && incoming) {
                incoming=false; streaming=true; result(RESULT_CONNECT); return;
            } else valid=false;
            break;
        case 'O': valid=escaped && (!found || value==0); if (valid) { escaped=false; streaming=true; result(RESULT_CONNECT); return; } break;
        case 'C': {
            char info[384]; valid=!found || value<=1;
            if (valid) { snprintf(info,sizeof(info),"%s 1 %s",(streaming || escaped) ? "CONNECTED" : incoming ? "RINGING" : "DISCONNECTED",peer_name);
                queue_eol(); queue_text(info); queue_eol();
                if (listening) { snprintf(info,sizeof(info),"LISTENING 1 127.0.0.1:%u",listener_port); queue_text(info); queue_eol(); } }
            break;
        }
        case 'H': valid=!found || value<=1; if (valid) { hangup(false); restart_listener(); } break;
        case 'Z': valid=!found || value == 0; if (valid) modem_reset(); break;
        case 'I': {
            valid=!found || value<=6;
            if (valid) {
                queue_eol();
                if (value==2) queue_text("Host bridge: host routing and DNS (no guest Wi-Fi IP)");
                else if (value==3) { queue_text("MOCK Wi-Fi SSID: "); queue_text(mock_ssid); }
                else if (value==1 || value==5) {
                    char info[256]; snprintf(info,sizeof(info),"E%u V%u Q%u B%lu R%u S0=%u S45=%u S62=%u S63=%u",echo_on,!numeric,quiet,modem_baud,eol_mode,sregs[0],sregs[45],sregs[62],hangup_mode); queue_text(info);
                } else if (value==6) queue_text("MOCK MAC: 02:58:31:36:00:01 (not a host adapter)");
                else queue_text("X16 host TCP modem - Zimodem command bridge 0.2\r\nMOCK Wi-Fi; networking uses the host connection");
                queue_eol();
            }
            break;
        }
        default: valid=false; break;
        }
    }
    result(valid ? RESULT_OK : RESULT_ERROR);
}
static bool modem_accept(uint8_t value) {
    if (downloading) {
        if (value==3) { hangup(false); result(RESULT_OK); }
        return true;
    }
    if (streaming) {
        uint64_t guard=(uint64_t)sregs[12]*160000; // 20ms units at 8MHz, normalized in step
        if (sregs[2]<128 && value==sregs[2] &&
            (escape_count || silence_clock>=guard)) {
            if (escape_count<3) { escape_count++; escape_clock=0; silence_clock=0; return true; }
        }
        while (escape_count) { if (!stream_write((uint8_t)sregs[2])) return false; escape_count--; }
        if (!stream_write(value)) return false;
        silence_clock=0;
        return true;
    }
    if (dialing || listen_pending) return false;
    if (echo_on) queue_byte(value);
    if (value == 10 && !command_length) return true;
    if (value == 13) {
        command[command_length]=0;
        if (config_menu) {
            if (config_menu==2) { snprintf(mock_ssid,sizeof(mock_ssid),"%.63s",command); config_menu=true; queue_text("\r\nMock label updated; host networking unchanged.\r\nChoice: "); }
            else if (same(command,"Q") || !*command) { config_menu=false; result(RESULT_OK); }
            else if (same(command,"1")) queue_text("\r\nMOCK Wi-Fi; host bridge active; no credentials stored.\r\nChoice: ");
            else if (same(command,"2")) { config_menu=2; queue_text("\r\nMock SSID label: "); }
            else queue_text("\r\nChoose 1, 2 or Q: ");
        }
        else if (command_overflow || command_length < 2) result(RESULT_ERROR);
        else execute_command();
        memset(command,0,sizeof(command));
        command_length=0; command_overflow=false;
    } else if (value == 8 || value == 127) {
        if (command_length) command_length--;
    } else if (command_length < sizeof(command)-1) {
        command[command_length++]=(char)value;
    } else command_overflow=true;
    return true;
}
void netcard_write(uint8_t reg,uint8_t value) {
    unsigned port=(reg >> 3) & 1;
    uart_state *u=&uarts[port];
    unsigned offset=reg & 7;
    if ((u->lcr & 0x80) && offset < 2) {
        if (offset) u->dlm=value; else u->dll=value;
        return;
    }
    switch (offset) {
    case 0:
        if (u->tx_count < fifo_size(u)) {
            u->tx[(u->tx_head+u->tx_count)%16]=value;
            u->tx_count++; u->thre_irq=false;
        }
        break;
    case 1:
        if (!(u->ier & 2) && (value & 2) && !u->tx_count) u->thre_irq=true;
        u->ier=value & 15;
        break;
    case 2:
        if ((u->fcr ^ value) & 1) { u->rx_count=u->tx_count=0; }
        if (value & 2) { u->rx_count=u->rx_head=0; u->errors=0; }
        if (value & 4) { u->tx_count=u->tx_head=0; u->thre_irq=true; }
        u->fcr=value & 0xc9;
        break;
    case 3: u->lcr=value; break;
    case 4: {
        uint8_t previous=u->mcr;
        u->mcr=value & 0x3f;
        if (!port && !(value & 0x10)) {
            if ((hangup_mode == 2 && (previous & 1) && !(value & 1)) ||
                (hangup_mode == 3 && (previous & 4) && !(value & 4)))
                { hangup(true); restart_listener(); }
        }
        update_msr(port);
        break;
    }
    case 7: u->scratch=value; break;
    default: break;
    }
}
static int modem_output(void) {
    if (response_count) {
        unsigned char value=responses[response_head];
        response_head=(response_head+1)%RESPONSE_SIZE; response_count--;
        return value;
    }
    if (downloading && download_data) {
        if (download_pos<download_length) {
            unsigned char v=download_data[download_pos++];
            if (sregs[45]==1) {
                char hex[4]; snprintf(hex,sizeof(hex),"%02X",v); queue_text(hex);
                if (++download_column==39) { queue_eol(); download_column=0; }
                return modem_output();
            }
            if (sregs[45]==2) { char dec[5]; snprintf(dec,sizeof(dec),"%u",v); queue_text(dec); queue_eol(); return modem_output(); }
            return v;
        }
        if (download_column) { queue_eol(); download_column=0; }
        free(download_data); download_data=NULL; downloading=false; result(RESULT_OK);
        return modem_output();
    }
    if (!streaming) return -1;
    return telnet_mode ? telnet_read() : net_socket_read(network);
}
void netcard_step(unsigned mhz,uint32_t clocks) {
    if (!enabled) return;
    uint64_t normalized=mhz ? (uint64_t)clocks*8/mhz : clocks;
    silence_clock+=normalized;
    if (escape_count) {
        escape_clock+=normalized;
        uint64_t guard=(uint64_t)sregs[12]*160000;
        if (escape_count==3 && escape_clock>=guard) { escape_count=0; streaming=false; escaped=true; result(RESULT_OK); }
        else if (escape_count<3 && escape_clock>=guard) {
            while (escape_count && stream_write((uint8_t)sregs[2])) escape_count--;
        }
    }
    while (telnet_count && net_socket_space(network)) {
        if (!net_socket_write(network,telnet_queue[telnet_head])) break;
        telnet_head=(telnet_head+1)%sizeof(telnet_queue); telnet_count--;
    }
    int socket_state=net_socket_state(network);
    if (listen_pending && (socket_state==NET_LISTENING || socket_state==NET_CONNECTED)) {
        listen_pending=false; result(RESULT_OK);
    } else if (listen_pending && socket_state==NET_FAILED) {
        listen_pending=listening=false; listener_port=0; result(RESULT_ERROR);
    }
    if (downloading && !download_data && (socket_state==NET_CLOSED || socket_state==NET_FAILED)) {
        download_data=net_socket_download(network,&download_length);
        download_pos=0; download_column=0;
        if (!download_data) { downloading=false; result(RESULT_ERROR); }
        else download_header();
    }
    if (listening && !incoming && !streaming && !escaped && socket_state==NET_CONNECTED) {
        incoming=true; ring_clock=0; ring_count=1; result(RESULT_RING);
        snprintf(peer_name,sizeof(peer_name),"incoming:%u",listener_port);
    }
    if (incoming) {
        ring_clock+=normalized;
        if (sregs[0] && ring_count>=sregs[0]) { incoming=false; streaming=true; result(RESULT_CONNECT); }
        else if (ring_clock>=16000000) { ring_clock=0; ring_count++; result(RESULT_RING); }
    }
    bool just_connected=false;
    if (dialing && (socket_state == NET_CONNECTED || net_socket_established(network))) {
        dialing=false; streaming=true; silence_clock=escape_clock=0; result(RESULT_CONNECT);
        carrier_hold=4*character_clocks(&uarts[0],mhz); just_connected=true;
    } else if (dialing && (socket_state == NET_FAILED || socket_state == NET_CLOSED)) {
        dialing=false; result(RESULT_ANSWER); restart_listener();
    }
    for (unsigned port=0; port<2; port++) {
        uart_state *u=&uarts[port];
        uint64_t period=character_clocks(u,mhz);
        if (u->rx_count) {
            u->rx_clock+=clocks;
            if (u->rx_clock >= period) {
                u->rx_idle+=u->rx_clock/period; u->rx_clock%=period;
            }
        }
        if (!u->shifting && u->tx_count &&
            ((u->mcr & 0x10) || !(u->mcr & 0x20) || cts(port))) {
            u->shift=u->tx[u->tx_head]; u->tx_head=(u->tx_head+1)%16; u->tx_count--;
            u->shifting=true; u->tx_clock=0;
            if (!u->tx_count) u->thre_irq=true;
        }
        if (u->shifting) {
            u->tx_clock+=clocks;
            if (u->tx_clock >= period) {
                if (u->mcr & 0x10) {
                    receive(u,(u->lcr & 0x40) ? 0 : u->shift);
                    if (u->lcr & 0x40) u->errors |= 0x10;
                    u->shifting=false;
                } else if (port ? (!serial_peer || net_socket_write(serial_peer,u->shift)) : modem_accept(u->shift)) u->shifting=false;
            }
        }
        if (port && serial_peer) {
            unsigned divisor=u->dll|((unsigned)u->dlm<<8);
            net_socket_serial_config(serial_peer,divisor ? 921600/divisor : 14,u->lcr,
                (u->mcr&~2u)|(rts(u)?2:0));
        }
        if (!(u->mcr & 0x10)) {
            // Peer clock is represented by one character interval per transfer.
            // AFE + RTS can hold data at the modem instead of overrunning FIFO.
            peer_clock[port]+=clocks;
            if (peer_clock[port] >= period) {
                peer_clock[port]%=period;
                if ((port ? !(u->mcr&0x20) : !hardware_flow) || rts(u)) {
                    int value=port ? net_socket_read(serial_peer) : modem_output();
                    if (value >= 0) receive(u,(uint8_t)value);
                }
            }
        }
        update_msr(port);
    }
    if (!just_connected) carrier_hold=carrier_hold > clocks ? carrier_hold-clocks : 0;
    if ((streaming || escaped || incoming) && !carrier_hold && (socket_state == NET_CLOSED || socket_state == NET_FAILED) &&
        !net_socket_available(network) && !uarts[0].rx_count && !response_count) {
        // Drain host/serial queues before dropping carrier; prevents final-byte loss.
        streaming=escaped=incoming=false; result(RESULT_CARRIER); restart_listener();
        update_msr(0);
    }
}

bool netcard_attach_serial(const char *endpoint) {
    char host[256],port[6];
    if (!split_endpoint(endpoint,host,sizeof(host),port)) return false;
    if (!serial_peer) serial_peer=net_socket_create();
    if (!serial_peer) return false;
    snprintf(serial_target,sizeof(serial_target),"%s",endpoint);
    return net_socket_connect(serial_peer,host,port);
}

bool netcard_attach_device(const char *path) {
    if (!path || !*path) return false;
    if (!serial_peer) serial_peer=net_socket_create();
    if (!serial_peer) return false;
    return net_socket_open(serial_peer,path,"0",NET_SERIAL,NULL,NULL);
}
