#include <stdint.h>
#include "gic.h"

// QEMU 'virt' GICv2 memory map.
#define GICD_BASE 0x08000000UL   // Distributor
#define GICC_BASE 0x08010000UL   // CPU interface

#define GICD_CTLR      (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER ((volatile uint32_t *)(GICD_BASE + 0x100))

#define GICC_CTLR (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR  (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR  (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR (*(volatile uint32_t *)(GICC_BASE + 0x010))

void gic_init(void)
{
    GICD_CTLR = 1;       // enable the distributor
    GICC_PMR  = 0xFF;    // priority mask: accept all priorities
    GICC_CTLR = 1;       // enable this CPU's interface
}

void gic_enable_irq(uint32_t id)
{
    GICD_ISENABLER[id / 32] = (1u << (id % 32));
}

uint32_t gic_ack(void)
{
    return GICC_IAR & 0x3FF;   // interrupt id is the low 10 bits
}

void gic_eoi(uint32_t id)
{
    GICC_EOIR = id;
}
