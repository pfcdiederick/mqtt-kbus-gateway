// Simulated KBUS for testing without a PFC (KBUS_SIMULATION=1).
//
// Simulated node (same process-image layout rules as the real KBUS: complex /
// analog data first, digital data packed behind it):
//   pos 1  750-455  4AI   in  bits   0..63  (AI1..AI3 ramps, AI4 = loopback of AO1)
//   pos 2  750-559  4AO   out bits   0..63
//   pos 3  750-450  4RTD  in  bits  64..127 (AI5..AI8: 21.4, -12.5, 850.0 degC, AI8 = AO2)
//   pos 4  750-671  stepper, 12 bytes in bits 128..223, out bits 64..159 (model of the
//                   positioning handshake, see sim_stepper)
//   pos 5  750-430  8DI   in  bits 224..231 (loopback of DO1..DO8)
//   pos 6  750-530  8DO   out bits 160..167
//   pos 7  750-402  4DI   in  bits 232..235 (binary seconds counter)
//
// Register communication (read only, KBUS_REG_*): every module except the digital ones,
// one table per channel. Register 6 of the 750-450 = 0x0060 while the channel shows
// 850.0 degC or more (like a real 750-450 with an open input), else 0; register 8 =
// module type; register 30 = type * 10 (order number); everything else 0. 20 ms per read.
//
// Hot-plug test (KBUS_SIM_HOTPLUG=<file>): while <file> exists, an 8th module
// "was added" (750-430 8DI, in bits 240..247, all inputs TRUE). Until the bus is
// opened again, every cycle fails, as the real KBUS does after a change; the
// next open (rescan) then reports 6 modules. Delete the file to remove it.
//
// Hang test (KBUS_SIM_HANG=<file>): while <file> exists, the exchange does not return
// (like a bus call that hangs inside the WAGO library): tests the hang detector.
#include "backend.h"
#include "log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int sim_extra;          // 1 = the 6th module was present at the last open

// For the register reads (other thread than the cycle): modules and input image.
static pthread_mutex_t sim_lock = PTHREAD_MUTEX_INITIALIZER;
static kbus_module_t   sim_mods[8];
static size_t          sim_count;
static uint8_t         sim_in[32];

static int hotplug_present(void)
{
    const char *f = getenv("KBUS_SIM_HOTPLUG");
    return f != NULL && *f != '\0' && access(f, F_OK) == 0;
}

static void put16(uint8_t *img, size_t byte, uint16_t v)
{
    img[byte]     = (uint8_t)(v & 0xffu);
    img[byte + 1] = (uint8_t)(v >> 8);
}

static int sim_open(kbus_backend_t *self, kbus_module_t *m, size_t max_modules,
                    size_t *module_count, size_t *in_bytes, size_t *out_bytes)
{
    (void)self;
    static const kbus_module_t node[] = {
        { 1, 455,     0, 64,  0,  0, 4, 0 },
        { 2, 559,     0,  0,  0, 64, 4, 0 },
        { 3, 450,    64, 64,  0,  0, 4, 0 },  // RTD, 4 channels
        { 4, 671,   128, 96, 64, 96, 0, 0 },  // stepper controller, 12 bytes in/out
        { 5, 0x8801, 224, 8,  0,  0, 8, 0 },  // digital, 8 channels, input
        { 6, 0x8802,  0,  0, 160, 8, 8, 0 },  // digital, 8 channels, output
        { 7, 0x8401, 232, 4,  0,  0, 4, 0 },  // digital, 4 channels, input
        { 8, 0x8801, 240, 8,  0,  0, 8, 0 },  // hot-plug test only: digital, 8 channels, input
    };
    sim_extra = hotplug_present();
    size_t n = sizeof(node) / sizeof(node[0]) - (sim_extra ? 0 : 1);
    if (n > max_modules)
        n = max_modules;
    memcpy(m, node, n * sizeof(node[0]));
    pthread_mutex_lock(&sim_lock);
    memcpy(sim_mods, node, n * sizeof(node[0]));
    sim_count = n;
    pthread_mutex_unlock(&sim_lock);
    *module_count = n;
    *in_bytes = sim_extra ? 31 : 30;
    *out_bytes = 21;
    LOG_WARN("SIMULATION mode: no real KBUS I/O is read or written");
    return 0;
}

