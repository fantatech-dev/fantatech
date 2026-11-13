#include "codec/wav.h"

#include <string.h>

bool ft_wav_parse(const uint8_t *buf, size_t len, ft_wav *out)
{
    memset(out, 0, sizeof *out);
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        ft_set_error("not a RIFF WAVE file");
        return false;
    }
    bool have_fmt = false;
    size_t pos = 12;
    while (len - pos >= 8) {
        const uint8_t *chunk = buf + pos;
        uint32_t size = ft_rd32(chunk + 4);
        if (size > len - pos - 8) {
            ft_set_error("WAVE chunk %.4s runs past the end", (const char *)chunk);
            return false;
        }
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            out->format = ft_rd16(chunk + 8);
            out->channels = ft_rd16(chunk + 10);
            out->sample_rate = ft_rd32(chunk + 12);
            out->bits_per_sample = ft_rd16(chunk + 22);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!have_fmt) {
                ft_set_error("WAVE data chunk before fmt");
                return false;
            }
            out->data = chunk + 8;
            out->data_size = size;
            return true;
        }
        pos += 8 + (size_t)size + (size & 1);
    }
    ft_set_error("WAVE file has no data chunk");
    return false;
}
