#include <stdint.h>
extern uint16_t fbmem[0x10000];
#define OVR16 ((volatile uint16_t *)fbmem)
#define MARKS_UNCACHED(p) ((uint32_t (*)[2])(p))
#define R65_HOST_SIM 1
