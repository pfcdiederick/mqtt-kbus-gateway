// Minimal MQTT 3.1.1 client, see mqtt_client.h. Specification: OASIS MQTT 3.1.1.
#include "mqtt_client.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    CONNECT = 1, CONNACK = 2, PUBLISH = 3, PUBACK = 4, PUBREC = 5, PUBREL = 6, PUBCOMP = 7,
    SUBSCRIBE = 8, SUBACK = 9, UNSUBSCRIBE = 10, UNSUBACK = 11, PINGREQ = 12, PINGRESP = 13,
    DISCONNECT = 14
};

int64_t mqtt_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int fail(mqtt_client_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail(mqtt_client_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, sizeof(c->error), fmt, ap);
    va_end(ap);
    mqtt_close(c);
    return -1;
}

void mqtt_init(mqtt_client_t *c, mqtt_msg_cb on_msg, void *ctx)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->on_msg = on_msg;
    c->ctx = ctx;
    c->next_id = 1;
}

int mqtt_connected(const mqtt_client_t *c) { return c->fd >= 0; }

void mqtt_close(mqtt_client_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->rx_len = c->tx_len = 0;
    c->ping_pending = 0;
    c->acks_pending = 0;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
static int send_all(mqtt_client_t *c, const uint8_t *p, size_t n)
{
    while (n > 0) {
        const ssize_t w = send(c->fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return fail(c, "send: %s", errno == EAGAIN ? "time-out (broker does not read)" : strerror(errno));
        }
        p += w;
        n -= (size_t)w;
    }
    c->last_tx_ms = mqtt_now_ms();
    return 0;
}

int mqtt_flush(mqtt_client_t *c)
{
    if (c->fd < 0)
        return -1;
    if (c->tx_len == 0)
        return 0;
    const size_t n = c->tx_len;
    c->tx_len = 0;
    return send_all(c, c->tx, n);
}

// Append to the send buffer; flushes first when it does not fit, sends directly when
// it never fits.
static int put(mqtt_client_t *c, const void *p, size_t n)
{
    if (c->fd < 0)
        return -1;
    if (c->tx_len + n > sizeof(c->tx) && mqtt_flush(c) != 0)
        return -1;
    if (n > sizeof(c->tx))
        return send_all(c, p, n);
    memcpy(c->tx + c->tx_len, p, n);
    c->tx_len += n;
    return 0;
}

// Fixed header: type/flags byte + "remaining length" (1-4 bytes). Returns its size.
static size_t fixed_header(uint8_t *h, uint8_t type_flags, size_t remaining)
{
    size_t i = 0;
    h[i++] = type_flags;
    do {
        uint8_t b = remaining % 128;
        remaining /= 128;
        if (remaining > 0)
            b |= 0x80;
        h[i++] = b;
    } while (remaining > 0 && i < 5);
    return i;
}

static size_t put_str(uint8_t *p, const char *s, size_t n)
{
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)(n & 0xff);
    memcpy(p + 2, s, n);
    return n + 2;
}

int mqtt_publish(mqtt_client_t *c, const char *topic, const void *payload, size_t len, int retain)
{
    const size_t tl = strlen(topic);
    if (tl == 0 || tl > 65535 || len > 256u * 1024 * 1024)
        return -1;
    uint8_t h[5 + 2];
    size_t hn = fixed_header(h, (uint8_t)(PUBLISH << 4 | (retain ? 1 : 0)), 2 + tl + len);
    h[hn++] = (uint8_t)(tl >> 8);
    h[hn++] = (uint8_t)(tl & 0xff);
    if (put(c, h, hn) != 0 || put(c, topic, tl) != 0)
        return -1;
    return len > 0 ? put(c, payload, len) : 0;
}

int mqtt_publish_str(mqtt_client_t *c, const char *topic, const char *payload, int retain)
{
    return mqtt_publish(c, topic, payload, payload ? strlen(payload) : 0, retain);
}

