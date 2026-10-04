/* Puppy on the original FREE-WILi display CPU.
 *
 * The frames are 144x144 (see tools/sprite_convert.py) and sit on the
 * bottom of the 320x240 panel, leaving the 24px the three-step jump
 * needs. Idle bobs. BLUE is the stand-in for another badge being near:
 * the excited frame jumps up in three steps, a three-note beep plays,
 * and the seven LEDs ripple out from the center. Back to idle after
 * three seconds.
 *
 * GREEN plays a rising C5-E5-G5-C6 success arpeggio (~0.8 s), holds the
 * excited frame, and flashes the LEDs green. A short RED press plays a
 * two-second descending A-minor tone with a smooth fade, holds the
 * default frame, and pulses the LEDs red. Presses are ignored while a
 * sound is already playing. A red hold is not a short press: the 6 s
 * power-off path in fwog_power_poll() still sees the button.
 *
 * Screen: st7789_* is 320x240, pixels are st7789_rgb565() (the blit sends
 * them big-endian). Speaker: there is no beep() call. i2s_audio_start()
 * plays a buffer this file synthesizes once, at the driver's 8000 Hz rate.
 */
#include "fwog_display.h"
#include "puppy_sprites.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

FWOG_POWER_DEFAULT();

#define PUPPY_X           ((ST7789_W - PUPPY_W) / 2u)
/* Bottom of the panel. The jump subtracts 3 * JUMP_LIFT from this. */
#define PUPPY_Y           (ST7789_H - PUPPY_H)
#define BOB_MS            600u
#define BOB_LIFT          3u
#define JUMP_STEP_MS      160u
#define JUMP_LIFT         8u
#define REACT_MS          3000u
#define RIPPLE_MS         90u
#define LED_DIV           16u
#define GREEN_FLASH_MS    100u
#define RED_PULSE_MS      40u

#define TONE_RATE         8000u
#define NOTE_SAMPLES      (TONE_RATE * 140u / 1000u)
#define GAP_SAMPLES       (TONE_RATE * 40u / 1000u)
#define BEEP_SAMPLES      (NOTE_SAMPLES * 3u + GAP_SAMPLES * 2u)
#define BEEP_AMPLITUDE    10000

/* Four notes and three short gaps fill 0.8 s exactly:
 * (6400 - 3*160) / 4 = 1480 samples per note. */
#define SUCCESS_SAMPLES   (TONE_RATE * 800u / 1000u)
#define SUCCESS_GAP       (TONE_RATE * 20u / 1000u)
#define SUCCESS_NOTE      ((SUCCESS_SAMPLES - SUCCESS_GAP * 3u) / 4u)
#define SUCCESS_FADE      (TONE_RATE * 40u / 1000u)

/* 2 s at the driver's fixed 8000 Hz, int16. 16000 * 2 = 32000 bytes of RAM. */
#define MINOR_SAMPLES     (TONE_RATE * 2u)
#define MINOR_NOTE        (MINOR_SAMPLES / 4u)

static const uint16_t k_beep_hz[3] = { 523u, 659u, 784u };
static const uint16_t k_success_hz[4] = { 523u, 659u, 784u, 1047u };
/* Descending A minor: E5, C5, A4, A4. The last note is where the fade lands. */
static const uint16_t k_minor_hz[4] = { 659u, 523u, 440u, 440u };

_Static_assert(PUPPY_W <= ST7789_W, "sprite wider than the panel");
_Static_assert(PUPPY_H + 3u * JUMP_LIFT <= ST7789_H,
               "sprite plus the 3-step jump is taller than the panel");
_Static_assert(PUPPY_Y >= 3u * JUMP_LIFT, "jump would clip the top");
_Static_assert(MINOR_SAMPLES == 16000u, "minor tone is 2 s at 8000 Hz");
_Static_assert(MINOR_SAMPLES * sizeof(int16_t) == 32000u,
               "2 s buffer is 32000 bytes");
_Static_assert(SUCCESS_NOTE * 4u + SUCCESS_GAP * 3u == SUCCESS_SAMPLES,
               "success arpeggio must be 0.8 s");

/* 64-step full-scale sine. The I2S driver has no tone generator; the
 * header says to synthesize samples and hand them to i2s_audio_start(). */
static const int16_t k_sin64[64] = {
    0, 3212, 6393, 9512, 12539, 15446, 18204, 20787,
    23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
    32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329,
    23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
    0, -3212, -6393, -9512, -12539, -15446, -18204, -20787,
    -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
    -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
    -23170, -20787, -18204, -15446, -12539, -9512, -6393, -3212
};

