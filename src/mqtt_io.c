// KBUS <-> MQTT, see mqtt_io.h. Everything runs in one thread (the main thread):
// broker connection, publishing, writes and bus rescans. The KBUS cycle runs in its
// own real-time thread (gateway.c); the two share only the locked process images.
#include "mqtt_io.h"
#include "gateway.h"
#include "log.h"
#include "modules.h"
#include "mqtt_client.h"
#include "serial.h"
#include "stepper.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define RECONNECT_MIN_MS   1000
#define RECONNECT_MAX_MS   30000
#define CLEANUP_MS         300     // after SUBACK: time to collect the old retained channel topics

static const char *const cls[IO_KIND_COUNT] = { "di", "do", "ai", "ao" };

typedef enum { ST_OFFLINE, ST_CLEANUP, ST_RUN } state_t;

static struct {
    const gw_config_t *cfg;
    mqtt_client_t      cli;
    state_t            state;
    int64_t            cleanup_until;

    uint16_t cur[IO_KIND_COUNT][GW_MAX_CHANNELS];      // snapshot of this tick
    uint16_t pub[IO_KIND_COUNT][GW_MAX_CHANNELS];      // value last published per channel
    size_t   topics[IO_KIND_COUNT];                     // channel topics that exist on the broker

    int      full;                                      // publish everything at the next tick
    int      layout;                                    // modules / channel set changed
    uint32_t version_seen;
    int      healthy_pub;                               // -1 = not published yet
    int      cycle_pub;
    int64_t  next_stats_ms;
    int64_t  next_republish_ms;
    int      warned_retained_set;

    // raw input bytes of complex modules (e.g. 750-495): <prefix>/module/<pos>/in
    uint8_t  raw_mod[KBUS_MAX_MODULES];                 // 1 = module has no channels but input data
    uint8_t  raw_pub[KBUS_MAX_MODULES][GW_MODULE_RAW_MAX];
    uint8_t  raw_len[KBUS_MAX_MODULES];                 // bytes last published (0 = topic not published)
    uint16_t raw_pos[KBUS_MAX_MODULES];                 // position the topic was published for
    int64_t  raw_next_ms[KBUS_MAX_MODULES];             // MQTT_RAW_MIN_MS: earliest next publish

    // serial interface COM1: <prefix>/serial/com1/... (values last published)
    serial_snapshot_t ser_pub;
    int      ser_valid;                                 // ser_pub holds published values
    int64_t  next_ser_stats_ms;

    // functional values of module descriptors (modules.h), per AI channel (index = n-1):
    // <prefix>/module/<pos>/<function>/<module channel>
    uint16_t f_pub[GW_MAX_CHANNELS];                    // raw word last published
    uint8_t  f_valid[GW_MAX_CHANNELS];                  // topic published
    uint16_t f_pos[GW_MAX_CHANNELS], f_ch[GW_MAX_CHANNELS];
    const char *f_func[GW_MAX_CHANNELS];

    uint8_t  stp_pub[256];                              // positions with a published stepper/state

    // write watchdog (MQTT_WATCHDOG_S)
    int      wd_armed;                                  // a write or watchdog/set was received
    int      wd_expired;                                // outputs were switched to 0
    int64_t  wd_last_ms;
    int      wd_pub;                                    // -1 = not published, else wd_expired
} m;

static int is_digital(io_kind_t k) { return k == IO_DI || k == IO_DO; }