static int sub_unsub(mqtt_client_t *c, int type, const char *const *filters, size_t n)
{
    uint8_t body[4096];
    size_t b = 0;
    const uint16_t id = c->next_id++;
    if (c->next_id == 0)
        c->next_id = 1;
    body[b++] = (uint8_t)(id >> 8);
    body[b++] = (uint8_t)(id & 0xff);
    for (size_t i = 0; i < n; ++i) {
        const size_t fl = strlen(filters[i]);
        if (b + fl + 3 > sizeof(body))
            return fail(c, "too many topic filters in one request");
        b += put_str(body + b, filters[i], fl);
        if (type == SUBSCRIBE)
            body[b++] = 0;                                 // requested QoS 0
    }
    uint8_t h[5];
    const size_t hn = fixed_header(h, (uint8_t)(type << 4 | 0x2), b);
    if (put(c, h, hn) != 0 || put(c, body, b) != 0)
        return -1;
    c->acks_pending++;
    return mqtt_flush(c);
}

int mqtt_subscribe(mqtt_client_t *c, const char *const *filters, size_t n)
{
    return sub_unsub(c, SUBSCRIBE, filters, n);
}

int mqtt_unsubscribe(mqtt_client_t *c, const char *const *filters, size_t n)
{
    return sub_unsub(c, UNSUBSCRIBE, filters, n);
}

static int send_simple(mqtt_client_t *c, uint8_t type_flags, int with_id, uint16_t id)
{
    uint8_t p[4] = { type_flags, (uint8_t)(with_id ? 2 : 0), (uint8_t)(id >> 8), (uint8_t)(id & 0xff) };
    if (put(c, p, with_id ? 4 : 2) != 0)
        return -1;
    return mqtt_flush(c);
}

void mqtt_disconnect(mqtt_client_t *c)
{
    if (c->fd >= 0 && mqtt_flush(c) == 0)
        send_simple(c, DISCONNECT << 4, 0, 0);
    mqtt_close(c);
}

// ---------------------------------------------------------------------------
// Connecting
// ---------------------------------------------------------------------------
static int tcp_connect(mqtt_client_t *c, const char *host, int port, int timeout_ms)
{
    char ps[8];
    snprintf(ps, sizeof(ps), "%d", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL;
    const int g = getaddrinfo(host, ps, &hints, &res);
    if (g != 0) {
        snprintf(c->error, sizeof(c->error), "%s: %s", host, gai_strerror(g));
        return -1;
    }
    int fd = -1;
    snprintf(c->error, sizeof(c->error), "%s:%d: no address", host, port);
    for (struct addrinfo *a = res; a != NULL; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0)
            continue;
        const int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r != 0 && errno == EINPROGRESS) {
            struct pollfd pf = { .fd = fd, .events = POLLOUT, .revents = 0 };
            r = poll(&pf, 1, timeout_ms);
            if (r == 1) {
                int err = 0;
                socklen_t el = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                errno = err;
                r = err == 0 ? 0 : -1;
            } else {
                errno = r == 0 ? ETIMEDOUT : errno;
                r = -1;
            }
        }
        if (r == 0) {
            fcntl(fd, F_SETFL, fl);                        // back to blocking
            break;
        }
        snprintf(c->error, sizeof(c->error), "%s:%d: %s", host, port, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };     // a broker that stops reading
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    c->fd = fd;
    return 0;
}

static const char *connack_reason(int rc)
{
    switch (rc) {
    case 1: return "unacceptable protocol version";
    case 2: return "client id rejected";
    case 3: return "server unavailable";
    case 4: return "bad user name or password";
    case 5: return "not authorized";
    default: return "refused";
    }
}

static int read_packet(mqtt_client_t *c, int timeout_ms, uint8_t *type, const uint8_t **body, size_t *blen);