// 750-671 model (manual 750-671, 2.1.2.11.1): enough of the positioning handshake to
// test the driver. o = 12 output bytes (C0, -, D0..D6, C3, C2, C1), i = 12 input bytes.
static struct { int32_t pos, target, vel; int busy, on_target, ref_ok, error, reset, ref_left; uint8_t c1, c2, c3; } stp = { .reset = 1 };

static void sim_stepper(const uint8_t *o, uint8_t *i, uint32_t dt_ms)
{
    const uint8_t c1 = o[11], c2 = o[10], c3 = o[9];
    if ((c2 & 0x80) && !(stp.c2 & 0x80))
        stp.error = 0;                                     // Error_Quit edge
    if ((c3 & 0x80) && !(stp.c3 & 0x80))
        stp.reset = 0;                                     // Reset_Quit edge
    const int ready = (c1 & 0x01) && !stp.error;
    const int stop_ack = ready && (c1 & 0x02);              // Input1 is closed in the simulation
    uint8_t mode_ack = 0;
    if (stop_ack)
        mode_ack = c1 & 0xF8;                               // one mode at a time is enough here
    if (!ready || !(mode_ack & c1)) {                       // mode ended: stop (at once here)
        stp.busy = 0;
        stp.vel = 0;
    }
    const int start_ack = (c1 & 0x04) && mode_ack;
    if ((c1 & 0x04) && !(stp.c1 & 0x04) && mode_ack) {      // Start rising edge
        const int32_t v = (int32_t)(int16_t)(o[2] | (o[3] << 8));
        int32_t t = o[6] | (o[7] << 8) | (o[8] << 16);
        if (t & 0x800000)
            t -= 0x1000000;
        if (mode_ack & 0x08) {                              // positioning
            if (v < 1 || v > 25000) {
                stp.error = 1;
            } else {
                stp.target = t;
                stp.vel = v;
                stp.busy = stp.pos != t;
                stp.on_target = stp.pos == t;
            }
        } else if (mode_ack & 0x20) {                       // referencing: 0.5 s, then position 0
            stp.busy = 1;
            stp.on_target = 0;
            stp.ref_ok = 0;
            stp.ref_left = 500;
        }
    }
    if (stp.busy && (mode_ack & 0x08)) {
        int32_t step = (int32_t)((int64_t)stp.vel * dt_ms / 1000);
        if (step < 1)
            step = 1;
        const int32_t d = stp.target - stp.pos;
        stp.pos += d > step ? step : d < -step ? -step : d;
        if (stp.pos == stp.target) {
            stp.busy = 0;
            stp.on_target = 1;
        }
    } else if (stp.busy && (mode_ack & 0x20)) {
        stp.ref_left -= (int)dt_ms;
        stp.pos += (c3 & 0x08) ? -1 : 1;
        if (stp.ref_left <= 0) {
            stp.busy = 0;
            stp.pos = 0;
            stp.ref_ok = 1;
            stp.on_target = 1;
        }
    }
    stp.c1 = c1;
    stp.c2 = c2;
    stp.c3 = c3;
    const int16_t av = stp.busy ? (int16_t)stp.vel : 0;
    memset(i, 0, 12);
    i[0] = stp.error ? 0x40 : 0;
    i[2] = (uint8_t)av;
    i[3] = (uint8_t)(av >> 8);
    i[6] = (uint8_t)stp.pos;
    i[7] = (uint8_t)(stp.pos >> 8);
    i[8] = (uint8_t)(stp.pos >> 16);
    i[9] = (uint8_t)((stp.reset ? 0x80 : 0) | 0x01);                       // S3: Reset, Input1
    i[10] = (uint8_t)((stp.error ? 0x80 : 0) | (stp.ref_ok ? 0x20 : 0) | (stp.busy ? 0x02 : 0x04)
                      | (stp.on_target ? 0x01 : 0));                        // S2
    i[11] = (uint8_t)((ready ? 0x01 : 0) | (stop_ack ? 0x02 : 0) | (start_ack ? 0x04 : 0) | mode_ack); // S1
}

