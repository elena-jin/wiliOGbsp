/* Puppy, main CPU: badge-to-badge proximity over one CC1101.
 *
 * Every BEACON_MS (+/- BEACON_JITTER_MS) the radio drops out of RX, sends a
 * 6-byte beacon -- "PUP1" and this badge's random 16-bit id -- and goes
 * straight back to RX. One radio does both jobs, so it cannot hear its own
 * beacon; an id match is still dropped in case two badges roll the same id.
 *
 * A beacon counts only with a good CRC and the signature. NEAR_STREAK
 * beacons in a row from one badge stronger than NEAR_RSSI_DBM send
 * PUP_MSG_NEAR to the display, which runs the same reaction as BLUE. Each
 * badge id then waits NEAR_COOLDOWN_MS before it can trigger again.
 *
 * Every beacon heard is logged with its RSSI; tune NEAR_RSSI_DBM from
 * `fw console --cpu main`.
 */
#include <string.h>
#include "fwog_main.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "puppy_link.h"

/* Every main app must kick the 8.3 s watchdog board_init() arms, and must
   declare that it does -- board_init() references the symbol this macro
   defines, so an app declaring neither policy does not link. The kick itself
   is in the loop below; nothing can check that for you.
   See bsp/main_cpu/watchdog/watchdog.h. */
FWOG_WATCHDOG_DEFAULT();

/* Low on purpose: only a badge within a metre or two should clear
 * NEAR_RSSI_DBM. The PA table goes down to -30 dBm. */
#define PUP_TX_DBM          (-10)
#define RADIO_SPI_HZ        1000000u

#define BEACON_SIG          "PUP1"
#define BEACON_SIG_LEN      4u
#define BEACON_LEN          6u
#define BEACON_MS           500u
/* +/- this much each time, so two badges that collide once drift apart. */
#define BEACON_JITTER_MS    150u

#define NEAR_RSSI_DBM       (-60)
#define NEAR_STREAK         2u
/* Silence this long breaks a streak: "in a row" means consecutive beacons. */
#define STREAK_GAP_MS       1500u
#define NEAR_COOLDOWN_MS    10000u
#define MAX_PEERS           8u

/* Re-enter RX now and then, in case the receiver wedged. */
#define RX_REARM_MS         2000u
#define ANNOUNCE_MS         10000u
#define STATUS_FAST_MS      1000u
#define STATUS_MS           5000u

typedef struct {
    uint16_t id;
    uint8_t streak;
    bool used;
    bool fired;
    uint32_t last_heard;
    uint32_t last_near;
} peer_t;

static cc1101_t s_radio;
static bool s_radio_ok;
static unsigned s_radio_cs;
static bool s_link_ok;
static uint16_t s_my_id;
static peer_t s_peers[MAX_PEERS];

static unsigned s_tx, s_tx_fail, s_rx, s_crc_bad, s_foreign, s_near;

static bool radio_start(cc1101_radio_t which) {
    board_watchdog_kick();
    cc1101_bind(&s_radio, which);
    bool ok = cc1101_bringup(&s_radio);
    ok = ok && cc1101_probe(&s_radio);
    ok = ok && cc1101_set_frequency(&s_radio, PUP_RADIO_HZ);
    ok = ok && cc1101_set_power(&s_radio, PUP_TX_DBM);
    ok = ok && cc1101_idle(&s_radio);
    DIAG("[puppy_main] radio cs%u %s\n", (unsigned)which, ok ? "up" : "FAILED");
    return ok;
}

static void rx_arm(void) {
    if (!s_radio_ok) return;
    cc1101_idle(&s_radio);
    cc1101_flush_rx(&s_radio);
    cc1101_rx(&s_radio);
}

static void send_beacon(void) {
    if (!s_radio_ok) return;
    uint8_t b[BEACON_LEN];
    memcpy(b, BEACON_SIG, BEACON_SIG_LEN);
    b[4] = (uint8_t)(s_my_id & 0xFFu);
    b[5] = (uint8_t)(s_my_id >> 8);
    cc1101_idle(&s_radio);
    if (cc1101_send_packet(&s_radio, b, BEACON_LEN)) s_tx++;
    else s_tx_fail++;
    rx_arm();
}

static void send_near(uint16_t id, int rssi) {
    s_near++;
    if (!s_link_ok) {
        DIAG("[puppy_main] NEAR id=%04x rssi=%d (no link to display)\n", (unsigned)id, rssi);
        return;
    }
    const pup_msg_near_t m = { PUP_MSG_NEAR, id, (int8_t)rssi };
    fwog_link_uart_send_frame(&m, sizeof m);
    DIAG("[puppy_main] NEAR id=%04x rssi=%d -> display\n", (unsigned)id, rssi);
}

static peer_t *peer_for(uint16_t id, uint32_t now) {
    peer_t *free_slot = NULL, *oldest = &s_peers[0];
    for (unsigned i = 0u; i < MAX_PEERS; i++) {
        peer_t *p = &s_peers[i];
        if (p->used && p->id == id) return p;
        if (!p->used) { if (!free_slot) free_slot = p; }
        else if (p->last_heard < oldest->last_heard) oldest = p;
    }
    peer_t *p = free_slot ? free_slot : oldest;
    memset(p, 0, sizeof *p);
    p->id = id;
    p->used = true;
    p->last_heard = now;
    return p;
}