int mqtt_connect(mqtt_client_t *c, const mqtt_opts_t *o)
{
    mqtt_close(c);
    c->error[0] = '\0';
    if (tcp_connect(c, o->host, o->port, o->timeout_ms) != 0)
        return -1;

    const int user = o->username && *o->username;
    const int pass = user && o->password && *o->password;
    const int will = o->will_topic && *o->will_topic;
    const size_t idl = strlen(o->client_id);
    const size_t wtl = will ? strlen(o->will_topic) : 0;
    const size_t wpl = will && o->will_payload ? strlen(o->will_payload) : 0;
    const size_t ul = user ? strlen(o->username) : 0;
    const size_t pl = pass ? strlen(o->password) : 0;
    if (idl > 512 || wtl > 512 || wpl > 512 || ul > 512 || pl > 512)
        return fail(c, "client id, user, password or will too long");

    uint8_t body[3000];
    size_t b = 0;
    b += put_str(body + b, "MQTT", 4);
    body[b++] = 4;                                         // protocol level 3.1.1
    body[b++] = (uint8_t)(0x02                             // clean session
                          | (will ? 0x04 : 0) | (will && o->will_retain ? 0x20 : 0)
                          | (pass ? 0x40 : 0) | (user ? 0x80 : 0));
    body[b++] = (uint8_t)(o->keepalive_s >> 8);
    body[b++] = (uint8_t)(o->keepalive_s & 0xff);
    b += put_str(body + b, o->client_id, idl);
    if (will) {
        b += put_str(body + b, o->will_topic, wtl);
        b += put_str(body + b, o->will_payload ? o->will_payload : "", wpl);
    }
    if (user)
        b += put_str(body + b, o->username, ul);
    if (pass)
        b += put_str(body + b, o->password, pl);
    uint8_t h[5];
    const size_t hn = fixed_header(h, CONNECT << 4, b);
    if (put(c, h, hn) != 0 || put(c, body, b) != 0 || mqtt_flush(c) != 0)
        return -1;

    uint8_t type;
    const uint8_t *p;
    size_t n;
    const int r = read_packet(c, o->timeout_ms, &type, &p, &n);
    if (r <= 0)
        return r < 0 ? -1 : fail(c, "no CONNACK within %d ms", o->timeout_ms);
    if (type != CONNACK || n < 2)
        return fail(c, "unexpected answer to CONNECT (packet type %u)", type);
    if (p[1] != 0)
        return fail(c, "broker refused the connection: %s (code %u)", connack_reason(p[1]), p[1]);
    c->rx_len = 0;                                         // CONNACK consumed (nothing else arrives before it)
    c->keepalive_ms = o->keepalive_s * 1000;
    c->last_rx_ms = c->last_tx_ms = mqtt_now_ms();
    return 0;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------
// Length of the first complete packet in rx (0 = incomplete, -1 = invalid / too large).
static long complete_packet(mqtt_client_t *c, size_t *hdr)
{
    size_t mult = 1, rem = 0, i = 1;
    for (;;) {
        if (i >= c->rx_len)
            return 0;
        const uint8_t d = c->rx[i++];
        rem += (size_t)(d & 0x7f) * mult;
        if (!(d & 0x80))
            break;
        mult *= 128;
        if (i > 4)
            return -1;
    }
    if (rem + i > MQTT_RX_MAX)
        return -1;
    *hdr = i;
    return c->rx_len >= i + rem ? (long)(i + rem) : 0;
}

// Read until one complete packet is in rx. 1 = packet (type, body), 0 = time-out, -1 = lost.
// The packet stays in rx until consume() is called.
static int read_packet(mqtt_client_t *c, int timeout_ms, uint8_t *type, const uint8_t **body, size_t *blen)
{
    const int64_t until = mqtt_now_ms() + timeout_ms;
    for (;;) {
        size_t hdr = 0;
        const long len = complete_packet(c, &hdr);
        if (len < 0)
            return fail(c, "packet from the broker larger than %d bytes", MQTT_RX_MAX);
        if (len > 0) {
            *type = c->rx[0] >> 4;
            *body = c->rx + hdr;
            *blen = (size_t)len - hdr;
            return 1;
        }
        const int64_t left = until - mqtt_now_ms();
        if (left <= 0)
            return 0;
        struct pollfd pf = { .fd = c->fd, .events = POLLIN, .revents = 0 };
        const int pr = poll(&pf, 1, (int)left);
        if (pr < 0 && errno != EINTR)
            return fail(c, "poll: %s", strerror(errno));
        if (pr <= 0)
            continue;
        const ssize_t r = recv(c->fd, c->rx + c->rx_len, sizeof(c->rx) - c->rx_len, 0);
        if (r == 0)
            return fail(c, "connection closed by the broker");
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return fail(c, "recv: %s", strerror(errno));
        }
        c->rx_len += (size_t)r;
        c->last_rx_ms = mqtt_now_ms();
    }
}