enum { POSE_BOB = 0, POSE_JUMP, POSE_SUCCESS, POSE_MINOR };

static int16_t s_beep[BEEP_SAMPLES];
static int16_t s_success[SUCCESS_SAMPLES];
static int16_t s_minor[MINOR_SAMPLES];
static bool s_lcd;
static bool s_leds;
static bool s_audio;

static unsigned s_pose;
static uint32_t s_jump_ms;
static uint32_t s_red_ms;
static bool s_red_down;
static const uint16_t *s_drawn;
static uint16_t s_drawn_y = 0xFFFFu;
static unsigned s_ripple = 0xFFu;
static unsigned s_led_mark = 0xFFFFFFFFu;

static int16_t sample_at(unsigned i, unsigned hz) {
    const uint32_t idx = ((uint32_t)i * hz * 64u) / TONE_RATE;
    return (int16_t)(((int32_t)k_sin64[idx & 63u] * BEEP_AMPLITUDE) / 32767);
}

static void build_beep(void) {
    unsigned at = 0u;
    for (unsigned n = 0u; n < 3u; n++) {
        for (unsigned i = 0u; i < NOTE_SAMPLES; i++) {
            s_beep[at++] = sample_at(i, k_beep_hz[n]);
        }
        if (n + 1u < 3u) {
            for (unsigned i = 0u; i < GAP_SAMPLES; i++) s_beep[at++] = 0;
        }
    }
}

static void build_success(void) {
    unsigned at = 0u;
    for (unsigned n = 0u; n < 4u; n++) {
        for (unsigned i = 0u; i < SUCCESS_NOTE; i++) {
            s_success[at++] = sample_at(i, k_success_hz[n]);
        }
        if (n + 1u < 4u) {
            for (unsigned i = 0u; i < SUCCESS_GAP; i++) s_success[at++] = 0;
        }
    }
    /* Quick linear fade on the last 40 ms, so C6 does not click off. */
    for (unsigned i = 0u; i < SUCCESS_FADE; i++) {
        const unsigned pos = SUCCESS_SAMPLES - SUCCESS_FADE + i;
        s_success[pos] = (int16_t)(((int32_t)s_success[pos] *
                                    (int32_t)(SUCCESS_FADE - 1u - i)) /
                                   (int32_t)SUCCESS_FADE);
    }
}

/* Quarter-cosine from the sine table: index 16 is full scale, index 32 is
 * zero. The first quarter stays loud; the rest fades out to silence. */
static void fade_out(int16_t *buf, unsigned n) {
    const unsigned tail = (n * 3u) / 4u;
    const unsigned start = n - tail;
    for (unsigned i = 0u; i < tail; i++) {
        const unsigned env_i = 16u + (i * 16u) / (tail - 1u);
        const unsigned pos = start + i;
        buf[pos] = (int16_t)(((int32_t)buf[pos] * k_sin64[env_i]) / 32767);
    }
}

static void build_minor(void) {
    for (unsigned n = 0u; n < 4u; n++) {
        for (unsigned i = 0u; i < MINOR_NOTE; i++) {
            s_minor[n * MINOR_NOTE + i] = sample_at(i, k_minor_hz[n]);
        }
    }
    fade_out(s_minor, MINOR_SAMPLES);
}

static void led_set(unsigned pixel, uint8_t r, uint8_t g, uint8_t b) {
    ws2812_set_color(pixel, (uint8_t)(r / LED_DIV), (uint8_t)(g / LED_DIV),
                     (uint8_t)(b / LED_DIV));
}

static void leds_off(void) {
    if (!s_leds) return;
    for (unsigned i = 0u; i < FWOG_LED_COUNT; i++) led_set(i, 0u, 0u, 0u);
    ws2812_process();
    s_led_mark = 0xFFFFFFFFu;
}

static void leds_fill(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_leds) return;
    for (unsigned i = 0u; i < FWOG_LED_COUNT; i++) led_set(i, r, g, b);
    ws2812_process();
}

/* Center pixel is index 3 of 7. radius 0 lights the center, then each
 * step lights the next pair outward and leaves a dimmer trail. */
static void leds_ripple(unsigned radius) {
    if (!s_leds) return;
    for (unsigned i = 0u; i < FWOG_LED_COUNT; i++) {
        const unsigned center = FWOG_LED_COUNT / 2u;
        const unsigned dist = (i > center) ? (i - center) : (center - i);
        if (dist == radius) led_set(i, 255u, 210u, 48u);
        else if (dist < radius) led_set(i, 96u, 48u, 160u);
        else led_set(i, 0u, 0u, 0u);
    }
    ws2812_process();
}