static void heard(uint16_t id, int rssi, unsigned lqi, uint32_t now) {
    peer_t *p = peer_for(id, now);
    if (now - p->last_heard > STREAK_GAP_MS) p->streak = 0;
    p->last_heard = now;
    if (rssi > NEAR_RSSI_DBM) {
        if (p->streak < 255u) p->streak++;
    } else {
        p->streak = 0;
    }
    const bool cooling = p->fired && now - p->last_near < NEAR_COOLDOWN_MS;
    DIAG("[puppy_main] rx id=%04x rssi=%d lqi=%u streak=%u/%u%s\n", (unsigned)id, rssi, lqi,
         (unsigned)p->streak, NEAR_STREAK, cooling ? " cooldown" : "");
    if (p->streak >= NEAR_STREAK && !cooling) {
        p->fired = true;
        p->last_near = now;
        p->streak = 0;
        send_near(id, rssi);
    }
}

static void poll_radio(uint32_t now) {
    if (!s_radio_ok) return;
    const int raw = cc1101_rx_bytes_available(&s_radio);
    if (raw <= 0) return;
    if (raw & 0x80) { rx_arm(); return; }   /* RX FIFO overflow */
    /* GDO0 (IOCFG0 0x06) is high from sync word to end of packet. */
    if (cc1101_get_gdo0(&s_radio)) return;
    const unsigned n = (unsigned)raw & 0x7Fu;

    /* [length][payload...][RSSI][LQI|CRC_OK] -- PKTCTRL1 APPEND_STATUS. */
    uint8_t buf[64];
    const unsigned want = n > sizeof buf ? (unsigned)sizeof buf : n;
    if (cc1101_receive_packet(&s_radio, buf, (uint8_t)want) < 0) { rx_arm(); return; }
    rx_arm();

    const unsigned len = buf[0];
    if (len + 3u > want) { s_foreign++; return; }
    const int rssi = cc1101_rssi_dbm((int8_t)buf[len + 1u]);
    const uint8_t lqi_reg = buf[len + 2u];
    if (!cc1101_lqi_crc_ok(lqi_reg)) {
        s_crc_bad++;
        DIAG("[puppy_main] rx crc-fail len=%u rssi=%d\n", len, rssi);
        return;
    }
    if (len != BEACON_LEN || memcmp(&buf[1], BEACON_SIG, BEACON_SIG_LEN) != 0) {
        s_foreign++;
        return;
    }
    const uint16_t id = (uint16_t)(buf[5] | (buf[6] << 8));
    if (id == s_my_id) return;
    s_rx++;
    heard(id, rssi, cc1101_lqi_value(lqi_reg), now);
}

int main(void) {
    board_init();

    /* Brings the display up: link, HELLO, reflash if the embedded image
       differs, then RUN. This performs board_release_display() itself, at
       the point in the sequence where the announce window is guaranteed to
       be caught -- so applications must NOT call it separately. It leaves
       the link UART up, which NEAR rides on. */
    const fwog_display_result_t d = fwog_display_update_run();
    s_link_ok = d != FWOG_DISP_LINK_DOWN && d != FWOG_DISP_IMAGE_BAD;

    do { s_my_id = (uint16_t)get_rand_32(); } while (s_my_id == 0u);

    cc1101_bus_init(RADIO_SPI_HZ);
    s_radio_cs = 0u;
    s_radio_ok = radio_start(CC1101_RADIO_CS0);
    if (!s_radio_ok) {
        s_radio_cs = 1u;
        s_radio_ok = radio_start(CC1101_RADIO_CS1);
    }
    rx_arm();

    uint32_t now = to_ms_since_boot(get_absolute_time());
    const uint32_t announce_until = now + ANNOUNCE_MS;
    uint32_t next_beacon = now + (get_rand_32() % BEACON_MS);
    uint32_t next_rearm = now + RX_REARM_MS;
    uint32_t next_status = now;

    while (true) {
        board_watchdog_kick();      /* required: see watchdog.h */
        now = to_ms_since_boot(get_absolute_time());

        /* Nothing is expected from the display; drain so RTS never stalls it. */
        uint8_t b;
        while (s_link_ok && fwog_link_uart_read(&b)) {}

        poll_radio(now);

        if ((int32_t)(now - next_beacon) >= 0) {
            send_beacon();
            next_beacon = now + BEACON_MS - BEACON_JITTER_MS +
                          get_rand_32() % (2u * BEACON_JITTER_MS + 1u);
        }
        if ((int32_t)(now - next_rearm) >= 0) {
            rx_arm();
            next_rearm = now + RX_REARM_MS;
        }

        /* Repeated, not printed once: anything written before the host opens
           the CDC port is dropped, and boot is over by then. */
        if ((int32_t)(now - next_status) >= 0) {
            DIAG("[puppy_main] alive id=%04x radio=%s cs%u %u kHz %d dBm near_rssi=%d display=%s "
                 "tx=%u tx_fail=%u rx=%u crc_bad=%u foreign=%u near=%u\n",
                 (unsigned)s_my_id, s_radio_ok ? "up" : "DOWN", s_radio_cs,
                 (unsigned)(PUP_RADIO_HZ / 1000u), PUP_TX_DBM, NEAR_RSSI_DBM,
                 fwog_display_result_text(d), s_tx, s_tx_fail, s_rx, s_crc_bad, s_foreign,
                 s_near);
            next_status = now + ((int32_t)(now - announce_until) < 0 ? STATUS_FAST_MS : STATUS_MS);
        }

        sleep_ms(1);
    }
}
