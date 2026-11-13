#include "game/movie.h"

#include "codec/avi.h"
#include "codec/cinepak.h"
#include "codec/msadpcm.h"

#include <stdlib.h>

struct ft_movie {
    SDL_Renderer *r;
    ft_avi avi;
    ft_cinepak *cv;
    uint8_t *buf;               /* one chunk */
    SDL_Texture *tex;
    SDL_AudioStream *stream;
    uint64_t start_ms;
    int32_t frame;              /* last decoded, -1 before the first */
    bool dirty;                 /* frame decoded but not uploaded */
};

/* The whole soundtrack, queued at once (44.5 s of 16-bit stereo is under 8 MB). */
static void start_audio(ft_movie *m, SDL_AudioDeviceID device, float gain)
{
    const ft_avi_audio *fmt = &m->avi.audio;
    if (!device || !m->avi.has_audio || fmt->format != 2)
        return;
    size_t frames = 0;
    for (uint32_t i = 0; i < m->avi.audio_count; i++)
        for (uint32_t off = 0; off < m->avi.audio_chunks[i].size; off += fmt->block_align)
            frames += ft_msadpcm_block_samples(fmt, m->avi.audio_chunks[i].size - off);
    int16_t *pcm = malloc(frames * fmt->channels * sizeof *pcm + 1);
    if (!pcm)
        return;
    size_t got = 0;
    for (uint32_t i = 0; i < m->avi.audio_count; i++)
        if (ft_avi_read(&m->avi, &m->avi.audio_chunks[i], m->buf))
            got += ft_msadpcm_decode(fmt, m->buf, m->avi.audio_chunks[i].size, pcm + got * fmt->channels);
    SDL_AudioSpec spec = { SDL_AUDIO_S16, fmt->channels, (int)fmt->sample_rate };
    m->stream = SDL_CreateAudioStream(&spec, NULL);
    if (m->stream && SDL_BindAudioStream(device, m->stream)) {
        SDL_SetAudioStreamGain(m->stream, gain);
        SDL_PutAudioStreamData(m->stream, pcm, (int)(got * fmt->channels * sizeof *pcm));
        SDL_FlushAudioStream(m->stream);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "movie audio: %s", SDL_GetError());
    }
    free(pcm);
}

ft_movie *ft_movie_open(SDL_Renderer *r, SDL_AudioDeviceID device, const char *path, float gain, uint64_t now_ms)
{
    ft_movie *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->r = r;
    m->frame = -1;
    if (!ft_avi_open(&m->avi, path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "movie: %s", ft_get_error());
        free(m);
        return NULL;
    }
    if (m->avi.video_fourcc != ('c' | 'v' << 8 | 'i' << 16 | (uint32_t)'d' << 24)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "movie: %s isn't Cinepak", path);
        ft_movie_close(m);
        return NULL;
    }
    m->buf = malloc(ft_avi_max_chunk(&m->avi) + 1);
    m->cv = calloc(1, sizeof *m->cv); /* zeroed, so closing before init is safe */
    m->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, (int)m->avi.width,
                               (int)m->avi.height);
    if (!m->buf || !m->cv || !m->tex || !ft_cinepak_init(m->cv, m->avi.width, m->avi.height)) {
        ft_movie_close(m);
        return NULL;
    }
    start_audio(m, device, gain);
    m->start_ms = now_ms;
    return m;
}

void ft_movie_close(ft_movie *m)
{
    if (!m)
        return;
    SDL_DestroyAudioStream(m->stream);
    if (m->tex)
        SDL_DestroyTexture(m->tex);
    if (m->cv)
        ft_cinepak_free(m->cv);
    free(m->cv);
    free(m->buf);
    ft_avi_close(&m->avi);
    free(m);
}

void ft_movie_set_gain(ft_movie *m, float gain)
{
    if (m && m->stream)
        SDL_SetAudioStreamGain(m->stream, gain);
}

bool ft_movie_update(ft_movie *m, uint64_t now_ms)
{
    /* frame n shows from n * scale / rate seconds */
    uint64_t due = (now_ms - m->start_ms) * m->avi.rate / (m->avi.scale * 1000ull);
    if (due >= m->avi.video_count)
        return false;
    while (m->frame < (int32_t)due) {
        const ft_avi_chunk *c = &m->avi.video[++m->frame];
        if (c->size && ft_avi_read(&m->avi, c, m->buf) && ft_cinepak_decode(m->cv, m->buf, c->size))
            m->dirty = true;
    }
    return true;
}

void ft_movie_render(ft_movie *m, const SDL_FRect *dst)
{
    if (m->dirty) {
        SDL_UpdateTexture(m->tex, NULL, m->cv->rgb, (int)m->avi.width * 3);
        m->dirty = false;
    }
    if (m->frame >= 0)
        SDL_RenderTexture(m->r, m->tex, NULL, dst);
}

void ft_movie_size(const ft_movie *m, float *w, float *h)
{
    *w = (float)m->avi.width;
    *h = (float)m->avi.height;
}