static int sim_exchange(kbus_backend_t *self, const uint8_t *out, size_t out_len,
                        uint8_t *in, size_t in_len)
{
    (void)self;
    if (in_len < 30 || out_len < 21)
        return -1;
    if (hotplug_present() != sim_extra)
        return -1;                                                 // node changed: bus error until rescan
    const char *hang = getenv("KBUS_SIM_HANG");
    while (hang != NULL && *hang != '\0' && access(hang, F_OK) == 0)
        usleep(50 * 1000);                                         // hangs while the file exists
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint32_t ms = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
    static uint32_t last_ms;
    const uint32_t dt = last_ms ? ms - last_ms : 0;
    last_ms = ms;

    put16(in, 0, (uint16_t)((ms / 2) % 32768u));                 // AI1: sawtooth ~16 s
    uint32_t tri = (ms / 4) % 65536u;                              // AI2: triangle
    put16(in, 2, (uint16_t)(tri < 32768u ? tri : 65535u - tri));
    put16(in, 4, 16384);                                           // AI3: constant
    in[6] = out[0];                                                // AI4 = AO1
    in[7] = out[1];
    put16(in, 8, 214);                                             // AI5: 21.4 degC
    put16(in, 10, (uint16_t)(int16_t)-125);                        // AI6: -12.5 degC
    put16(in, 12, 8500);                                           // AI7: 850.0 degC
    in[14] = out[2];                                               // AI8 = AO2 (raw)
    in[15] = out[3];
    sim_stepper(out + 8, in + 16, dt);                             // 750-671 at position 4
    in[28] = out[20];                                              // DI1..8 = DO1..8
    in[29] = (uint8_t)(now.tv_sec & 0x0f);                         // DI9..12 = counter
    if (sim_extra && in_len >= 31)
        in[30] = 0xff;                                             // DI13..20 (hot-plugged 8DI)
    pthread_mutex_lock(&sim_lock);
    memcpy(sim_in, in, in_len < sizeof(sim_in) ? in_len : sizeof(sim_in));
    pthread_mutex_unlock(&sim_lock);
    return 0;
}

static int sim_reg_read(kbus_backend_t *self, uint16_t terminal, uint16_t table, uint16_t reg,
                        uint16_t count, uint16_t *buf, uint16_t *n)
{
    (void)self;
    usleep(20000);                                                 // like the real register access
    pthread_mutex_lock(&sim_lock);
    int rc = KBUS_REG_FAILED;
    if (terminal >= 1 && terminal <= sim_count) {
        const kbus_module_t *m = &sim_mods[terminal - 1];
        const unsigned tables = m->channel_count ? m->channel_count : 1;
        if (!(m->type & 0x8000u) && table < tables) {
            for (uint16_t i = 0; i < count; ++i) {
                const unsigned r = reg + i;
                uint16_t v = 0;
                if (r == 8)
                    v = m->type;
                else if (r == 30)
                    v = (uint16_t)(m->type * 10u);
                else if (r == 6 && m->type == 450) {
                    const size_t b = m->in_offset_bits / 8u + 2u * table;
                    const int16_t w = (int16_t)(sim_in[b] | (sim_in[b + 1] << 8));
                    v = w >= 8500 ? 0x0060 : 0;
                }
                buf[i] = v;
            }
            *n = count;
            rc = KBUS_REG_OK;
        }
    }
    pthread_mutex_unlock(&sim_lock);
    return rc;
}

static void sim_close(kbus_backend_t *self)
{
    (void)self;
}

kbus_backend_t *backend_sim_create(const gw_config_t *cfg)
{
    (void)cfg;
    static kbus_backend_t backend = {
        .name = "simulation", .open = sim_open, .exchange = sim_exchange,
        .close = sim_close, .reg_read = sim_reg_read, .priv = NULL,
    };
    return &backend;
}
