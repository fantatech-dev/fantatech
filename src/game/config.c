#include "game/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const volume_keys[FT_VOL_COUNT] = { "volume.master", "volume.music", "volume.voice", "volume.se" };

void ft_config_default(ft_config *c)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < FT_VOL_COUNT; i++)
        c->volume[i] = i == FT_VOL_MASTER ? 100 : 80;
    c->fullscreen = true;
}

bool ft_config_load(ft_config *c, const char *path)
{
    ft_config_default(c);
    FILE *f = ft_fopen(path, "rb");
    if (!f)
        return false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *val = strchr(line, ' ');
        if (!val)
            val = line + strlen(line);
        else
            *val++ = 0;
        if (strcmp(line, "language") == 0)
            snprintf(c->lang, sizeof c->lang, "%s", val);
        if (strcmp(line, "display.fullscreen") == 0)
            c->fullscreen = atoi(val) != 0;
        for (int i = 0; i < FT_VOL_COUNT; i++)
            if (strcmp(line, volume_keys[i]) == 0) {
                int v = atoi(val);
                c->volume[i] = v < 0 ? 0 : v > 100 ? 100 : v;
            }
    }
    fclose(f);
    return true;
}

bool ft_config_save(const ft_config *c, const char *path)
{
    FILE *f = ft_fopen(path, "wb");
    if (!f) {
        ft_set_error("cannot write %s", path);
        return false;
    }
    fprintf(f, "language %s\n", c->lang);
    for (int i = 0; i < FT_VOL_COUNT; i++)
        fprintf(f, "%s %d\n", volume_keys[i], c->volume[i]);
    fprintf(f, "display.fullscreen %d\n", c->fullscreen ? 1 : 0);
    return fclose(f) == 0;
}

float ft_config_gain(const ft_config *c, int group)
{
    float g = (float)c->volume[FT_VOL_MASTER] / 100.0f * (float)c->volume[group] / 100.0f;
    return g * g;
}
