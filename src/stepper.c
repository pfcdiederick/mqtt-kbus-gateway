// 750-671 stepper controller driver, see stepper.h. Bit names as in the manual
// (750-671, 2.1.2.11.1.4.1 "Positioning Process Image").
#include "stepper.h"
#include "backend.h"
#include "gateway.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Output bytes
#define O_C0   0
#define O_VEL  2      // D0/D1 Velocity L/H
#define O_ACC  4      // D2/D3 Acceleration L/H
#define O_POS  6      // D4..D6 Target position L/M/H
#define O_C3   9
#define O_C2  10
#define O_C1  11
// Input bytes
#define I_S0   0
#define I_VEL  2      // D0/D1 Actual velocity L/H
#define I_POS  6      // D4..D6 Actual position L/M/H
#define I_S3   9
#define I_S2  10
#define I_S1  11

// C1 / S1
#define C1_ENABLE   0x01
#define C1_STOP2_N  0x02
#define C1_START    0x04
#define C1_M_POS    0x08
#define C1_M_PROG   0x10
#define C1_M_REF    0x20
#define C1_M_JOG    0x40
#define C1_M_MBX    0x80
#define C1_MODES    (C1_M_POS | C1_M_PROG | C1_M_REF | C1_M_JOG | C1_M_MBX)
#define S1_READY      0x01
#define S1_STOP_N_ACK 0x02
#define S1_START_ACK  0x04
#define S1_MODES      0xF8      // the mode _ACK bits, same positions as in C1
// C2 / S2
#define C2_ERROR_QUIT 0x80
#define S2_ON_TARGET  0x01
#define S2_BUSY       0x02
#define S2_STANDSTILL 0x04
#define S2_ON_SPEED   0x08
#define S2_DIRECTION  0x10
#define S2_REF_OK     0x20
#define S2_ERROR      0x80
// C3 / S3
#define C3_DIR_POS    0x04
#define C3_DIR_NEG    0x08
#define C3_RESET_QUIT 0x80
#define S3_INPUT1     0x01
#define S3_INPUT2     0x02
#define S3_RESET      0x80

#define VEL_MAX     25000
#define ACC_MAX     32767
#define POS_MIN     (-8388608L)
#define POS_MAX     8388607L
#define HANDSHAKE_MS 2000       // a handshake step must be acknowledged within this time
#define MODE_CHANGE_MS 30000    // leaving an active mode may wait for the drive to stop
#define STOP_MS     30000
#define STATE_MIN_MS 100        // position/velocity changes: state at most every 100 ms

typedef enum {
    PH_IDLE,
    PH_ENABLE,          // wait for Ready + Stop_N_ACK
    PH_DISABLE,         // wait for Ready = 0
    PH_STOP,            // wait for standstill and no mode
    PH_QUIT,            // Error_Quit / Reset_Quit held for a few ticks
    PH_LEAVE_MODE,      // other mode active: wait until it is gone
    PH_SELECT,          // mode bit set: wait for its _ACK
    PH_START_LOW,       // Start must be 0 (and Start_ACK gone) before the edge
    PH_START,           // Start set: wait for Start_ACK
    PH_RUN,             // wait for the result (On_Target / Reference_OK)
} phase_t;

typedef struct {
    uint8_t  out[STEPPER_PI_BYTES];
    uint8_t  written[STEPPER_PI_BYTES];
    int      have_written;
    phase_t  phase;
    int64_t  deadline;
    char     cmd[16];
    char     id[64];
    uint8_t  mode;          // C1 mode bit of the running command
    int32_t  target, vel, acc;
    uint8_t  dir;           // C3 direction bits (referencing)
    int      seen_busy, ticks;
    char     state[512];
    uint32_t flags;         // S1/S2/S3 of the last published state
    int64_t  next_state_ms;
    int      warned_size;
} stp_t;

static stp_t st[KBUS_MAX_MODULES];
static stepper_pub_fn g_pub;

static int32_t get24(const uint8_t *p)
{
    int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    return (v & 0x800000) ? v - 0x1000000 : v;
}

static void put16(uint8_t *p, int32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put24(uint8_t *p, int32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); }