static void consume(mqtt_client_t *c, size_t n)
{
    memmove(c->rx, c->rx + n, c->rx_len - n);
    c->rx_len -= n;
}

static int handle_packet(mqtt_client_t *c, uint8_t first, const uint8_t *p, size_t n)
{
    const uint8_t type = first >> 4;
    switch (type) {
    case PUBLISH: {
        const int qos = (first >> 1) & 3;
        if (n < 2)
            return fail(c, "malformed PUBLISH");
        const size_t tl = (size_t)p[0] << 8 | p[1];
        size_t off = 2 + tl;
        if (off + (qos ? 2 : 0) > n)
            return fail(c, "malformed PUBLISH");
        uint16_t id = 0;
        if (qos) {
            id = (uint16_t)(p[off] << 8 | p[off + 1]);
            off += 2;
        }
        char topic[1024];
        if (tl < sizeof(topic) && c->on_msg) {
            memcpy(topic, p + 2, tl);
            topic[tl] = '\0';
            // NUL-terminated copy of the payload for the parsers (rx has spare bytes)
            char *payload = (char *)(p + off);
            const size_t plen = n - off;
            const char saved = payload[plen];
            payload[plen] = '\0';
            c->on_msg(c->ctx, topic, payload, plen, first & 1);
            payload[plen] = saved;
        }
        if (qos == 1)
            return send_simple(c, PUBACK << 4, 1, id);
        if (qos == 2)
            return send_simple(c, PUBREC << 4, 1, id);
        return 0;
    }
    case PUBREL:
        return n >= 2 ? send_simple(c, PUBCOMP << 4, 1, (uint16_t)(p[0] << 8 | p[1])) : 0;
    case SUBACK:
        if (c->acks_pending > 0)
            c->acks_pending--;
        for (size_t i = 2; i < n; ++i)
            if (p[i] == 0x80) {
                snprintf(c->error, sizeof(c->error), "the broker refused a subscription (ACL?)");
                return 1;                                  // reported, not fatal
            }
        return 0;
    case UNSUBACK:
        if (c->acks_pending > 0)
            c->acks_pending--;
        return 0;
    case PINGRESP:
        c->ping_pending = 0;
        return 0;
    default:
        return 0;                                          // PUBACK/PUBREC/PUBCOMP: we only send QoS 0
    }
}

int mqtt_poll(mqtt_client_t *c, int timeout_ms)
{
    if (c->fd < 0)
        return -1;
    if (mqtt_flush(c) != 0)
        return -1;
    c->error[0] = '\0';
    int64_t until = mqtt_now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        // Keep-alive: something must be sent within the keep-alive period.
        const int64_t now = mqtt_now_ms();
        if (c->keepalive_ms > 0) {
            if (c->ping_pending && now - c->last_rx_ms > c->keepalive_ms)
                return fail(c, "broker does not answer (keep-alive %d s)", c->keepalive_ms / 1000);
            if (!c->ping_pending && now - c->last_tx_ms >= c->keepalive_ms / 2) {
                if (send_simple(c, PINGREQ << 4, 0, 0) != 0)
                    return -1;
                c->ping_pending = 1;
                c->last_rx_ms = now;                       // the answer is due within keepalive
            }
        }
        uint8_t type;
        const uint8_t *body;
        size_t blen;
        const int64_t left = until - mqtt_now_ms();
        const int r = read_packet(c, left > 0 ? (int)left : 0, &type, &body, &blen);
        if (r < 0)
            return -1;
        if (r == 0)
            return 0;
        const uint8_t first = c->rx[0];
        const size_t total = (size_t)(body - c->rx) + blen;
        const int h = handle_packet(c, first, body, blen);
        if (h < 0 || c->fd < 0)                            // the callback may have lost the connection
            return -1;
        consume(c, total);
        if (mqtt_flush(c) != 0)
            return -1;
        // Packets already received are handled before returning; then wait until the deadline.
        if (c->rx_len == 0 && mqtt_now_ms() >= until)
            return 0;
    }
}
