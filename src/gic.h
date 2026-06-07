#pragma once
#include <stdint.h>

void gic_init(void);                 // enable distributor + CPU interface
void gic_enable_irq(uint32_t id);    // enable a specific interrupt id
uint32_t gic_ack(void);              // acknowledge -> returns active id
void gic_eoi(uint32_t id);           // signal end-of-interrupt
