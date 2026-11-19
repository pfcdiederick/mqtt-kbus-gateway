// Minimal MQTT 3.1.1 client (plain TCP, no library): CONNECT with last will,
// QoS 0 PUBLISH (outgoing messages are collected and sent with mqtt_flush()),
// SUBSCRIBE / UNSUBSCRIBE, incoming PUBLISH of any QoS, keep-alive.
// Not thread safe: one thread uses one client.
#ifndef KBUS_MQTT_CLIENT_H
#define KBUS_MQTT_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define MQTT_RX_MAX  (64 * 1024)    // largest incoming packet; bigger ones end the connection
#define MQTT_TX_BUF  (64 * 1024)    // outgoing messages are collected up to this size

typedef struct {
    const char *host;
    int         port;
    const char *client_id;
    const char *username;          // NULL or "" = none
    const char *password;          // NULL or "" = none
    int         keepalive_s;
    const char *will_topic;        // NULL = no last will
    const char *will_payload;
    int         will_retain;
    int         timeout_ms;        // TCP connect + CONNACK
} mqtt_opts_t;

// Incoming PUBLISH. retained = 1 for a stored retained message (delivered at subscribe time).
typedef void (*mqtt_msg_cb)(void *ctx, const char *topic, const char *payload, size_t len, int retained);

typedef struct {
    int         fd;
    int         keepalive_ms;
    int64_t     last_tx_ms;
    int64_t     last_rx_ms;
    int         ping_pending;
    uint16_t    next_id;
    int         acks_pending;      // SUBACK / UNSUBACK not received yet
    mqtt_msg_cb on_msg;
    void       *ctx;
    uint8_t     rx[MQTT_RX_MAX + 8];
    size_t      rx_len;
    uint8_t     tx[MQTT_TX_BUF];
    size_t      tx_len;
    char        error[160];        // reason of the last failure
} mqtt_client_t;

void mqtt_init(mqtt_client_t *c, mqtt_msg_cb on_msg, void *ctx);
// 0 = connected (CONNACK accepted); -1 = failed, reason in c->error.
int  mqtt_connect(mqtt_client_t *c, const mqtt_opts_t *o);
int  mqtt_connected(const mqtt_client_t *c);
// Queue one QoS 0 message; it is sent by mqtt_flush() (or earlier when the buffer is full).
// payload may be NULL with len 0 (with retain: deletes the retained message). 0 = ok.
int  mqtt_publish(mqtt_client_t *c, const char *topic, const void *payload, size_t len, int retain);
int  mqtt_publish_str(mqtt_client_t *c, const char *topic, const char *payload, int retain);
int  mqtt_flush(mqtt_client_t *c);
// Subscribe (QoS 0) / unsubscribe n topic filters in one packet. 0 = sent.
int  mqtt_subscribe(mqtt_client_t *c, const char *const *filters, size_t n);
int  mqtt_unsubscribe(mqtt_client_t *c, const char *const *filters, size_t n);
// Flush, wait up to timeout_ms for data, handle every complete packet (calls on_msg),
// keep the connection alive. 0 = ok, -1 = connection lost (reason in c->error).
int  mqtt_poll(mqtt_client_t *c, int timeout_ms);
// Clean disconnect (DISCONNECT packet: the broker does NOT send the last will).
void mqtt_disconnect(mqtt_client_t *c);
// Drop the connection without DISCONNECT.
void mqtt_close(mqtt_client_t *c);

int64_t mqtt_now_ms(void);

#endif
