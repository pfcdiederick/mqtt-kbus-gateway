// KBUS <-> MQTT, see mqtt_io.h. Everything runs in one thread (the main thread):
// broker connection, publishing, writes and bus rescans. The KBUS cycle runs in its
// own real-time thread (gateway.c); the two share only the locked process images.
#include "mqtt_io.h"
#include "gateway.h"
#include "log.h"
#include "mqtt_client.h"

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
} m;

static int is_digital(io_kind_t k) { return k == IO_DI || k == IO_DO; }

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
    size_t cap = 64 + count * 160;
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
        m.layout = 0;
        m.full = 1;
    }

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
        if (m.full || n != m.raw_len[mi] || memcmp(raw, m.raw_pub[mi], n) != 0) {
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
    const int r = gateway_module_output(mi, (const uint8_t *)p, len);
    if (r == GW_ERANGE) {
        char e[96];
        snprintf(e, sizeof(e), "expected exactly %u bytes (binary)", (mods[mi].out_size_bits + 7) / 8);
        reply_error(t, e);
    } else if (r != GW_OK) {
        reply_error(t, "module has no output data");
    } else if (m.cfg->mqtt_log >= 2) {
        LOG_INFO("write module %lu: %zu bytes (%s)", pos, len, t);
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
static int start_session(void)
{
    pub("status", "online", 1);
    // Collect the retained channel topics of an earlier run (removed when stale).
    const char *f[IO_KIND_COUNT];
    char fb[IO_KIND_COUNT][160];
    for (int k = 0; k < IO_KIND_COUNT; ++k) {
        snprintf(fb[k], sizeof(fb[k]), "%s/%s/+", m.cfg->topic_prefix, cls[k]);
        f[k] = fb[k];
    }
    if (mqtt_subscribe(&m.cli, f, IO_KIND_COUNT) != 0)
        return -1;
    m.state = ST_CLEANUP;
    m.cleanup_until = 0;
    return 0;
}

static int finish_cleanup(void)
{
    const char *f[IO_KIND_COUNT];
    char fb[IO_KIND_COUNT][160];
    for (int k = 0; k < IO_KIND_COUNT; ++k) {
        snprintf(fb[k], sizeof(fb[k]), "%s/%s/+", m.cfg->topic_prefix, cls[k]);
        f[k] = fb[k];
    }
    if (mqtt_unsubscribe(&m.cli, f, IO_KIND_COUNT) != 0)
        return -1;
    if (m.cfg->write_enable) {
        char s0[160], s1[160], s2[160];
        snprintf(s0, sizeof(s0), "%s/do/+/set", m.cfg->topic_prefix);
        snprintf(s1, sizeof(s1), "%s/ao/+/set", m.cfg->topic_prefix);
        snprintf(s2, sizeof(s2), "%s/bus/+/set", m.cfg->topic_prefix);
        char s3[160];
        snprintf(s3, sizeof(s3), "%s/module/+/out/set", m.cfg->topic_prefix);
        const char *sf[4] = { s0, s1, s2, s3 };
        if (mqtt_subscribe(&m.cli, sf, 4) != 0)
            return -1;
    }
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
        }

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
