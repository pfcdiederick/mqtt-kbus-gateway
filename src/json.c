#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *json_value(const char *j, const char *key)
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

int json_str(const char *j, const char *key, char *buf, size_t cap)
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

int json_num(const char *j, const char *key, long *v, int *present)
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
