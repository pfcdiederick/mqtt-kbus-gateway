// 750-671 stepper controller: functional driver (positioning application,
// Application_Selector = 1, cyclic process image; manual 750-671, 2.1.2.7 and 2.1.2.11.1).
//
// The handshake with the module (Enable -> Ready, Stop2_N -> Stop_N_ACK, mode bit ->
// mode _ACK, Start edge -> Start_ACK) runs in the gateway at every MQTT tick; MQTT
// carries only commands and the result:
//
//   <prefix>/module/<pos>/stepper/state      JSON state (retained, on change)
//   <prefix>/module/<pos>/stepper/cmd/set    JSON command (without retain):
//        {"cmd":"enable"} {"cmd":"disable"} {"cmd":"quit"} {"cmd":"stop"}
//        {"cmd":"move","position":N,"velocity":V,"acceleration":A}   absolute position
//        {"cmd":"reference","direction":"neg"|"pos"}
//        optional "id": echoed in the response
//   <prefix>/module/<pos>/stepper/response   {"id":..,"cmd":..,"result":"accepted"|"done"|"error","error":..}
//
// Process image (12 bytes): 0 C0/S0, 1 reserved, 2..8 D0..D6, 9 C3/S3, 10 C2/S2, 11 C1/S1.
#ifndef KBUS_GW_STEPPER_H
#define KBUS_GW_STEPPER_H

#include <stddef.h>
#include <stdint.h>

#define STEPPER_PI_BYTES 12

// One tick for the stepper at module index mi: read the inputs, advance the handshake,
// write the outputs. Publishing goes through the callbacks of mqtt_io.
typedef void (*stepper_pub_fn)(uint16_t pos, const char *what, const char *payload, int retain);
void stepper_tick(size_t mi, uint16_t pos, int64_t now_ms, int full, stepper_pub_fn pub);

// A command (JSON). Returns 0 when accepted for processing; else an error text in err.
int  stepper_command(size_t mi, uint16_t pos, const char *json, size_t len, char *err, size_t errlen);

// Safe state (write watchdog): controlled stop (mode and Start off, the module brakes
// with Acceleration_Stop_Fast); Enable stays, so the motor keeps its holding current.
void stepper_safe(size_t mi);

// Forget the state of all steppers (bus rescan).
void stepper_reset_all(void);

#endif
