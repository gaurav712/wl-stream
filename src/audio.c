#define _GNU_SOURCE
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *audio_get_default_monitor(void)
{
    FILE *f = popen("pactl info 2>/dev/null | grep 'Default Sink' | awk '{print $3}'", "r");
    if (!f) return strdup("default.monitor");

    char line[256] = {0};
    fgets(line, sizeof(line), f);
    pclose(f);

    size_t len = strlen(line);
    while (len > 0 && (line[len-1] <= ' ')) line[--len] = '\0';
    if (len == 0) return strdup("default.monitor");

    char *out = malloc(len + 9);
    memcpy(out, line, len);
    memcpy(out + len, ".monitor", 9);
    return out;
}
