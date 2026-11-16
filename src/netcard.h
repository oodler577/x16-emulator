// Optional TL16C2550-compatible serial card. BSD-2-Clause.
#ifndef X16_NETCARD_H
#define X16_NETCARD_H
#include <stdint.h>
#include <stdbool.h>
bool netcard_enable(uint16_t address);
bool netcard_matches(uint16_t address);
bool netcard_attach_device(const char *path);
bool netcard_attach_serial(const char *endpoint);
void netcard_reset(void);
void netcard_close(void);
void netcard_step(unsigned mhz, uint32_t clocks);
bool netcard_irq(void);
uint8_t netcard_read(uint8_t reg, bool debug);
void netcard_write(uint8_t reg, uint8_t value);
#endif
