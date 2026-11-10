/* Simulated SimpleBus line for host tests: implements sb_bus.h. */
#ifndef FAKE_BUS_H
#define FAKE_BUS_H
#include <stdint.h>

#define FAKE_MAX_MODULES 16

typedef struct {
    uint8_t bus, addr;
    uint8_t out;        /* relay state */
    int present;
    int drop_ack_at;    /* drop the ack of the Nth press (1-based), 0 = never */
    int presses;
    int stuck;          /* ignore presses but still ack with current state */
} fake_module_t;

void fake_reset(void);
fake_module_t* fake_add(uint8_t bus, uint8_t addr, uint8_t out);
void fake_set_busy(uint8_t bus, int busy);   /* LBT always fails */

/* Event log: "poll 2:6", "press 2:6 btn=1", "sleep 500" ... */
extern char fake_log[4096];
#endif