static void respond(uint16_t pos, stp_t *s, const char *result, const char *err)
{
    if (!g_pub)
        return;
    char p[320];
    size_t b = (size_t)snprintf(p, sizeof(p), "{\"cmd\":\"%s\"", s->cmd);
    if (s->id[0])
        b += (size_t)snprintf(p + b, sizeof(p) - b, ",\"id\":\"%s\"", s->id);
    b += (size_t)snprintf(p + b, sizeof(p) - b, ",\"result\":\"%s\"", result);
    if (err)
        b += (size_t)snprintf(p + b, sizeof(p) - b, ",\"error\":\"%s\"", err);
    snprintf(p + b, sizeof(p) - b, "}");
    g_pub(pos, "stepper/response", p, 0);
    if (strcmp(result, "accepted") != 0)
        LOG_INFO("stepper %u: %s %s%s%s", pos, s->cmd, result, err ? ": " : "", err ? err : "");
}

static void finish(uint16_t pos, stp_t *s, const char *result, const char *err)
{
    respond(pos, s, result, err);
    s->phase = PH_IDLE;
    s->out[O_C1] &= (uint8_t)~C1_START;
    s->cmd[0] = s->id[0] = '\0';
}

// ---------------------------------------------------------------------------
// Minimal JSON: "key":"text" and "key":number in one flat object
// ---------------------------------------------------------------------------
static const char *json_value(const char *j, const char *key)
{
    char k[40];
    snprintf(k, sizeof(k), "\"%s\"", key);
    const char *p = strstr(j, k);
    if (!p)
        return NULL;
    p += strlen(k);
    while (isspace((unsigned char)*p)) ++p;
    if (*p != ':')
        return NULL;
    ++p;
    while (isspace((unsigned char)*p)) ++p;
    return p;
}

static int json_str(const char *j, const char *key, char *buf, size_t cap)
{
    const char *p = json_value(j, key);
    if (!p || *p != '"')
        return -1;
    ++p;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' || (unsigned char)*p < 0x20)
            return -1;                       // keep it simple: no escapes in ids / commands
        buf[n++] = *p++;
    }
    if (*p != '"')
        return -1;
    buf[n] = '\0';
    return 0;
}

