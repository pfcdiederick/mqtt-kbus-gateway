// Register communication worker, see regcom.h.
//
// Thread model: the MQTT thread owns the channel map and calls regcom_layout() after
// every scan; this thread only uses its own copy (generation counter: results of reads
// that started before a new layout are dropped). The reads themselves go through
// gateway_reg_read(), which is serialised with bus rescans.
#include "regcom.h"
#include "gateway.h"
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint16_t n;              // AI channel number (1-based)
    uint16_t pos;            // module position = terminal of the register request
    uint16_t table;          // channel on the module - 1
} chan_t;

typedef struct {
    uint16_t pos, table, reg;
    char     id[REGCOM_ID_MAX];
} req_t;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_t       thread;
    int             started, stop;
    int             interval_ms;          // MQTT_AI_STATUS_MS, 0 = no status polling

    uint32_t gen;                         // +1 per layout
    chan_t   ch[GW_MAX_CHANNELS];
    size_t   nch;
    int64_t  due[GW_MAX_CHANNELS];        // next read of ch[i]
    uint8_t  warned[GW_MAX_CHANNELS];     // a failed read of ch[i] was logged
    uint8_t  mod_state[256];              // per position: 0 unknown, 1 register access ok, 2 none

    uint8_t  state[GW_MAX_CHANNELS];      // per AI channel number - 1
    uint16_t value[GW_MAX_CHANNELS];

    req_t    req[REGCOM_QUEUE];
    size_t   req_head, req_count;
    regcom_result_t res[REGCOM_QUEUE];
    size_t   res_head, res_count;
    size_t   outstanding;                 // queued + being read + answered, not yet popped
} rc;

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static const char *reason(int r)
{
    switch (r) {
    case GW_EFAILED:      return "read failed";
    case GW_EUNAVAILABLE: return "register access not available";
    case GW_EOFFLINE:     return "bus not running";
    default:              return "error";
    }
}

static void push_result(const req_t *q, int r, uint16_t v)
{
    regcom_result_t *o = &rc.res[(rc.res_head + rc.res_count) % REGCOM_QUEUE];
    rc.res_count++;                       // outstanding <= REGCOM_QUEUE: never overflows
    memset(o, 0, sizeof(*o));
    o->pos = q->pos;
    o->table = q->table;
    o->reg = q->reg;
    memcpy(o->id, q->id, sizeof(o->id));
    o->ok = r == GW_OK;
    o->value = v;
    o->error = o->ok ? NULL : reason(r);
}

// Status read of ch[i] finished (lock held, same generation).
static void status_done(size_t i, int r, uint16_t v)
{
    const chan_t *c = &rc.ch[i];
    uint8_t *ms = &rc.mod_state[c->pos & 0xff];
    if (r == GW_OK) {
        if (*ms != 1)
            LOG_INFO("module %u: status register %d of its analog inputs is read every %d ms",
                     c->pos, REGCOM_STATUS_REGISTER, rc.interval_ms);
        *ms = 1;
        rc.state[c->n - 1] = REGCOM_VALID;
        rc.value[c->n - 1] = v;
        if (rc.warned[i]) {
            LOG_INFO("AI%u: status register readable again", c->n);
            rc.warned[i] = 0;
        }
        return;
    }
    if (*ms == 0 || r == GW_EUNAVAILABLE) {
        // the first answer decides: no register access -> no status for this module
        *ms = 2;
        LOG_INFO("module %u: status register not readable (%s): no ai/<n>/status for its channels",
                 c->pos, reason(r));
        for (size_t k = 0; k < rc.nch; ++k)
            if (rc.ch[k].pos == c->pos)
                rc.state[rc.ch[k].n - 1] = REGCOM_UNSUPPORTED;
        return;
    }
    if (!rc.warned[i]) {                  // later failures: keep the last value, retry
        LOG_WARN("AI%u: reading status register %d failed (%s); last value kept",
                 c->n, REGCOM_STATUS_REGISTER, reason(r));
        rc.warned[i] = 1;
    }
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&rc.lock);
    while (!rc.stop) {
        // 1. requests first: someone is waiting for the answer
        if (rc.req_count) {
            req_t q = rc.req[rc.req_head];
            rc.req_head = (rc.req_head + 1) % REGCOM_QUEUE;
            rc.req_count--;
            pthread_mutex_unlock(&rc.lock);
            uint16_t v = 0, n = 0;
            int r = gateway_reg_read(q.pos, q.table, q.reg, 1, &v, &n);
            if (r == GW_OK && n < 1)
                r = GW_EFAILED;
            pthread_mutex_lock(&rc.lock);
            push_result(&q, r, v);
            continue;
        }

        // 2. status polling: the channel that is due first
        const int64_t now = now_ms();
        int64_t next = now + 1000;
        size_t pick = SIZE_MAX;
        if (rc.interval_ms > 0) {
            for (size_t i = 0; i < rc.nch; ++i) {
                if (rc.mod_state[rc.ch[i].pos & 0xff] == 2)
                    continue;
                if (pick == SIZE_MAX || rc.due[i] < rc.due[pick])
                    pick = i;
            }
        }
        if (pick != SIZE_MAX && rc.due[pick] <= now) {
            gw_stats_t st;
            pthread_mutex_unlock(&rc.lock);
            gateway_stats(&st);           // no reads while the bus is failing
            pthread_mutex_lock(&rc.lock);
            if (!st.healthy || rc.stop) {
                next = now + 1000;
            } else if (pick < rc.nch) {
                const chan_t c = rc.ch[pick];
                const uint32_t gen = rc.gen;
                rc.due[pick] = now + rc.interval_ms;
                pthread_mutex_unlock(&rc.lock);
                uint16_t v = 0, n = 0;
                int r = gateway_reg_read(c.pos, c.table, REGCOM_STATUS_REGISTER, 1, &v, &n);
                if (r == GW_OK && n < 1)
                    r = GW_EFAILED;
                pthread_mutex_lock(&rc.lock);
                if (gen == rc.gen)
                    status_done(pick, r, v);
                continue;
            }
        } else if (pick != SIZE_MAX) {
            next = rc.due[pick];
        }

        // 3. wait for the next due channel or a request
        if (rc.stop || rc.req_count)
            continue;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t wait = next - now_ms();
        if (wait < 1)
            wait = 1;
        ts.tv_sec += wait / 1000;
        ts.tv_nsec += (wait % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            ts.tv_sec++;
        }
        pthread_cond_timedwait(&rc.cond, &rc.lock, &ts);
    }
    pthread_mutex_unlock(&rc.lock);
    return NULL;
}

