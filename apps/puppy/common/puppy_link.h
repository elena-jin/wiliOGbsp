/* Puppy: what main tells display over the inter-CPU link, and the radio
 * settings both CPUs must agree on.
 *
 * Main owns the CC1101s; display owns the PCAL6416 that steers their
 * antenna paths. So the carrier lives here, next to the antenna path that
 * serves it, and each CPU applies its half.
 *
 * Message types start at 0x40: 0x00 is invalid, 0x01-0x1F belong to the
 * display bootloader and 0x20-0x22 to the BSP's I/O messages
 * (common/link/link_frame.h, common/link/io_proto.h). */
#ifndef PUPPY_LINK_H
#define PUPPY_LINK_H
#include <stdint.h>
#include "common/io_cfg.h"

/* US 902-928 MHz ISM. The 900 MHz antenna path is confirmed at 915 MHz on
 * hardware (tools/ant_sweep.py). For 433.92 MHz, pair it with
 * FWOG_ANT_400MHZ, the path the expander powers up in. */
#define PUP_RADIO_HZ   915000000u
#define PUP_ANTENNA    FWOG_ANT_900MHZ

/* main -> display: another badge has been close for long enough. */
#define PUP_MSG_NEAR   0x40u
typedef struct __attribute__((packed)) {
    uint8_t type;       /* PUP_MSG_NEAR */
    uint16_t peer_id;   /* the other badge's random per-boot id */
    int8_t rssi_dbm;    /* the beacon that tipped it over */
} pup_msg_near_t;

_Static_assert(sizeof(pup_msg_near_t) == 4u, "pup_msg_near_t layout");

#endif
