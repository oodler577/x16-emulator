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
static net_socket *network;
static bool enabled, streaming, dialing, quiet, echo_on, numeric, hardware_flow;
static unsigned base_address, eol_mode, hangup_mode, command_length;
static unsigned long modem_baud=115200;
static bool command_overflow;
static char command[256];
static uint8_t responses[RESPONSE_SIZE];
static unsigned response_head, response_count;
static uint64_t peer_clock, carrier_hold;

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
enum { RESULT_OK, RESULT_CONNECT, RESULT_CARRIER, RESULT_ERROR, RESULT_ANSWER };
static void result(unsigned code) {
    static const char *words[]={"OK","CONNECT","NO CARRIER","ERROR","NO ANSWER"};
    static const char *numbers[]={"0","1","3","4","8"};
    if (quiet) return;
    queue_eol(); queue_text(numeric ? numbers[code] : words[code]); queue_eol();
}
static void hangup(bool notice) {
    bool was_connected=streaming || dialing;
    net_socket_disconnect(network);
    streaming=dialing=false; carrier_hold=0;
    if (notice && was_connected) result(RESULT_CARRIER);
}
static void modem_reset(void) {
    hangup(false);
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
    response_head=response_count=0; peer_clock=0;
    modem_baud=115200;
    modem_reset();
}
void netcard_close(void) {
    enabled=false;
    net_socket_destroy(network); network=NULL;
}
static bool rts(const uart_state *u) {
    return (u->mcr & 2) && (!(u->mcr & 0x20) || u->rx_count < trigger(u));
}
static bool cts(unsigned port) {
    if (port) return false; // no host RS-232 peer attached
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
              ((!port && streaming) ? 0x80 : 0);
    }
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
static bool start_dial(const char *text) {
    char endpoint[256];
    size_t length=strlen(text);
    if (length < 3 || text[0] != '"' || text[length-1] != '"' || length >= sizeof(endpoint))
        return false;
    memcpy(endpoint,text+1,length-2); endpoint[length-2]=0;
    char *port=strrchr(endpoint,':');
    if (!port || port == endpoint || !port[1]) return false;
    *port++=0;
    for (char *p=port; *p; p++) if (!isdigit((unsigned char)*p)) return false;
    unsigned long value=strtoul(port,NULL,10);
    if (!value || value > 65535) return false;
    char *host=endpoint;
    if (host[0]=='[') {
        size_t n=strlen(host);
        if (n < 3 || host[n-1]!=']') return false;
        host[n-1]=0; host++;
    }
    if (!net_socket_connect(network,host,port)) return false;
    dialing=true;
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
        if (op == 'D') {
            valid=start_dial(p);
            if (valid) return; // worker supplies CONNECT/NO ANSWER later
            break;
        }
        if (op == '&') {
            op=(unsigned char)toupper((unsigned char)*p++);
            if (!op) { valid=false; break; }
            value=number(&p,&found);
            if (op == 'W') { valid=!found || value == 0; }
            else if (op == 'P') valid=found && value == 0;
            else if (op == 'K' && found && (value == 0 || value == 3)) hardware_flow=value == 3;
            else valid=false;
            continue;
        }
        if (op == 'S') {
            unsigned long index=number(&p,&found);
            if (!found || *p++ != '=') { valid=false; break; }
            value=number(&p,&found);
            if (!found) { valid=false; break; }
            if (index == 63 && (value == 0 || value == 2 || value == 3)) hangup_mode=(unsigned)value;
            else if ((index == 46 || index == 48 || index == 50 || index == 54 || index == 62 || index == 0) && value == 0) { }
            else if (index == 45 && value <= 3) { } // AT&G itself is not implemented
            else valid=false;
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
        case 'N': valid=found && value == 0; break; // no inbound listeners exist
        case 'H': valid=!found || value == 0; if (valid) hangup(false); break;
        case 'Z': valid=!found || value == 0; if (valid) modem_reset(); break;
        case 'I': {
            valid=!found || value == 0 || value == 4;
            if (valid) { queue_eol(); queue_text("X16 host TCP modem (Zimodem-compatible subset)"); queue_eol(); }
            break;
        }
        default: valid=false; break;
        }
    }
    result(valid ? RESULT_OK : RESULT_ERROR);
}
static bool modem_accept(uint8_t value) {
    if (streaming) return net_socket_write(network,value);
    if (dialing) return false;
    if (echo_on) queue_byte(value);
    if (value == 10 && !command_length) return true;
    if (value == 13) {
        command[command_length]=0;
        if (command_overflow || command_length < 2) result(RESULT_ERROR);
        else execute_command();
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
                hangup(true);
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
    return streaming ? net_socket_read(network) : -1;
}
void netcard_step(unsigned mhz,uint32_t clocks) {
    if (!enabled) return;
    int socket_state=net_socket_state(network);
    bool just_connected=false;
    if (dialing && (socket_state == NET_CONNECTED || net_socket_established(network))) {
        dialing=false; streaming=true; result(RESULT_CONNECT);
        carrier_hold=4*character_clocks(&uarts[0],mhz); just_connected=true;
    } else if (dialing && (socket_state == NET_FAILED || socket_state == NET_CLOSED)) {
        dialing=false; result(RESULT_ANSWER);
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
                } else if (port || modem_accept(u->shift)) u->shifting=false;
            }
        }
        if (!port && !(u->mcr & 0x10)) {
            // Peer clock is represented by one character interval per transfer.
            // AFE + RTS can hold data at the modem instead of overrunning FIFO.
            peer_clock+=clocks;
            if (peer_clock >= period) {
                peer_clock%=period;
                if (!hardware_flow || rts(u)) {
                    int value=modem_output();
                    if (value >= 0) receive(u,(uint8_t)value);
                }
            }
        }
        update_msr(port);
    }
    if (!just_connected) carrier_hold=carrier_hold > clocks ? carrier_hold-clocks : 0;
    if (streaming && !carrier_hold && (socket_state == NET_CLOSED || socket_state == NET_FAILED) &&
        !net_socket_available(network) && !uarts[0].rx_count && !response_count) {
        // Drain host/serial queues before dropping carrier; prevents final-byte loss.
        streaming=false; result(RESULT_CARRIER);
        update_msr(0);
    }
}