void regcom_start(const gw_config_t *cfg)
{
    memset(&rc, 0, sizeof(rc));
    rc.interval_ms = cfg->ai_status_ms;
    pthread_mutex_init(&rc.lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&rc.cond, &ca);
    pthread_condattr_destroy(&ca);

    // Normal priority, whatever the caller has: the reads must never compete with the
    // KBUS cycle or the MQTT thread.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    struct sched_param sp = { .sched_priority = 0 };
    pthread_attr_setschedparam(&attr, &sp);
    if (pthread_create(&rc.thread, &attr, worker, NULL) == 0) {
        rc.started = 1;
        if (rc.interval_ms > 0)
            LOG_INFO("register communication (read only): AI status register %d every %d ms per channel",
                     REGCOM_STATUS_REGISTER, rc.interval_ms);
        else
            LOG_INFO("register communication (read only): AI status off (MQTT_AI_STATUS_MS=0), reg/get only");
    } else {
        LOG_ERROR("could not start the register communication thread: no ai/<n>/status, no reg/get");
    }
    pthread_attr_destroy(&attr);
}

void regcom_stop(void)
{
    if (!rc.started)
        return;
    pthread_mutex_lock(&rc.lock);
    rc.stop = 1;
    pthread_cond_signal(&rc.cond);
    pthread_mutex_unlock(&rc.lock);
    pthread_join(rc.thread, NULL);       // a running read takes ~20 ms
    rc.started = 0;
}

void regcom_layout(void)
{
    size_t nmod = 0;
    const kbus_module_t *mods = gateway_modules(&nmod);
    const size_t n = gateway_channel_count(IO_AI);
    pthread_mutex_lock(&rc.lock);
    rc.gen++;
    rc.nch = 0;
    const int64_t now = now_ms();
    for (size_t c = 1; c <= n && rc.nch < GW_MAX_CHANNELS; ++c) {
        const io_channel_t *ch = gateway_channel(IO_AI, c);
        if (ch == NULL || ch->module_index >= nmod)
            continue;
        chan_t *e = &rc.ch[rc.nch];
        e->n = (uint16_t)c;
        e->pos = mods[ch->module_index].position;
        e->table = (uint16_t)(ch->module_channel - 1);
        rc.due[rc.nch] = now;
        rc.warned[rc.nch] = 0;
        rc.nch++;
    }
    memset(rc.mod_state, 0, sizeof(rc.mod_state));
    const uint8_t s = rc.started && rc.interval_ms > 0 ? REGCOM_PENDING : REGCOM_UNSUPPORTED;
    memset(rc.state, s, sizeof(rc.state));
    memset(rc.value, 0, sizeof(rc.value));
    pthread_cond_signal(&rc.cond);
    pthread_mutex_unlock(&rc.lock);
}

void regcom_status(uint8_t *state, uint16_t *value, size_t n)
{
    if (n > GW_MAX_CHANNELS)
        n = GW_MAX_CHANNELS;
    pthread_mutex_lock(&rc.lock);
    memcpy(state, rc.state, n);
    memcpy(value, rc.value, n * sizeof(value[0]));
    pthread_mutex_unlock(&rc.lock);
}

int regcom_request(uint16_t pos, uint16_t table, uint16_t reg, const char *id)
{
    if (!rc.started)
        return -2;
    pthread_mutex_lock(&rc.lock);
    if (rc.outstanding >= REGCOM_QUEUE) {
        pthread_mutex_unlock(&rc.lock);
        return -1;
    }
    req_t *q = &rc.req[(rc.req_head + rc.req_count) % REGCOM_QUEUE];
    q->pos = pos;
    q->table = table;
    q->reg = reg;
    snprintf(q->id, sizeof(q->id), "%s", id ? id : "");
    rc.req_count++;
    rc.outstanding++;
    pthread_cond_signal(&rc.cond);
    pthread_mutex_unlock(&rc.lock);
    return 0;
}

int regcom_pop_result(regcom_result_t *r)
{
    pthread_mutex_lock(&rc.lock);
    const int got = rc.res_count > 0;
    if (got) {
        *r = rc.res[rc.res_head];
        rc.res_head = (rc.res_head + 1) % REGCOM_QUEUE;
        rc.res_count--;
        rc.outstanding--;
    }
    pthread_mutex_unlock(&rc.lock);
    return got;
}

void regcom_drop_results(void)
{
    regcom_result_t r;
    while (regcom_pop_result(&r))
        ;
}
