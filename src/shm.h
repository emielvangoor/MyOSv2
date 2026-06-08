// shm.h -- shared-memory objects: physical pages that can be mapped into several
// address spaces at once, so processes share writes (the basis for IPC).
#pragma once
#include <stdint.h>

struct addrspace;   // from vm.h

void     shm_init(void);                            // reset the object table (boot/tests)
int      shm_create(uint64_t len);                  // allocate an object -> handle (or -1)
uint64_t shm_map(struct addrspace *as, int handle); // map it into `as` -> base VA (0 on fail)