static bool sound_busy(void) {
    return s_audio && !i2s_audio_is_idle();
}

static bool play(const int16_t *buf, unsigned n, const char *name) {
    if (!s_audio || sound_busy()) return false;
    const bool started = i2s_audio_start(buf, n, true, false);
    DIAG("[puppy] %s %s (%u bytes)\n", name, started ? "started" : "failed",
         n * (unsigned)sizeof(int16_t));
    return started;
}

/* BLUE passes 0, 0. peer_id and closeness are unused stand-in arguments. */
static void on_peer_near(uint8_t peer_id, int8_t closeness) {
    s_pose = POSE_JUMP;
    s_jump_ms = to_ms_since_boot(get_absolute_time());
    s_ripple = 0xFFu;
    DIAG("[puppy] state=jump peer=%u closeness=%d\n", (unsigned)peer_id, (int)closeness);
    (void)play(s_beep, BEEP_SAMPLES, "beep");
}

static void pose(uint32_t now, const uint16_t **frame, uint16_t *y) {
    if (s_pose == POSE_SUCCESS || s_pose == POSE_MINOR) {
        if (!sound_busy()) {
            s_pose = POSE_BOB;
            leds_off();
            DIAG("[puppy] state=idle\n");
        } else {
            *frame = (s_pose == POSE_SUCCESS) ? puppy_excited : puppy_default;
            *y = (uint16_t)PUPPY_Y;
            return;
        }
    }
    if (s_pose == POSE_JUMP) {
        const uint32_t dt = now - s_jump_ms;
        if (dt >= REACT_MS) {
            s_pose = POSE_BOB;
            s_ripple = 0xFFu;
            leds_off();
            DIAG("[puppy] state=idle\n");
        } else {
            const unsigned step = dt / JUMP_STEP_MS;
            const unsigned lift = (step >= 2u) ? 2u : step;
            *frame = puppy_excited;
            *y = (uint16_t)(PUPPY_Y - (lift + 1u) * JUMP_LIFT);
            return;
        }
    }
    if (((now / BOB_MS) & 1u) == 0u) {
        *frame = puppy_default;
        *y = (uint16_t)PUPPY_Y;
    } else {
        *frame = puppy_excited;
        *y = (uint16_t)(PUPPY_Y - BOB_LIFT);
    }
}

/* The uncovered strip only. Clearing the whole sprite first would blank
 * a 144px frame on every bob. The move is always a few pixels, so the
 * old and new rects overlap and this strip is the trail. */
static void clear_trail(uint16_t y) {
    if (s_drawn_y == 0xFFFFu || y == s_drawn_y) return;
    if (y > s_drawn_y) {
        st7789_fill_rect(PUPPY_X, s_drawn_y, (uint16_t)PUPPY_W,
                         (uint16_t)(y - s_drawn_y), PUPPY_BG);
    } else {
        st7789_fill_rect(PUPPY_X, (uint16_t)(y + PUPPY_H), (uint16_t)PUPPY_W,
                         (uint16_t)(s_drawn_y - y), PUPPY_BG);
    }
}

static void draw_puppy(const uint16_t *frame, uint16_t y) {
    if (!s_lcd) return;
    if (frame == s_drawn && y == s_drawn_y) return;
    clear_trail(y);
    st7789_set_window(PUPPY_X, y, (uint16_t)PUPPY_W, (uint16_t)PUPPY_H);
    st7789_blit(frame, (size_t)PUPPY_W * (size_t)PUPPY_H);
    s_drawn = frame;
    s_drawn_y = y;
}

static void leds_tick(uint32_t now, bool ship_armed) {
    if (!s_leds || ship_armed) return;
    if (s_pose == POSE_JUMP) {
        const unsigned step = ((now - s_jump_ms) / RIPPLE_MS) % 4u;
        if (step == s_ripple) return;
        s_ripple = step;
        leds_ripple(step);
        return;
    }
    if (s_pose == POSE_SUCCESS) {
        const unsigned mark = now / GREEN_FLASH_MS;
        if (mark == s_led_mark) return;
        s_led_mark = mark;
        if ((mark & 1u) == 0u) leds_fill(40u, 255u, 64u);
        else leds_fill(0u, 0u, 0u);
        return;
    }
    if (s_pose == POSE_MINOR) {
        const unsigned mark = now / RED_PULSE_MS;
        if (mark == s_led_mark) return;
        s_led_mark = mark;
        const unsigned phase = mark & 31u;
        const unsigned tri = (phase < 16u) ? phase : (31u - phase);
        /* 32 survives /LED_DIV as a dim red; 32+15*14 = 242 is near full. */
        leds_fill((uint8_t)(32u + tri * 14u), 0u, 0u);
    }
}