static int json_num(const char *j, const char *key, long *v, int *present)
{
    const char *p = json_value(j, key);
    *present = p != NULL;
    if (!p)
        return 0;
    char *end = NULL;
    *v = strtol(p, &end, 10);
    if (end == p || (*end && *end != ',' && *end != '}' && !isspace((unsigned char)*end)))
        return -1;
    return 0;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
int stepper_command(size_t mi, uint16_t pos, const char *json, size_t len, char *err, size_t errlen)
{
    if (mi >= KBUS_MAX_MODULES)
        return snprintf(err, errlen, "no module"), -1;
    stp_t *s = &st[mi];
    char j[512], cmd[16] = "", id[64] = "";
    if (len >= sizeof(j))
        return snprintf(err, errlen, "command too long"), -1;
    memcpy(j, json, len);
    j[len] = '\0';
    if (json_str(j, "cmd", cmd, sizeof(cmd)) != 0)
        return snprintf(err, errlen, "expected JSON with \\\"cmd\\\", e.g. {\\\"cmd\\\":\\\"enable\\\"}"), -1;
    const char *idv = json_value(j, "id");
    if (idv && json_str(j, "id", id, sizeof(id)) != 0)
        return snprintf(err, errlen, "\\\"id\\\" must be a short string without escapes"), -1;

    uint8_t in[STEPPER_PI_BYTES];
    if (gateway_module_input(mi, in, sizeof(in)) != STEPPER_PI_BYTES)
        return snprintf(err, errlen, "module has no 12-byte process image"), -1;
    const uint8_t s1 = in[I_S1], s2 = in[I_S2];

    const int preempt = !strcmp(cmd, "stop") || !strcmp(cmd, "disable") || !strcmp(cmd, "quit") || !strcmp(cmd, "move");
    if (s->phase != PH_IDLE) {
        if (!preempt)
            return snprintf(err, errlen, "busy with \\\"%s\\\" (stop, disable, quit or move may interrupt it)", s->cmd), -1;
        finish(pos, s, "error", !strcmp(cmd, "move") ? "superseded by a new move" : "interrupted");
    }

    long v = 0;
    int present = 0;
    if (!strcmp(cmd, "enable")) {
        s->out[O_C1] |= C1_ENABLE | C1_STOP2_N;
        if (in[I_S3] & S3_RESET)
            s->out[O_C3] |= C3_RESET_QUIT;    // acknowledge the power-on reset of the module
        s->phase = PH_ENABLE;
        s->deadline = 0;
    } else if (!strcmp(cmd, "disable")) {
        if (!(s2 & S2_STANDSTILL) && (s1 & S1_READY))
            return snprintf(err, errlen, "drive is moving: send stop first (Enable 0 stops without a ramp)"), -1;
        s->out[O_C1] = 0;
        s->out[O_C3] &= (uint8_t)~(C3_DIR_POS | C3_DIR_NEG);
        s->phase = PH_DISABLE;
        s->deadline = 0;
    } else if (!strcmp(cmd, "stop")) {
        s->out[O_C1] &= (uint8_t)~(C1_MODES | C1_START);
        s->out[O_C3] &= (uint8_t)~(C3_DIR_POS | C3_DIR_NEG);
        s->phase = PH_STOP;
        s->deadline = 0;
    } else if (!strcmp(cmd, "quit")) {
        s->out[O_C2] |= C2_ERROR_QUIT;
        s->out[O_C3] |= C3_RESET_QUIT;
        s->phase = PH_QUIT;
        s->ticks = 0;
    } else if (!strcmp(cmd, "move") || !strcmp(cmd, "reference")) {
        if (!(s1 & S1_READY) || !(s1 & S1_STOP_N_ACK))
            return snprintf(err, errlen, "not ready: send enable first (and check Input1 / errors)"), -1;
        if (!strcmp(cmd, "move")) {
            if (json_num(j, "position", &v, &present) || !present || v < POS_MIN || v > POS_MAX)
                return snprintf(err, errlen, "\\\"position\\\" must be an integer %ld..%ld", POS_MIN, POS_MAX), -1;
            s->target = (int32_t)v;
            if (json_num(j, "velocity", &v, &present) || !present || v < 1 || v > VEL_MAX)
                return snprintf(err, errlen, "\\\"velocity\\\" must be 1..%d", VEL_MAX), -1;
            s->vel = (int32_t)v;
            if (json_num(j, "acceleration", &v, &present) || !present || v < 1 || v > ACC_MAX)
                return snprintf(err, errlen, "\\\"acceleration\\\" must be 1..%d", ACC_MAX), -1;
            s->acc = (int32_t)v;
            s->mode = C1_M_POS;
            s->dir = 0;
        } else {
            char d[8] = "";
            if (json_str(j, "direction", d, sizeof(d)) != 0 || (strcmp(d, "neg") && strcmp(d, "pos")))
                return snprintf(err, errlen, "\\\"direction\\\" must be \\\"neg\\\" or \\\"pos\\\""), -1;
            s->mode = C1_M_REF;
            s->dir = !strcmp(d, "neg") ? C3_DIR_NEG : C3_DIR_POS;
        }
        const uint8_t other = (uint8_t)(s1 & S1_MODES & ~s->mode);
        s->out[O_C1] &= (uint8_t)~((C1_MODES & ~s->mode) | C1_START);
        s->phase = other ? PH_LEAVE_MODE : PH_SELECT;
        s->deadline = 0;
    } else {
        return snprintf(err, errlen, "unknown command (enable, disable, quit, stop, move, reference)"), -1;
    }
    snprintf(s->cmd, sizeof(s->cmd), "%s", cmd);
    snprintf(s->id, sizeof(s->id), "%s", id);
    return 0;
}

void stepper_safe(size_t mi)
{
    if (mi >= KBUS_MAX_MODULES)
        return;
    stp_t *s = &st[mi];
    s->out[O_C1] &= (uint8_t)~(C1_MODES | C1_START);    // controlled stop, Enable stays
    s->out[O_C3] &= (uint8_t)~(C3_DIR_POS | C3_DIR_NEG);
    if (s->phase != PH_IDLE) {
        snprintf(s->cmd, sizeof(s->cmd), "%s", s->cmd[0] ? s->cmd : "safe");
        s->phase = PH_STOP;                   // finish() with an error on the next tick
        s->deadline = -1;
    }
    uint8_t cur[STEPPER_PI_BYTES];
    memcpy(cur, s->out, sizeof(cur));
    gateway_module_output(mi, cur, sizeof(cur));
    memcpy(s->written, cur, sizeof(cur));
    s->have_written = 1;
}

void stepper_reset_all(void)
{
    memset(st, 0, sizeof(st));
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
static const char *mode_name(uint8_t s1)
{
    return (s1 & C1_M_POS) ? "positioning" : (s1 & C1_M_REF) ? "reference" : (s1 & C1_M_JOG) ? "jog"
         : (s1 & C1_M_PROG) ? "program" : (s1 & C1_M_MBX) ? "mailbox" : "none";
}

static int timed_out(stp_t *s, int64_t now, int64_t ms)
{
    if (s->deadline == 0)
        s->deadline = now + ms;
    return now >= s->deadline;
}

static void advance(uint16_t pos, stp_t *s, const uint8_t *in, int64_t now)
{
    const uint8_t s1 = in[I_S1], s2 = in[I_S2], s3 = in[I_S3];
    if (s->deadline == -1) {                                    // watchdog stopped the command
        finish(pos, s, "error", "write watchdog: controlled stop");
        return;
    }
    switch (s->phase) {
    case PH_IDLE:
        break;
    case PH_ENABLE:
        if (!(s3 & S3_RESET))
            s->out[O_C3] &= (uint8_t)~C3_RESET_QUIT;
        if ((s1 & S1_READY) && (s1 & S1_STOP_N_ACK) && !(s3 & S3_RESET))
            finish(pos, s, "done", NULL);
        else if (timed_out(s, now, HANDSHAKE_MS))
            finish(pos, s, "error", (s2 & S2_ERROR) ? "module error: send quit"
                                   : !(s3 & S3_INPUT1) ? "Input1 (stop circuit) is open"
                                   : !(s1 & S1_READY) ? "no Ready" : "no Stop_N_ACK");
        break;
    case PH_DISABLE:
        if (!(s1 & S1_READY))
            finish(pos, s, "done", NULL);
        else if (timed_out(s, now, HANDSHAKE_MS))
            finish(pos, s, "error", "Ready stays on");
        break;
    case PH_STOP:
        if ((s2 & S2_STANDSTILL) && !(s1 & S1_MODES))
            finish(pos, s, "done", NULL);
        else if (timed_out(s, now, STOP_MS))
            finish(pos, s, "error", "no standstill");
        break;
    case PH_QUIT:
        if (++s->ticks >= 3) {                                  // held for >= 2 ticks: edge seen
            s->out[O_C2] &= (uint8_t)~C2_ERROR_QUIT;
            s->out[O_C3] &= (uint8_t)~C3_RESET_QUIT;
            finish(pos, s, (s2 & S2_ERROR) ? "error" : "done", (s2 & S2_ERROR) ? "error still present" : NULL);
        }
        break;
    case PH_LEAVE_MODE:
        if (!(s1 & S1_MODES & ~s->mode)) {
            s->phase = PH_SELECT;
            s->deadline = 0;
        } else if (timed_out(s, now, MODE_CHANGE_MS)) {
            finish(pos, s, "error", "the previous mode did not end");
        }
        break;
    case PH_SELECT:
        s->out[O_C1] |= s->mode;
        if (s1 & s->mode) {
            s->phase = PH_START_LOW;
            s->deadline = 0;
        } else if (timed_out(s, now, HANDSHAKE_MS)) {
            finish(pos, s, "error", "mode not acknowledged");
        }
        break;
    case PH_START_LOW:
        put16(&s->out[O_VEL], s->vel);
        put16(&s->out[O_ACC], s->acc);
        put24(&s->out[O_POS], s->target);
        s->out[O_C3] = (uint8_t)((s->out[O_C3] & ~(C3_DIR_POS | C3_DIR_NEG)) | s->dir);
        s->out[O_C1] &= (uint8_t)~C1_START;
        if (!(s1 & S1_START_ACK)) {
            s->phase = PH_START;
            s->deadline = 0;
        } else if (timed_out(s, now, HANDSHAKE_MS)) {
            finish(pos, s, "error", "Start_ACK stays on");
        }
        break;
    case PH_START:
        s->out[O_C1] |= C1_START;
        if (s1 & S1_START_ACK) {
            s->out[O_C1] &= (uint8_t)~C1_START;                 // Start may be canceled after Start_ACK
            respond(pos, s, "accepted", NULL);
            s->phase = PH_RUN;
            s->seen_busy = 0;
            s->ticks = 0;
            s->deadline = 0;
        } else if (s2 & S2_ERROR) {
            finish(pos, s, "error", "module error: send quit");
        } else if (timed_out(s, now, HANDSHAKE_MS)) {
            finish(pos, s, "error", "Start not acknowledged");
        }
        break;
    case PH_RUN: {
        ++s->ticks;
        if (s2 & S2_BUSY)
            s->seen_busy = 1;
        const int settled = !(s2 & S2_BUSY) && (s->seen_busy || s->ticks >= 3);
        const int ok = s->mode == C1_M_REF ? (s2 & S2_REF_OK) : (s2 & S2_ON_TARGET);
        if (s2 & S2_ERROR) {
            finish(pos, s, "error", "module error: send quit");
        } else if (!(s1 & s->mode)) {
            finish(pos, s, "error", "mode ended (stop, Input1 or limit switch)");
        } else if (settled && ok) {
            if (s->mode == C1_M_REF)
                s->out[O_C3] &= (uint8_t)~(C3_DIR_POS | C3_DIR_NEG);
            finish(pos, s, "done", NULL);
        } else if (settled && s->ticks > 25) {
            finish(pos, s, "error", s->mode == C1_M_REF ? "reference not found" : "stopped before the target");
        }
        break;
    }
    }
}

void stepper_tick(size_t mi, uint16_t pos, int64_t now, int full, stepper_pub_fn pub)
{
    if (mi >= KBUS_MAX_MODULES)
        return;
    stp_t *s = &st[mi];
    g_pub = pub;
    uint8_t in[STEPPER_PI_BYTES];
    if (gateway_module_input(mi, in, sizeof(in)) != STEPPER_PI_BYTES) {
        if (!s->warned_size++)
            LOG_WARN("stepper %u: process image is not 12 bytes: no driver, raw bytes only", pos);
        return;
    }
    advance(pos, s, in, now);
    if (!s->have_written || memcmp(s->out, s->written, sizeof(s->out)) != 0) {
        if (gateway_module_output(mi, s->out, sizeof(s->out)) == GW_OK) {
            memcpy(s->written, s->out, sizeof(s->out));
            s->have_written = 1;
        }
    }

    const uint8_t s1 = in[I_S1], s2 = in[I_S2], s3 = in[I_S3];
    const uint32_t flags = (uint32_t)s1 | ((uint32_t)s2 << 8) | ((uint32_t)s3 << 16) | ((uint32_t)(s->out[O_C1] & C1_ENABLE) << 24);
    char j[sizeof(s->state)];
    snprintf(j, sizeof(j),
             "{\"enabled\":%s,\"ready\":%s,\"stopAck\":%s,\"mode\":\"%s\",\"busy\":%s,\"onTarget\":%s,"
             "\"standstill\":%s,\"referenced\":%s,\"error\":%s,\"reset\":%s,\"input1\":%s,\"input2\":%s,"
             "\"position\":%ld,\"velocity\":%d,\"command\":%s%s%s}",
             (s->out[O_C1] & C1_ENABLE) ? "true" : "false", (s1 & S1_READY) ? "true" : "false",
             (s1 & S1_STOP_N_ACK) ? "true" : "false", mode_name(s1), (s2 & S2_BUSY) ? "true" : "false",
             (s2 & S2_ON_TARGET) ? "true" : "false", (s2 & S2_STANDSTILL) ? "true" : "false",
             (s2 & S2_REF_OK) ? "true" : "false", (s2 & S2_ERROR) ? "true" : "false",
             (s3 & S3_RESET) ? "true" : "false", (s3 & S3_INPUT1) ? "true" : "false",
             (s3 & S3_INPUT2) ? "true" : "false", (long)get24(&in[I_POS]),
             (int)(int16_t)((uint16_t)in[I_VEL] | ((uint16_t)in[I_VEL + 1] << 8)),
             s->cmd[0] ? "\"" : "", s->cmd[0] ? s->cmd : "null", s->cmd[0] ? "\"" : "");
    if (full || (strcmp(j, s->state) != 0 && (flags != s->flags || now >= s->next_state_ms))) {
        pub(pos, "stepper/state", j, 1);
        memcpy(s->state, j, sizeof(j));
        s->flags = flags;
        s->next_state_ms = now + STATE_MIN_MS;
    }
}
