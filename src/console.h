// console.h -- the terminal line discipline on top of the raw UART.
// =================================================================
//
// This is the tty layer (in the Unix sense): the UART driver moves raw bytes,
// and the console turns them into terminal behaviour. Input is interrupt-driven
// -- a received byte runs through console_input(), which either turns Ctrl-C
// into a SIGINT for the foreground program or queues the byte and wakes a reader
// that is BLOCKED (asleep, not spinning) in console_getc().
#pragma once

void console_input(int c);   // line discipline for one received byte
int  console_getc(void);     // block until a byte is available; -1 if a signal interrupts
int  console_has_input(void);// is a byte queued? (non-blocking)
void console_isr(void);      // UART receive-interrupt handler (drains the device)

void tty_intr(void);              // deliver SIGINT to the terminal's foreground group/thread
void tty_set_fg_pgrp(int pgrp);   // set the terminal's foreground process group (tcsetpgrp)
int  tty_fg_pgrp(void);           // the terminal's foreground process group (0 = unset)