static void lcd_bringup(void) {
    st7789_init_begin();
    const absolute_time_t deadline = make_timeout_time_ms(500);
    while (!st7789_ready() && !time_reached(deadline)) {
        st7789_init_step();
        sleep_ms(1);
    }
    s_lcd = st7789_ready();
    if (!s_lcd) {
        DIAG("[puppy] LCD init failed; LEDs and the beep still run\n");
        return;
    }
    st7789_clear(PUPPY_BG);
    st7789_dma_wait();
    board_backlight(255);
    DIAG("[puppy] LCD ready %ux%u\n", (unsigned)ST7789_W, (unsigned)ST7789_H);
}

int main(void) {
    board_init();
    build_beep();
    build_success();
    build_minor();
    DIAG("[puppy] sprites %u bytes flash, minor tone %u bytes RAM\n",
         (unsigned)(2u * PUPPY_W * PUPPY_H * sizeof(uint16_t)),
         (unsigned)sizeof s_minor);
    lcd_bringup();

    s_leds = ws2812_init(pio0, 0u);
    if (s_leds) {
        leds_off();
        /* The first frame after the PIO state machine starts does not
         * latch every pixel. Push the cleared strip more than once. */
        ws2812_process();
        ws2812_process();
    } else {
        DIAG("[puppy] WS2812 init failed\n");
    }

    s_audio = i2s_audio_init(pio0, 2u);
    if (s_audio) i2s_audio_set_volume(6);
    else DIAG("[puppy] speaker init failed; no beep\n");

    DIAG("[puppy] state=idle\n");

    while (true) {
        const uint32_t now = to_ms_since_boot(get_absolute_time());
        const fwog_power_t power = fwog_power_poll(now);
        if (s_audio) i2s_audio_process();

        const uint8_t pressed = power.buttons.pressed;
        const uint8_t released = power.buttons.released;

        if (pressed & FWOG_BTN_BIT(FWOG_BTN_RED)) {
            s_red_down = true;
            s_red_ms = now;
        }
        if ((released & FWOG_BTN_BIT(FWOG_BTN_RED)) && s_red_down) {
            const uint32_t held = now - s_red_ms;
            s_red_down = false;
            /* Release before the 6 s hold completes. fwog_power_poll() has
             * already dropped the countdown on this same sample, and a hold
             * that reaches FWOG_SHIP_HOLD_MS powers off instead of landing
             * here. sound_busy() is read again below so a tone started here
             * blocks blue and green in this same pass. */
            if (power.armed || held >= FWOG_SHIP_HOLD_MS) {
                DIAG("[puppy] red hold %u ms, not a tone\n", (unsigned)held);
            } else if (sound_busy()) {
                DIAG("[puppy] red ignored, sound busy\n");
            } else if (play(s_minor, MINOR_SAMPLES, "minor")) {
                s_pose = POSE_MINOR;
                s_led_mark = 0xFFFFFFFFu;
                DIAG("[puppy] state=minor held=%u\n", (unsigned)held);
            }
        }

        if (pressed & FWOG_BTN_BIT(FWOG_BTN_BLUE)) {
            if (sound_busy()) DIAG("[puppy] blue ignored, sound busy\n");
            else {
                DIAG("[puppy] blue pressed\n");
                on_peer_near(0u, 0);
            }
        }

        if (pressed & FWOG_BTN_BIT(FWOG_BTN_GREEN)) {
            DIAG("[puppy] green pressed\n");
            if (sound_busy()) DIAG("[puppy] green ignored, sound busy\n");
            else if (play(s_success, SUCCESS_SAMPLES, "success")) {
                s_pose = POSE_SUCCESS;
                s_led_mark = 0xFFFFFFFFu;
                DIAG("[puppy] state=success\n");
            }
        }

        const uint16_t *frame = puppy_default;
        uint16_t y = (uint16_t)PUPPY_Y;
        pose(now, &frame, &y);
        draw_puppy(frame, y);
        leds_tick(now, power.armed);

        sleep_ms(2);
    }
}
