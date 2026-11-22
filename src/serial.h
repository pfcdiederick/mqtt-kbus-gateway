// Serial interface COM1 (/dev/serial): transparent forwarding over MQTT.
//
// The gateway does not interpret the bytes: what a client publishes on
// <prefix>/serial/com1/tx/set is sent as is, what arrives is split into frames
// by a pause of FrameGap ms and published as one <prefix>/serial/com1/rx message
// per frame. The protocol lives in the client (CODESYS: FB_MqttSerial). RS-232 /
// RS-485 is the mode set in the WBM (Interfaces > Serial Interface, owner "None");
// the RS-485 direction is switched by the driver.
//
// The receive thread puts complete frames in a queue (SERIAL_QUEUE frames); the
// MQTT thread takes them out at every tick (serial_pop_frame). When the queue is
// full, the oldest frame is dropped and counted.
#ifndef KBUS_GW_SERIAL_H
#define KBUS_GW_SERIAL_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define SERIAL_BUF      512     // longest frame, longest transmit (bytes)
#define SERIAL_QUEUE    64      // complete frames waiting for the MQTT thread

typedef struct {
    int      enabled;            // SERIAL_DEVICE set
    int      open;               // device open
    uint32_t baud;
    char     format[4];          // e.g. "8E1"
    uint32_t frame_gap_ms;
    uint64_t tx_count, rx_count, frame_count, dropped, errors;
} serial_snapshot_t;

// Start the receive thread (does nothing when SERIAL_DEVICE is empty). 0 = ok.
int  serial_start(const gw_config_t *cfg);
void serial_stop(void);
int  serial_enabled(void);
void serial_snapshot(serial_snapshot_t *s);

// Validate settings: 0 = ok.
int  serial_valid_baud(long baud);
int  serial_valid_format(const char *fmt, size_t len);
// Apply new settings (any of them; baud 0 / fmt NULL / gap 0 = unchanged). 0 = ok.
int  serial_configure(uint32_t baud, const char *fmt, uint32_t gap_ms);

// Send bytes (blocks until written or 2 s). 0 = ok, -1 = port not open / write failed.
int  serial_write(const uint8_t *data, size_t len);

// Take the oldest complete frame out of the queue: its length, 0 = queue empty.
// buf must hold SERIAL_BUF bytes.
size_t serial_pop_frame(uint8_t *buf);

#endif