// Module with a functional driver in the gateway (750-671 stepper): its raw outputs
// belong to the driver.
static int is_stepper(const kbus_module_t *mod)
{
    return mod->type == 671 && mod->in_size_bits == 8 * STEPPER_PI_BYTES && mod->out_size_bits == 8 * STEPPER_PI_BYTES;
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------
static void topic(char *buf, size_t len, const char *suffix)
{
    snprintf(buf, len, "%s/%s", m.cfg->topic_prefix, suffix);
}

static void pub(const char *suffix, const char *payload, int retain)
{
    char t[256];
    topic(t, sizeof(t), suffix);
    mqtt_publish_str(&m.cli, t, payload, retain);
}

static void pub_value(io_kind_t k, size_t n, uint16_t v)
{
    char s[32], p[8];
    snprintf(s, sizeof(s), "%s/%zu", cls[k], n);
    if (is_digital(k))
        snprintf(p, sizeof(p), "%s", v ? "true" : "false");
    else
        snprintf(p, sizeof(p), "%u", (unsigned)v);
    pub(s, p, m.cfg->retain);
}

static void pub_array(io_kind_t k, size_t count)
{
    char *buf = malloc(count * 7 + 4);
    if (buf == NULL)
        return;
    size_t b = 0;
    buf[b++] = '[';
    for (size_t i = 0; i < count; ++i) {
        if (i)
            buf[b++] = ',';
        if (is_digital(k))
            b += (size_t)sprintf(buf + b, "%s", m.cur[k][i] ? "true" : "false");
        else
            b += (size_t)sprintf(buf + b, "%u", (unsigned)m.cur[k][i]);
    }
    buf[b++] = ']';
    buf[b] = '\0';
    char s[16];
    snprintf(s, sizeof(s), "%s/all", cls[k]);
    pub(s, buf, m.cfg->retain);
    free(buf);
}

// Delete a retained message (empty retained payload).
static void delete_topic(const char *full_topic)
{
    mqtt_publish(&m.cli, full_topic, NULL, 0, 1);
}

// <prefix>/modules: [{"position":1,"name":"75x-455","typeCode":455,"channels":{"ai":[1,2,3,4]}}, ...]
static void pub_modules(void)
{
    size_t count = 0;
    const kbus_module_t *mods = gateway_modules(&count);
    size_t cap = 64 + count * 256;
    for (int k = 0; k < IO_KIND_COUNT; ++k)
        cap += gateway_channel_count((io_kind_t)k) * 6;
    char *buf = malloc(cap);
    if (buf == NULL)
        return;
    size_t b = 0;
    buf[b++] = '[';
    for (size_t mi = 0; mi < count; ++mi) {
        char name[48];
        gateway_module_name(&mods[mi], name, sizeof(name));
        b += (size_t)snprintf(buf + b, cap - b, "%s{\"position\":%u,\"name\":\"%s\",\"typeCode\":%u,\"channels\":{",
                              mi ? "," : "", mods[mi].position, name, mods[mi].type);
        int first_class = 1;
        for (int k = 0; k < IO_KIND_COUNT; ++k) {
            int first = 1;
            const size_t n = gateway_channel_count((io_kind_t)k);
            for (size_t c = 1; c <= n; ++c) {
                const io_channel_t *ch = gateway_channel((io_kind_t)k, c);
                if (ch->module_index != mi)
                    continue;
                if (first)
                    b += (size_t)snprintf(buf + b, cap - b, "%s\"%s\":[", first_class ? "" : ",", cls[k]);
                b += (size_t)snprintf(buf + b, cap - b, "%s%zu", first ? "" : ",", c);
                first = first_class = 0;
            }
            if (!first)
                buf[b++] = ']';
        }
        b += (size_t)snprintf(buf + b, cap - b, "}");
        const module_desc_t *d = module_desc(mods[mi].type);
        if (d && gateway_module_has_channels(mi))
            b += (size_t)snprintf(buf + b, cap - b, ",\"functions\":{\"%s\":{\"unit\":\"%s\",\"resolution\":%g}}",
                                  d->function, d->unit, d->resolution);
        if (is_stepper(&mods[mi]))
            b += (size_t)snprintf(buf + b, cap - b, ",\"driver\":\"stepper\"");
        if (!gateway_module_has_channels(mi) && mods[mi].in_size_bits)
            b += (size_t)snprintf(buf + b, cap - b, ",\"raw\":{\"in\":%u,\"out\":%u}",
                                  (mods[mi].in_size_bits + 7) / 8, (mods[mi].out_size_bits + 7) / 8);
        b += (size_t)snprintf(buf + b, cap - b, "}");
    }
    buf[b++] = ']';
    buf[b] = '\0';
    pub("modules", buf, 1);
    free(buf);
}

static void pub_stats(const gw_stats_t *st)
{
    char p[512];
    snprintf(p, sizeof(p),
             "{\"version\":\"%s\",\"backend\":\"%s\",\"healthy\":%s,\"cycleTime\":%d,\"realtime\":%s,"
             "\"cycles\":%llu,\"errors\":%llu,\"overruns\":%llu,\"cycleDuration\":%u,"
             "\"maxCycleDuration\":%u,\"maxWakeupDelay\":%u}",
             GW_VERSION, st->backend ? st->backend : "", st->healthy ? "true" : "false", st->cycle_ms,
             st->realtime ? "true" : "false", (unsigned long long)st->cycles,
             (unsigned long long)st->errors, (unsigned long long)st->overruns, st->last_cycle_us,
             st->max_cycle_us, st->max_late_us);
    pub("bus/statistics", p, 1);
}

// COM1: settings and state when changed, statistics when changed (at most 1x per second),
// every frame received since the last tick as one <prefix>/serial/com1/rx message.
static void publish_serial(int64_t now)
{
    serial_snapshot_t s;
    serial_snapshot(&s);
    const serial_snapshot_t *o = &m.ser_pub;
    const int all = m.full || !m.ser_valid;
    char v[256];
    if (all || s.open != o->open)
        pub("serial/com1/open", s.open ? "true" : "false", 1);
    if (all || s.baud != o->baud) {
        snprintf(v, sizeof(v), "%u", (unsigned)s.baud);
        pub("serial/com1/baudrate", v, 1);
    }
    if (all || strcmp(s.format, o->format) != 0)
        pub("serial/com1/format", s.format, 1);
    if (all || s.frame_gap_ms != o->frame_gap_ms) {
        snprintf(v, sizeof(v), "%u", (unsigned)s.frame_gap_ms);
        pub("serial/com1/framegap", v, 1);
    }
    const int stats_changed = s.tx_count != o->tx_count || s.rx_count != o->rx_count ||
                              s.frame_count != o->frame_count || s.dropped != o->dropped || s.errors != o->errors;
    serial_snapshot_t keep = s;
    if (all || (stats_changed && now >= m.next_ser_stats_ms)) {
        snprintf(v, sizeof(v), "{\"txBytes\":%llu,\"rxBytes\":%llu,\"frames\":%llu,\"dropped\":%llu,\"errors\":%llu}",
                 (unsigned long long)s.tx_count, (unsigned long long)s.rx_count,
                 (unsigned long long)s.frame_count, (unsigned long long)s.dropped,
                 (unsigned long long)s.errors);
        pub("serial/com1/statistics", v, 1);
        m.next_ser_stats_ms = now + 1000;
    } else {                                            // not published yet: compare again next tick
        keep.tx_count = o->tx_count; keep.rx_count = o->rx_count; keep.frame_count = o->frame_count;
        keep.dropped = o->dropped; keep.errors = o->errors;
    }
    m.ser_pub = keep;
    m.ser_valid = 1;

    uint8_t frame[SERIAL_BUF];
    size_t n;
    char t[256];
    topic(t, sizeof(t), "serial/com1/rx");
    while ((n = serial_pop_frame(frame)) > 0) {
        mqtt_publish(&m.cli, t, frame, n, 0);
        if (m.cfg->mqtt_log >= 2)
            LOG_INFO("COM1 received %zu bytes", n);
    }
}

// Decimals that show the resolution of a descriptor (0.1 -> 1, 0.01 -> 2, 1 -> 0).
static int decimals(double res)
{
    int d = 0;
    for (double r = res; r < 0.999 && d < 6; r *= 10)
        ++d;
    return d;
}

static void func_topic(char *t, size_t len, uint16_t pos, const char *func, uint16_t ch)
{
    snprintf(t, len, "%s/module/%u/%s/%u", m.cfg->topic_prefix, pos, func, ch);
}

// Module descriptors: the AI channels of a module with a descriptor (750-450) are
// also published scaled, as <prefix>/module/<pos>/<function>/<n>. layout = 1: the
// channel map changed; topics that no longer apply are deleted.
static void publish_functions(int layout, int band)
{
    size_t nmod = 0;
    const kbus_module_t *mods = gateway_modules(&nmod);
    const size_t n = gateway_channel_count(IO_AI);
    char t[256], v[32];
    for (size_t c = 1; c <= GW_MAX_CHANNELS; ++c) {
        const module_desc_t *d = NULL;
        uint16_t pos = 0, mch = 0;
        if (c <= n) {
            const io_channel_t *ch = gateway_channel(IO_AI, c);
            if (ch && ch->mapped && ch->module_index < nmod) {
                d = module_desc(mods[ch->module_index].type);
                pos = mods[ch->module_index].position;
                mch = ch->module_channel;
            }
        }
        const size_t i = c - 1;
        if (layout && m.f_valid[i] && (!d || m.f_pos[i] != pos || m.f_ch[i] != mch || m.f_func[i] != d->function)) {
            func_topic(t, sizeof(t), m.f_pos[i], m.f_func[i], m.f_ch[i]);
            delete_topic(t);
            m.f_valid[i] = 0;
        }
        if (!d)
            continue;
        const uint16_t raw = m.cur[IO_AI][i];
        const int diff = d->is_signed ? abs((int)(int16_t)raw - (int)(int16_t)m.f_pub[i]) : abs((int)raw - (int)m.f_pub[i]);
        if (m.full || !m.f_valid[i] || diff >= band) {
            func_topic(t, sizeof(t), pos, d->function, mch);
            snprintf(v, sizeof(v), "%.*f", decimals(d->resolution), module_value(d, raw));
            pub(t + strlen(m.cfg->topic_prefix) + 1, v, m.cfg->retain);
            m.f_pub[i] = raw;
            m.f_valid[i] = 1;
            m.f_pos[i] = pos;
            m.f_ch[i] = mch;
            m.f_func[i] = d->function;
        }
    }
}

// One tick: compare the process image with what was published, publish the differences.
static void publish_state(void)
{
    const gw_config_t *cfg = m.cfg;
    const int64_t now = mqtt_now_ms();
    size_t count[IO_KIND_COUNT];
    for (int k = 0; k < IO_KIND_COUNT; ++k)
        count[k] = gateway_channel_count((io_kind_t)k);

    if (cfg->republish_s > 0 && now >= m.next_republish_ms) {
        m.full = 1;
        m.next_republish_ms = now + (int64_t)cfg->republish_s * 1000;
    }

    if (m.layout) {
        // Channels that no longer exist: remove their retained topics.
        char t[256];
        for (int k = 0; k < IO_KIND_COUNT; ++k) {
            for (size_t n = count[k] + 1; n <= m.topics[k]; ++n) {
                snprintf(t, sizeof(t), "%s/%s/%zu", cfg->topic_prefix, cls[k], n);
                delete_topic(t);
            }
            m.topics[k] = count[k];
        }
        // complex modules: which ones get a raw topic; remove raw topics that no longer apply
        size_t nmod = 0;
        const kbus_module_t *mods = gateway_modules(&nmod);
        for (size_t mi = 0; mi < KBUS_MAX_MODULES; ++mi) {
            const int raw = mi < nmod && mods[mi].in_size_bits && !gateway_module_has_channels(mi);
            if (m.raw_len[mi] && (!raw || m.raw_pos[mi] != mods[mi].position)) {
                snprintf(t, sizeof(t), "%s/module/%u/in", cfg->topic_prefix, m.raw_pos[mi]);
                delete_topic(t);
                m.raw_len[mi] = 0;
            }
            m.raw_mod[mi] = (uint8_t)raw;
        }
        char v[16];
        snprintf(v, sizeof(v), "%u", gateway_config_version());
        pub("bus/configversion", v, 1);
        pub_modules();
        stepper_reset_all();                            // module positions may have changed
        for (size_t p = 1; p < sizeof(m.stp_pub); ++p) {  // steppers that are gone: delete their state
            if (!m.stp_pub[p])
                continue;
            size_t k;
            for (k = 0; k < nmod && !(mods[k].position == p && is_stepper(&mods[k])); ++k)
                ;
            if (k == nmod) {
                snprintf(t, sizeof(t), "%s/module/%zu/stepper/state", cfg->topic_prefix, p);
                delete_topic(t);
                m.stp_pub[p] = 0;
            }
        }
        m.layout = 0;
        m.full = 1;
    }

    const int layout_changed = m.full;                 // m.layout set m.full above
    gateway_snapshot(m.cur);
    const int band = cfg->analog_deadband > 0 ? cfg->analog_deadband : 1;
    for (int k = 0; k < IO_KIND_COUNT; ++k) {
        int any = 0;
        for (size_t i = 0; i < count[k]; ++i) {
            const uint16_t v = m.cur[k][i];
            const int diff = abs((int)v - (int)m.pub[k][i]);
            if (m.full || (is_digital((io_kind_t)k) ? diff != 0 : diff >= band)) {
                pub_value((io_kind_t)k, i + 1, v);
                m.pub[k][i] = v;
                any = 1;
            }
        }
        if (cfg->arrays && (any || m.full))
            pub_array((io_kind_t)k, count[k]);
    }

    publish_functions(layout_changed, band);

    // raw input bytes of complex modules, one message per module, only when they changed
    size_t nmod = 0;
    const kbus_module_t *mods = gateway_modules(&nmod);
    for (size_t mi = 0; mi < nmod; ++mi) {
        if (!m.raw_mod[mi])
            continue;
        uint8_t raw[GW_MODULE_RAW_MAX];
        const size_t n = gateway_module_input(mi, raw, sizeof(raw));
        if (n == 0)
            continue;
        // Rate limit (MQTT_RAW_MIN_MS): a change inside the interval is not lost: the bytes stay
        // different from raw_pub, so the latest ones are published at the first tick after it.
        const int changed = n != m.raw_len[mi] || memcmp(raw, m.raw_pub[mi], n) != 0;
        if (m.full || (changed && now >= m.raw_next_ms[mi])) {
            m.raw_next_ms[mi] = now + cfg->raw_min_ms;
            char t[256];
            snprintf(t, sizeof(t), "%s/module/%u/in", cfg->topic_prefix, mods[mi].position);
            mqtt_publish(&m.cli, t, raw, n, cfg->retain);
            memcpy(m.raw_pub[mi], raw, n);
            m.raw_len[mi] = (uint8_t)n;
            m.raw_pos[mi] = mods[mi].position;
        }
    }

    gw_stats_t st;
    gateway_stats(&st);
    if (m.full || st.healthy != m.healthy_pub) {
        pub("bus/healthy", st.healthy ? "true" : "false", 1);
        m.healthy_pub = st.healthy;
    }
    if (m.full || st.cycle_ms != m.cycle_pub) {
        char v[16];
        snprintf(v, sizeof(v), "%d", st.cycle_ms);
        pub("bus/cycletime", v, 1);
        m.cycle_pub = st.cycle_ms;
    }
    if (cfg->stats_s > 0 && (m.full || now >= m.next_stats_ms)) {
        pub_stats(&st);
        m.next_stats_ms = now + (int64_t)cfg->stats_s * 1000;
    }
    if (serial_enabled())
        publish_serial(now);
    if (cfg->watchdog_s > 0 && (m.full || m.wd_pub != m.wd_expired)) {
        pub("watchdog", m.wd_expired ? "expired" : "ok", 1);
        m.wd_pub = m.wd_expired;
    }
    m.full = 0;
}

// ---------------------------------------------------------------------------
// Incoming messages
// ---------------------------------------------------------------------------
static void reply_error(const char *t, const char *err)
{
    LOG_WARN("rejected %s: %s", t, err);
    char p[512];
    snprintf(p, sizeof(p), "{\"topic\":\"%s\",\"error\":\"%s\"}", t, err);
    pub("error", p, 0);
}

// Trim spaces and one pair of double quotes into buf. 0 = ok.
static int token(const char *s, size_t len, char *buf, size_t blen)
{
    while (len > 0 && isspace((unsigned char)*s)) { ++s; --len; }
    while (len > 0 && isspace((unsigned char)s[len - 1])) --len;
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { ++s; len -= 2; }
    if (len == 0 || len >= blen)
        return -1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    return 0;
}

// Value of a digital (0/1) or analog (0..65535) channel, or -1 for null (arrays only).
// Returns 0 = ok, -1 = invalid.
static int parse_value(io_kind_t k, const char *s, size_t len, int allow_null, int32_t *out)
{
    char t[24];
    if (token(s, len, t, sizeof(t)) != 0)
        return -1;
    if (allow_null && strcmp(t, "null") == 0) {
        *out = -1;
        return 0;
    }
    if (is_digital(k)) {
        if (!strcasecmp(t, "true") || !strcasecmp(t, "on") || !strcmp(t, "1")) { *out = 1; return 0; }
        if (!strcasecmp(t, "false") || !strcasecmp(t, "off") || !strcmp(t, "0")) { *out = 0; return 0; }
        return -1;
    }
    char *end = NULL;
    errno = 0;
    const long v = strtol(t, &end, 10);
    if (errno || end == t || *end != '\0' || v < 0 || v > 65535)
        return -1;
    *out = (int32_t)v;
    return 0;
}

// ---------------------------------------------------------------------------
// Write watchdog (MQTT_WATCHDOG_S): the controlling side writes or sends
// <prefix>/watchdog/set at least every N s; otherwise all outputs go to 0.
// ---------------------------------------------------------------------------
static void watchdog_kick(void)
{
    m.wd_last_ms = mqtt_now_ms();
    m.wd_armed = 1;
    if (m.wd_expired) {
        m.wd_expired = 0;
        LOG_INFO("write watchdog: commands again, outputs are written again");
    }
}

// Safe state: every DO/AO 0 and the raw output bytes of complex modules 0.
static void safe_state(void)
{
    static int32_t zeros32[GW_MAX_CHANNELS];
    static uint8_t zeros8[256];
    const int32_t *v[IO_KIND_COUNT] = { 0 };
    size_t n[IO_KIND_COUNT] = { 0 };
    v[IO_DO] = v[IO_AO] = zeros32;
    n[IO_DO] = gateway_channel_count(IO_DO);
    n[IO_AO] = gateway_channel_count(IO_AO);
    gateway_write_multi(v, n, NULL);
    size_t nmod = 0;
    const kbus_module_t *mods = gateway_modules(&nmod);
    for (size_t mi = 0; mi < nmod; ++mi) {
        const size_t len = (mods[mi].out_size_bits + 7) / 8;
        if (is_stepper(&mods[mi]))
            stepper_safe(mi);                           // controlled stop, not Enable 0
        else if (!gateway_module_has_channels(mi) && len && len <= sizeof(zeros8))
            gateway_module_output(mi, zeros8, len);
    }
}

static void watchdog_check(int64_t now)
{
    if (m.cfg->watchdog_s <= 0 || !m.wd_armed || m.wd_expired)
        return;
    if (now - m.wd_last_ms < (int64_t)m.cfg->watchdog_s * 1000)
        return;
    safe_state();
    m.wd_expired = 1;
    LOG_WARN("write watchdog: no write and no watchdog/set for %d s: all outputs switched to 0",
             m.cfg->watchdog_s);
}

static void write_outputs(const char *t, io_kind_t k, const int32_t *vals, size_t n)
{
    const int32_t *v[IO_KIND_COUNT] = { 0 };
    size_t nn[IO_KIND_COUNT] = { 0 };
    v[k] = vals;
    nn[k] = n;
    int changed = 0;
    const int r = gateway_write_multi(v, nn, &changed);
    if (r == GW_ENOENT) {
        char e[96];
        snprintf(e, sizeof(e), "there are only %zu %s channels", gateway_channel_count(k), io_kind_name(k));
        reply_error(t, e);
        return;
    }
    if (r != GW_OK) {
        reply_error(t, "value out of range");
        return;
    }
    watchdog_kick();
    if (changed && m.cfg->mqtt_log >= 1) {
        char line[160];
        size_t b = 0;
        for (size_t i = 0; i < n && b < sizeof(line) - 24; ++i) {
            if (vals[i] < 0)
                continue;
            b += (size_t)snprintf(line + b, sizeof(line) - b, "%s%s%zu=%s", b ? " " : "", io_kind_name(k), i + 1,
                                  is_digital(k) ? (vals[i] ? "true" : "false") : "");
            if (!is_digital(k))
                b += (size_t)snprintf(line + b, sizeof(line) - b, "%d", vals[i]);
        }
        LOG_INFO("write %s (%s)", line, t);
    }
}

static int32_t g_vals[GW_MAX_CHANNELS];

static void on_output(const char *t, io_kind_t k, const char *which, const char *p, size_t len)
{
    if (strcmp(which, "all") == 0) {
        // JSON array: [true,false,null,...] / [100,null,2000]
        const char *s = p, *e = p + len;
        while (s < e && isspace((unsigned char)*s)) ++s;
        while (e > s && isspace((unsigned char)e[-1])) --e;
        if (e - s < 2 || *s != '[' || e[-1] != ']') {
            reply_error(t, "expected a JSON array, e.g. [true,false,null]");
            return;
        }
        ++s; --e;
        size_t n = 0;
        while (s < e) {
            const char *c = memchr(s, ',', (size_t)(e - s));
            const char *end = c ? c : e;
            if (n >= GW_MAX_CHANNELS || parse_value(k, s, (size_t)(end - s), 1, &g_vals[n]) != 0) {
                reply_error(t, n >= GW_MAX_CHANNELS ? "too many values" :
                               is_digital(k) ? "elements must be true, false or null" :
                                               "elements must be integers 0..65535 or null");
                return;
            }
            ++n;
            s = c ? c + 1 : e;
        }
        if (n > 0)
            write_outputs(t, k, g_vals, n);
        return;
    }
    char *end = NULL;
    const unsigned long ch = strtoul(which, &end, 10);
    if (end == which || *end != '\0' || ch == 0 || ch > GW_MAX_CHANNELS) {
        reply_error(t, "unknown channel");
        return;
    }
    int32_t v;
    if (parse_value(k, p, len, 0, &v) != 0) {
        reply_error(t, is_digital(k) ? "expected true or false" : "expected an integer 0..65535");
        return;
    }
    for (size_t i = 0; i + 1 < ch; ++i)
        g_vals[i] = -1;
    g_vals[ch - 1] = v;
    write_outputs(t, k, g_vals, ch);
}

static void on_bus(const char *t, const char *what, const char *p, size_t len)
{
    int32_t v;
    if (strcmp(what, "cycletime") == 0) {
        char tok[16];
        char *end = NULL;
        long ms = token(p, len, tok, sizeof(tok)) == 0 ? strtol(tok, &end, 10) : 0;
        if (end == NULL || *end != '\0' || ms < GW_CYCLE_MS_MIN || ms > GW_CYCLE_MS_MAX) {
            reply_error(t, "expected a cycle time of 1..40 ms");
            return;
        }
        const int old = gateway_set_cycle_ms((int)ms);
        if (old != ms) {
            LOG_INFO("KBUS cycle time %d -> %ld ms (%s)", old, ms, t);
            config_store_cycle(m.cfg, (int)ms);
        }
    } else if (strcmp(what, "rescan") == 0 || strcmp(what, "resetstatistics") == 0) {
        if (parse_value(IO_DO, p, len, 0, &v) != 0) {
            reply_error(t, "expected true");
            return;
        }
        if (!v)
            return;
        if (strcmp(what, "rescan") == 0) {
            LOG_INFO("KBUS rescan requested (%s)", t);
            gateway_request_rescan();
        } else {
            gateway_reset_stats();
            m.next_stats_ms = 0;                           // publish the reset statistics now
        }
    } else {
        reply_error(t, "unknown bus command (cycletime, rescan, resetstatistics)");
    }
}

// <prefix>/module/<pos>/out/set: raw output bytes of a complex module (e.g. written by the
// WAGO function block of a 750-495 in CODESYS). Applied in one piece, next bus cycle.
static void on_module_out(const char *t, const char *pos_s, const char *p, size_t len)
{
    char *end = NULL;
    const unsigned long pos = strtoul(pos_s, &end, 10);
    size_t nmod = 0, mi;
    const kbus_module_t *mods = gateway_modules(&nmod);
    for (mi = 0; mi < nmod && !(end != pos_s && *end == '\0' && mods[mi].position == pos); ++mi)
        ;
    if (mi == nmod) {
        reply_error(t, "no module at this position");
        return;
    }
    if (gateway_module_has_channels(mi)) {
        reply_error(t, "module has channels: use do/ao instead");
        return;
    }
    if (is_stepper(&mods[mi])) {
        reply_error(t, "module has a driver: use module/<pos>/stepper/cmd/set");
        return;
    }
    const int r = gateway_module_output(mi, (const uint8_t *)p, len);
    if (r == GW_ERANGE) {
        char e[96];
        snprintf(e, sizeof(e), "expected exactly %u bytes (binary)", (mods[mi].out_size_bits + 7) / 8);
        reply_error(t, e);
    } else if (r != GW_OK) {
        reply_error(t, "module has no output data");
    } else {
        watchdog_kick();
        if (m.cfg->mqtt_log >= 2)
            LOG_INFO("write module %lu: %zu bytes (%s)", pos, len, t);
    }
}

// A retained module topic of an earlier run (rest after "module/"): stale when the module
// at that position does not have it now (another node after a restart).
static int module_topic_stale(const char *rest)
{
    char *end = NULL;
    const unsigned long pos = strtoul(rest, &end, 10);
    if (end == rest || *end != '/')
        return 0;                                       // not ours: leave it
    const char *what = end + 1;
    size_t nmod = 0, mi;
    const kbus_module_t *mods = gateway_modules(&nmod);
    for (mi = 0; mi < nmod && mods[mi].position != pos; ++mi)
        ;
    if (mi == nmod)
        return 1;                                       // no module at this position now
    const kbus_module_t *mod = &mods[mi];
    if (strcmp(what, "in") == 0)
        return gateway_module_has_channels(mi) || !mod->in_size_bits;
    if (strcmp(what, "stepper/state") == 0 || strcmp(what, "stepper/response") == 0)
        return !is_stepper(mod);
    const module_desc_t *d = module_desc(mod->type);
    const size_t fl = d ? strlen(d->function) : 0;
    if (d && strncmp(what, d->function, fl) == 0 && what[fl] == '/') {
        const unsigned long n = strtoul(what + fl + 1, &end, 10);
        size_t count = 0;                               // the module's AI channels
        for (size_t c = 1; c <= gateway_channel_count(IO_AI); ++c)
            if (gateway_channel(IO_AI, c)->module_index == mi)
                ++count;
        return *end != '\0' || n == 0 || n > count;
    }
    return strstr(what, "/set") == NULL;                // other function topics of this position: stale
}

// ---------------------------------------------------------------------------
// 750-671 stepper drivers (stepper.c)
// ---------------------------------------------------------------------------
static void stepper_pub(uint16_t pos, const char *what, const char *payload, int retain)
{
    if (!mqtt_connected(&m.cli) || m.state != ST_RUN)
        return;
    char t[96];
    snprintf(t, sizeof(t), "module/%u/%s", pos, what);
    pub(t, payload, retain);
    if (retain && pos < sizeof(m.stp_pub))
        m.stp_pub[pos] = 1;
}

// Every tick, also without a broker connection: the handshakes go on.
static void steppers_tick(int64_t now)
{
    size_t nmod = 0;
    const kbus_module_t *mods = gateway_modules(&nmod);
    const int full = m.full && mqtt_connected(&m.cli) && m.state == ST_RUN;
    for (size_t mi = 0; mi < nmod; ++mi)
        if (is_stepper(&mods[mi]))
            stepper_tick(mi, mods[mi].position, now, full, stepper_pub);
}

// <prefix>/module/<pos>/stepper/cmd/set
static void on_stepper(const char *t, const char *pos_s, const char *p, size_t len)
{
    char *end = NULL;
    const unsigned long pos = strtoul(pos_s, &end, 10);
    size_t nmod = 0, mi;
    const kbus_module_t *mods = gateway_modules(&nmod);
    for (mi = 0; mi < nmod && !(end != pos_s && *end == '/' && mods[mi].position == pos); ++mi)
        ;
    if (mi == nmod || !is_stepper(&mods[mi])) {
        reply_error(t, "no 750-671 stepper at this position");
        return;
    }
    char err[200];
    if (stepper_command(mi, (uint16_t)pos, p, len, err, sizeof(err)) != 0) {
        reply_error(t, err);
        return;
    }
    watchdog_kick();
    if (m.cfg->mqtt_log >= 1)
        LOG_INFO("stepper %lu: %.*s (%s)", pos, (int)(len > 160 ? 160 : len), p, t);
}

// <prefix>/serial/com1/...: during the cleanup the retained topics of an earlier run
// (removed when COM1 is off now); afterwards the .../set commands.
static void on_serial(const char *t, const char *sub, const char *p, size_t len, int retained)
{
    if (m.state == ST_CLEANUP) {
        if (retained && len > 0 && !serial_enabled() && strncmp(sub, "com1/", 5) == 0 && !strchr(sub + 5, '/')) {
            LOG_INFO("removing old retained topic %s (COM1 off)", t);
            delete_topic(t);
        }
        return;
    }
    char what[16];
    if (sscanf(sub, "com1/%15[^/]", what) != 1 || strlen(sub) != 5 + strlen(what) + 4 ||
        strcmp(sub + 5 + strlen(what), "/set") != 0)
        return;
    if (retained) {
        if (!m.warned_retained_set++)
            LOG_WARN("ignoring retained command %s: publish .../set messages WITHOUT the retain flag "
                     "(delete it with: mosquitto_pub -r -n -t '%s')", t, t);
        return;
    }
    if (!serial_enabled()) {
        reply_error(t, "COM1 is off (SERIAL_DEVICE is empty)");
        return;
    }
    if (strcmp(what, "tx") == 0) {
        if (len == 0 || len > SERIAL_BUF) {
            reply_error(t, "expected 1..512 bytes (binary)");
            return;
        }
        if (serial_write((const uint8_t *)p, len) != 0) {
            serial_snapshot_t s;
            serial_snapshot(&s);
            reply_error(t, s.open ? "COM1 write failed" : "COM1 is not open");
            return;
        }
        if (m.cfg->mqtt_log >= 2)
            LOG_INFO("COM1 sent %zu bytes (%s)", len, t);
        return;
    }
    serial_snapshot_t cur;
    serial_snapshot(&cur);
    char tok[16];
    const int ok_tok = token(p, len, tok, sizeof(tok)) == 0;
    char *end = NULL;
    const long v = ok_tok ? strtol(tok, &end, 10) : 0;
    const int num_ok = ok_tok && end != tok && *end == '\0';
    if (strcmp(what, "baudrate") == 0) {
        if (!num_ok || serial_valid_baud(v) != 0) {
            reply_error(t, "expected a baud rate: 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400");
            return;
        }
        if ((uint32_t)v != cur.baud) {
            serial_configure((uint32_t)v, NULL, 0);
            config_store_serial(m.cfg, (int)v, NULL, 0);
            LOG_INFO("COM1 baud rate %u -> %ld (%s)", (unsigned)cur.baud, v, t);
        }
    } else if (strcmp(what, "format") == 0) {
        for (char *c = tok; ok_tok && *c; ++c)
            *c = (char)toupper((unsigned char)*c);
        if (!ok_tok || serial_valid_format(tok, strlen(tok)) != 0) {
            reply_error(t, "expected a format like 8E1: data bits 5..8, parity N/E/O, stop bits 1/2");
            return;
        }
        if (strcmp(tok, cur.format) != 0) {
            serial_configure(0, tok, 0);
            config_store_serial(m.cfg, 0, tok, 0);
            LOG_INFO("COM1 format %s -> %s (%s)", cur.format, tok, t);
        }
    } else if (strcmp(what, "framegap") == 0) {
        if (!num_ok || v < 1 || v > 1000) {
            reply_error(t, "expected a frame gap of 1..1000 ms");
            return;
        }
        if ((uint32_t)v != cur.frame_gap_ms) {
            serial_configure(0, NULL, (uint32_t)v);
            config_store_serial(m.cfg, 0, NULL, (int)v);
            LOG_INFO("COM1 frame gap %u -> %ld ms (%s)", (unsigned)cur.frame_gap_ms, v, t);
        }
    } else {
        reply_error(t, "unknown COM1 command (tx, baudrate, format, framegap)");
    }
}

static void on_message(void *ctx, const char *t, const char *p, size_t len, int retained)
{
    (void)ctx;
    const size_t pl = strlen(m.cfg->topic_prefix);
    if (strncmp(t, m.cfg->topic_prefix, pl) != 0 || t[pl] != '/')
        return;
    const char *rest = t + pl + 1;
    if (m.cfg->mqtt_log >= 2)
        LOG_INFO("message %s%s = %.*s", t, retained ? " (retained)" : "", (int)(len > 200 ? 200 : len), p);

    if (m.state == ST_CLEANUP && strncmp(rest, "module/", 7) == 0) {
        if (retained && len > 0 && module_topic_stale(rest + 7)) {
            LOG_INFO("removing old retained topic %s", t);
            delete_topic(t);
        }
        return;
    }
    if (strncmp(rest, "module/", 7) == 0) {
        const size_t rl = strlen(rest);
        if (rl > 23 && strcmp(rest + rl - 16, "/stepper/cmd/set") == 0) {          // module/<pos>/stepper/cmd/set
            if (m.state != ST_RUN)
                return;
            if (retained) {
                if (!m.warned_retained_set++)
                    LOG_WARN("ignoring retained command %s: publish .../set messages WITHOUT the retain flag "
                             "(delete it with: mosquitto_pub -r -n -t '%s')", t, t);
                return;
            }
            on_stepper(t, rest + 7, p, len);
            return;
        }
    }
    if (strcmp(rest, "watchdog/set") == 0) {          // heartbeat of the controlling side
        if (m.state == ST_RUN && !retained)
            watchdog_kick();
        return;
    }
    if (strncmp(rest, "serial/", 7) == 0) {         // before the generic parse: baudrate/set is too long for it
        on_serial(t, rest + 7, p, len, retained);
        return;
    }

    char a[16], b[24], c[8];
    a[0] = b[0] = c[0] = '\0';
    const int parts = sscanf(rest, "%15[^/]/%23[^/]/%7s", a, b, c);
    io_kind_t k;
    const int is_io = parts >= 2 && io_kind_parse(a, &k) == 0;

    if (m.state == ST_CLEANUP) {
        // Retained channel topics from an earlier run: remove those of channels that do not exist now.
        if (!retained || len == 0 || !is_io || parts != 2)
            return;
        char *end = NULL;
        const unsigned long n = strtoul(b, &end, 10);
        const int stale = strcmp(b, "all") == 0 ? !m.cfg->arrays
                        : (end == b || *end != '\0' || n == 0 || n > gateway_channel_count(k));
        if (stale) {
            LOG_INFO("removing old retained topic %s", t);
            delete_topic(t);
        }
        return;
    }

    const int raw_out = parts == 3 && strcmp(a, "module") == 0 && strcmp(c, "out/set") == 0;
    if (parts != 3 || (strcmp(c, "set") != 0 && !raw_out))
        return;
    if (retained) {
        // A retained command would be applied again at every reconnect: ignore it.
        if (!m.warned_retained_set++)
            LOG_WARN("ignoring retained command %s: publish .../set messages WITHOUT the retain flag "
                     "(delete it with: mosquitto_pub -r -n -t '%s')", t, t);
        return;
    }
    if (strcmp(a, "bus") == 0) {
        on_bus(t, b, p, len);
        return;
    }
    if (raw_out) {
        on_module_out(t, b, p, len);
        return;
    }
    if (!is_io || !io_kind_writable(k)) {
        reply_error(t, "not writable (only do and ao)");
        return;
    }
    on_output(t, k, b, p, len);
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------
// The retained topics of an earlier run that are checked after connecting: the channel
// topics (removed when the channel does not exist now) and the COM1 topics (removed when
// COM1 is off now).
#define CLEANUP_FILTERS (IO_KIND_COUNT + 3)
static void cleanup_filters(const char **f, char fb[][160])
{
    for (int k = 0; k < IO_KIND_COUNT; ++k) {
        snprintf(fb[k], 160, "%s/%s/+", m.cfg->topic_prefix, cls[k]);
        f[k] = fb[k];
    }
    snprintf(fb[IO_KIND_COUNT], 160, "%s/serial/com1/+", m.cfg->topic_prefix);
    f[IO_KIND_COUNT] = fb[IO_KIND_COUNT];
    snprintf(fb[IO_KIND_COUNT + 1], 160, "%s/module/+/in", m.cfg->topic_prefix);         // raw inputs
    f[IO_KIND_COUNT + 1] = fb[IO_KIND_COUNT + 1];
    snprintf(fb[IO_KIND_COUNT + 2], 160, "%s/module/+/+/+", m.cfg->topic_prefix);        // temperature/<n>, stepper/state
    f[IO_KIND_COUNT + 2] = fb[IO_KIND_COUNT + 2];
}

static int start_session(void)
{
    pub("status", "online", 1);
    const char *f[CLEANUP_FILTERS];
    char fb[CLEANUP_FILTERS][160];
    cleanup_filters(f, fb);
    if (mqtt_subscribe(&m.cli, f, CLEANUP_FILTERS) != 0)
        return -1;
    m.state = ST_CLEANUP;
    m.cleanup_until = 0;
    return 0;
}

static int finish_cleanup(void)
{
    const char *f[CLEANUP_FILTERS];
    char fb[CLEANUP_FILTERS][160];
    cleanup_filters(f, fb);
    if (mqtt_unsubscribe(&m.cli, f, CLEANUP_FILTERS) != 0)
        return -1;
    if (m.cfg->write_enable) {
        char s0[160], s1[160], s2[160];
        snprintf(s0, sizeof(s0), "%s/do/+/set", m.cfg->topic_prefix);
        snprintf(s1, sizeof(s1), "%s/ao/+/set", m.cfg->topic_prefix);
        snprintf(s2, sizeof(s2), "%s/bus/+/set", m.cfg->topic_prefix);
        char s3[160];
        snprintf(s3, sizeof(s3), "%s/module/+/out/set", m.cfg->topic_prefix);
        char s4[160], s5[160], s6[160];
        snprintf(s6, sizeof(s6), "%s/module/+/stepper/cmd/set", m.cfg->topic_prefix);
        snprintf(s4, sizeof(s4), "%s/serial/com1/+/set", m.cfg->topic_prefix);
        snprintf(s5, sizeof(s5), "%s/watchdog/set", m.cfg->topic_prefix);
        const char *sf[7] = { s0, s1, s2, s3, s6, 0, 0 };
        size_t nf = 5;
        if (serial_enabled())
            sf[nf++] = s4;
        if (m.cfg->watchdog_s > 0)
            sf[nf++] = s5;
        if (mqtt_subscribe(&m.cli, sf, nf) != 0)
            return -1;
    }
    // Frames received while there was no broker connection are old: not published.
    uint8_t frame[SERIAL_BUF];
    size_t old = 0;
    while (serial_pop_frame(frame) > 0)
        ++old;
    if (old)
        LOG_INFO("COM1: %zu frames received while offline discarded", old);
    m.ser_valid = 0;
    m.wd_pub = -1;
    m.state = ST_RUN;
    m.layout = 1;
    m.full = 1;
    m.healthy_pub = -1;
    m.next_stats_ms = 0;
    return 0;
}

int mqtt_io_run(const gw_config_t *cfg, volatile sig_atomic_t *stop)
{
    memset(&m, 0, sizeof(m));
    m.cfg = cfg;
    mqtt_init(&m.cli, on_message, NULL);
    m.version_seen = gateway_config_version();

    if (cfg->mqtt_priority > 0) {
        struct sched_param sp = { .sched_priority = cfg->mqtt_priority };
        const int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (r == 0)
            LOG_INFO("MQTT thread: SCHED_FIFO priority %d", cfg->mqtt_priority);
        else
            LOG_WARN("MQTT thread: SCHED_FIFO priority %d not permitted (%s)", cfg->mqtt_priority, strerror(r));
    }

    char will[256];
    topic(will, sizeof(will), "status");
    const mqtt_opts_t opts = {
        .host = cfg->mqtt_host, .port = cfg->mqtt_port, .client_id = cfg->mqtt_client_id,
        .username = cfg->mqtt_username, .password = cfg->mqtt_password, .keepalive_s = cfg->mqtt_keepalive_s,
        .will_topic = will, .will_payload = "offline", .will_retain = 1, .timeout_ms = 3000,
    };

    int64_t next_try = 0, backoff = RECONNECT_MIN_MS, next_tick = mqtt_now_ms();
    char last_err[sizeof(m.cli.error)] = "";
    while (!*stop) {
        int64_t now = mqtt_now_ms();
        if (!mqtt_connected(&m.cli) && now >= next_try) {
            if (mqtt_connect(&m.cli, &opts) == 0 && start_session() == 0) {
                LOG_INFO("connected to MQTT broker %s:%d as %s, topics %s/#", cfg->mqtt_host, cfg->mqtt_port,
                         cfg->mqtt_client_id, cfg->topic_prefix);
                backoff = RECONNECT_MIN_MS;
                last_err[0] = '\0';
            } else {
                if (strcmp(last_err, m.cli.error) != 0)    // log each new reason once
                    LOG_ERROR("MQTT broker %s:%d: %s (retrying)", cfg->mqtt_host, cfg->mqtt_port, m.cli.error);
                snprintf(last_err, sizeof(last_err), "%s", m.cli.error);
                m.state = ST_OFFLINE;
                next_try = now + backoff;
                backoff = backoff * 2 > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : backoff * 2;
            }
        }

        now = mqtt_now_ms();
        const int wait = next_tick > now ? (int)(next_tick - now) : 0;
        if (mqtt_connected(&m.cli)) {
            if (mqtt_poll(&m.cli, wait) != 0) {
                LOG_ERROR("MQTT connection lost: %s (reconnecting)", m.cli.error);
                snprintf(last_err, sizeof(last_err), "%s", m.cli.error);
                m.state = ST_OFFLINE;
                next_try = mqtt_now_ms() + RECONNECT_MIN_MS;
            } else if (m.cli.error[0]) {
                LOG_WARN("MQTT: %s", m.cli.error);
            }
        } else if (wait > 0) {
            poll(NULL, 0, wait);
        }

        now = mqtt_now_ms();
        if (now < next_tick)
            continue;
        next_tick += cfg->publish_ms;
        if (next_tick <= now)
            next_tick = now + cfg->publish_ms;             // skip missed ticks

        // Bus rescans run here: this thread is the only one that reads the channel map.
        if (gateway_rescan_due())
            gateway_rescan();
        const uint32_t ver = gateway_config_version();
        if (ver != m.version_seen) {
            m.version_seen = ver;
            m.layout = 1;
            stepper_reset_all();                       // before the next stepper tick
        }

        watchdog_check(now);                           // also while the broker is away
        steppers_tick(now);
        if (!mqtt_connected(&m.cli))
            continue;
        if (m.state == ST_CLEANUP) {
            if (m.cli.acks_pending == 0 && m.cleanup_until == 0)
                m.cleanup_until = now + CLEANUP_MS;
            if (m.cleanup_until != 0 && now >= m.cleanup_until && finish_cleanup() != 0)
                continue;
        }
        if (m.state == ST_RUN)
            publish_state();
        if (mqtt_flush(&m.cli) != 0)
            LOG_ERROR("MQTT connection lost: %s (reconnecting)", m.cli.error);
    }

    if (mqtt_connected(&m.cli)) {
        pub("status", "offline", 1);
        mqtt_disconnect(&m.cli);
    }
    return 0;
}
