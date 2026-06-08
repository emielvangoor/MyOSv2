// gic.c -- driver for the GICv2 (Generic Interrupt Controller, version 2).
// =======================================================================
//
// Devices don't signal the CPU directly; they go through the GIC, which acts as
// a switchboard: it knows which interrupts are enabled, their priorities, and
// which CPU should get them. GICv2 has TWO register blocks:
//
//   * Distributor (GICD): global, system-wide config -- which interrupt ids are
//     enabled, their priorities and routing.
//   * CPU Interface (GICC): per-core -- where THIS core acknowledges an
//     incoming interrupt and signals completion.
//
// The runtime handshake for each interrupt is:
//   1. read GICC_IAR  -> "acknowledge", returns the id that fired
//   2. ...handle it...
//   3. write GICC_EOIR -> "end of interrupt", lets the GIC send the next one

#include <stdint.h>
#include "gic.h"

// MMIO base addresses of the two blocks on the QEMU 'virt' board.
#define GICD_BASE 0x08000000UL   // Distributor
#define GICC_BASE 0x08010000UL   // CPU interface

// --- Distributor registers ---
#define GICD_CTLR      (*(volatile uint32_t *)(GICD_BASE + 0x000)) // global enable
// ISENABLER is an ARRAY of 32-bit words; each bit enables one interrupt id.
// Word n covers ids [32n .. 32n+31]. We index it as GICD_ISENABLER[id/32].
#define GICD_ISENABLER ((volatile uint32_t *)(GICD_BASE + 0x100))

// --- CPU interface registers ---
#define GICC_CTLR (*(volatile uint32_t *)(GICC_BASE + 0x000)) // enable this core's iface
#define GICC_PMR  (*(volatile uint32_t *)(GICC_BASE + 0x004)) // priority mask
#define GICC_IAR  (*(volatile uint32_t *)(GICC_BASE + 0x00C)) // acknowledge (read)
#define GICC_EOIR (*(volatile uint32_t *)(GICC_BASE + 0x010)) // end-of-interrupt (write)

void gic_init(void)
{
    GICD_CTLR = 1;       // turn the distributor on
    GICC_PMR  = 0xFF;    // priority mask = lowest possible threshold, i.e. accept
                         //   interrupts of ANY priority (smaller number = higher
                         //   priority; 0xFF lets everything through)
    GICC_CTLR = 1;       // turn this core's CPU interface on
}

void gic_enable_irq(uint32_t id)
{
    // A shared peripheral interrupt (id >= 32) needs a priority and a target CPU
    // before it will be delivered. On QEMU's single-core 'virt' the target
    // register is read-as-zero/write-ignored (SPIs go to the only CPU anyway),
    // but the priority write is what actually lets the interrupt through.
    // PPIs/SGIs (id < 32) are always delivered to the local core.
    volatile uint8_t *pri = (volatile uint8_t *)(GICD_BASE + 0x400); // IPRIORITYR
    volatile uint8_t *tgt = (volatile uint8_t *)(GICD_BASE + 0x800); // ITARGETSR
    if (id >= 32) { pri[id] = 0x00; tgt[id] = 0x01; }
    // Set the one bit that enables this interrupt id in the distributor.
    GICD_ISENABLER[id / 32] = (1u << (id % 32));
}

uint32_t gic_ack(void)
{
    // Reading IAR acknowledges the highest-priority pending interrupt and
    // returns its id. The id is in the low 10 bits.
    return GICC_IAR & 0x3FF;
}

void gic_eoi(uint32_t id)
{
    // Writing the same id back to EOIR tells the GIC we've finished handling it.
    GICC_EOIR = id;
}
