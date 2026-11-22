// Minimal JSON for commands: "key":"text" and "key":number in one flat object.
#ifndef KBUS_GW_JSON_H
#define KBUS_GW_JSON_H

#include <stddef.h>

// Start of the value of "key" (after the colon), NULL = key not present. j is NUL terminated.
const char *json_value(const char *j, const char *key);
// String value without escapes into buf. 0 = ok, -1 = missing, not a string or too long.
int json_str(const char *j, const char *key, char *buf, size_t cap);
// Integer value. *present = key present. 0 = ok (also when absent), -1 = not an integer.
int json_num(const char *j, const char *key, long *v, int *present);

#endif
