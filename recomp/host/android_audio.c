#ifdef __ANDROID__
// DirectSound PCM mixer. The real-time callback owns no guest pointers: Unlock
// publishes a native copy, and teardown closes the stream before freeing it.
#include "android_audio.h"
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    uint32_t id, size, rate, frequency;
    uint16_t channels, bits;
    unsigned char *pcm;
    double frame;
    int playing, loop;
    float left, right;
} Voice;
static Voice voices[256];
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
static AAudioStream *stream;
static int output_rate = 48000;

static Voice *find_voice(uint32_t id, int create) {
    Voice *empty = NULL;
    for (int i = 0; i < 256; ++i) {
        if (voices[i].id == id) return &voices[i];
        if (!voices[i].id && !empty) empty = &voices[i];
    }
    if (create && empty) { empty->id = id; empty->left = empty->right = 1; return empty; }
    return NULL;
}
static float sample(const Voice *v, unsigned frame, unsigned channel) {
    size_t index = (size_t)frame * v->channels + (v->channels == 1 ? 0 : channel);
    if (v->bits == 8) return ((int)v->pcm[index] - 128) / 128.0f;
    int16_t value; memcpy(&value, v->pcm + index * 2, 2);
    return value / 32768.0f;
}
// Called with audio_lock held; no allocation, logging, guest execution or I/O.
static void mix(float *out, int count, int rate) {
    memset(out, 0, (size_t)count * 2 * sizeof(float));
    for (int j = 0; j < 256; ++j) {
        Voice *v = &voices[j];
        if (!v->playing || !v->pcm || !v->channels || !v->bits) continue;
        unsigned frames = v->size / (v->channels * (v->bits / 8));
        if (!frames) { v->playing = 0; continue; }
        double step = (double)(v->frequency ? v->frequency : v->rate) / rate;
        for (int i = 0; i < count; ++i) {
            if (v->frame >= frames) {
                if (!v->loop) { v->playing = 0; v->frame = 0; break; }
                v->frame = fmod(v->frame, frames);
            }
            unsigned a = (unsigned)v->frame, b = a + 1;
            if (b >= frames) b = v->loop ? 0 : a;
            float t = (float)(v->frame - a);
            for (unsigned ch = 0; ch < 2; ++ch) {
                float value = sample(v, a, ch) * (1 - t) + sample(v, b, ch) * t;
                out[i * 2 + ch] += value * (ch ? v->right : v->left);
            }
            v->frame += step;
        }
    }
    for (int i = 0; i < count * 2; ++i) out[i] = fmaxf(-1, fminf(1, out[i]));
}
static aaudio_data_callback_result_t audio_callback(AAudioStream *s, void *user, void *data, int32_t frames) {
    (void)s; (void)user;
    if (!pthread_mutex_trylock(&audio_lock)) {
        mix(data, frames, output_rate);
        pthread_mutex_unlock(&audio_lock);
    } else memset(data, 0, (size_t)frames * 2 * sizeof(float));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
int android_audio_open(void) {
    pthread_mutex_lock(&stream_lock);
    if (stream) { pthread_mutex_unlock(&stream_lock); return 1; }
    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t error = AAudio_createStreamBuilder(&builder);
    if (error != AAUDIO_OK) { pthread_mutex_unlock(&stream_lock); return 0; }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, audio_callback, NULL);
    error = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (error == AAUDIO_OK && AAudioStream_getFormat(stream) == AAUDIO_FORMAT_PCM_FLOAT &&
        AAudioStream_getChannelCount(stream) == 2) {
        output_rate = AAudioStream_getSampleRate(stream);
        error = AAudioStream_requestStart(stream);
    } else error = AAUDIO_ERROR_INVALID_FORMAT;
    if (error != AAUDIO_OK && stream) { AAudioStream_close(stream); stream = NULL; }
    pthread_mutex_unlock(&stream_lock);
    return error == AAUDIO_OK;
}
void android_audio_reset(void) {
    // No other control thread may use the stream during close. close joins its
    // callback, then native buffers can be discarded without touching guest RAM.
    pthread_mutex_lock(&stream_lock);
    if (stream) { AAudioStream_requestStop(stream); AAudioStream_close(stream); stream = NULL; }
    pthread_mutex_lock(&audio_lock);
    for (int i = 0; i < 256; ++i) free(voices[i].pcm);
    memset(voices, 0, sizeof voices);
    pthread_mutex_unlock(&audio_lock);
    pthread_mutex_unlock(&stream_lock);
}
int android_audio_buffer(uint32_t id, const void *pcm, uint32_t size, uint16_t channels, uint16_t bits, uint32_t rate) {
    if (!id || !size || size > 64 * 1024 * 1024 || (channels != 1 && channels != 2) ||
        (bits != 8 && bits != 16) || rate < 100 || rate > 200000) return 0;
    // New Vegas adds byte padding to some one-shot buffers. DirectSound's
    // guest buffer keeps that capacity; the mixer only reads complete frames.
    uint32_t frame_bytes = channels * (bits / 8);
    size -= size % frame_bytes;
    if (!size) return 0;
    unsigned char *copy = malloc(size);
    if (!copy) return 0;
    memcpy(copy, pcm, size);
    pthread_mutex_lock(&audio_lock);
    Voice *v = find_voice(id, 1);
    if (v) {
        free(v->pcm); v->pcm = copy; v->size = size;
        v->channels = channels; v->bits = bits; v->rate = rate;
    }
    pthread_mutex_unlock(&audio_lock);
    if (!v) free(copy);
    return v != NULL;
}
void android_audio_remove(uint32_t id) {
    pthread_mutex_lock(&audio_lock);
    Voice *v = find_voice(id, 0);
    if (v) { free(v->pcm); memset(v, 0, sizeof *v); }
    pthread_mutex_unlock(&audio_lock);
}
void android_audio_control(uint32_t id, int playing, int loop, uint32_t position, int volume, int pan, uint32_t frequency) {
    pthread_mutex_lock(&audio_lock);
    Voice *v = find_voice(id, 0);
    if (v) {
        if (playing >= 0) { v->playing = playing; v->loop = loop; }
        v->frequency = frequency;
        if (position != UINT32_MAX) v->frame = position / (double)(v->channels * (v->bits / 8));
        float gain = volume <= -10000 ? 0 : powf(10, volume / 2000.0f);
        v->left = gain * (pan > 0 ? powf(10, -pan / 2000.0f) : 1);
        v->right = gain * (pan < 0 ? powf(10, pan / 2000.0f) : 1);
    }
    pthread_mutex_unlock(&audio_lock);
}
uint32_t android_audio_position(uint32_t id, int *playing) {
    pthread_mutex_lock(&audio_lock);
    Voice *v = find_voice(id, 0);
    uint32_t position = v ? (uint32_t)v->frame * v->channels * (v->bits / 8) : 0;
    if (v && v->size) position %= v->size;
    *playing = v ? v->playing : 0;
    pthread_mutex_unlock(&audio_lock);
    return position;
}
int android_audio_selftest(void) {
    // Mix into scratch RAM, never play the fixture through the speaker.
    int16_t pcm[] = {16384, -16384}; float out[12]; int playing;
    if (!android_audio_buffer(1, pcm, sizeof pcm, 1, 16, 24000)) return 1;
    android_audio_control(1, 1, 1, 0, 0, 0, 0);
    pthread_mutex_lock(&audio_lock); mix(out, 4, 48000); pthread_mutex_unlock(&audio_lock);
    if (fabsf(out[0] - .5f) > .0001f || out[1] != out[0] || fabsf(out[2]) > .0001f ||
        fabsf(out[4] + .5f) > .0001f || android_audio_position(1, &playing) != 0 || !playing) return 2;
    android_audio_control(1, 1, 0, 0, -10000, 0, 48000);
    pthread_mutex_lock(&audio_lock); mix(out, 3, 48000); pthread_mutex_unlock(&audio_lock);
    for (int i = 0; i < 6; ++i) if (out[i]) return 3;
    android_audio_position(1, &playing); if (playing) return 4;
    // Property changes preserve completion even when the guest cached playing=1.
    android_audio_control(1, -1, 0, UINT32_MAX, 0, 0, 24000);
    pthread_mutex_lock(&audio_lock); mix(out, 2, 48000); pthread_mutex_unlock(&audio_lock);
    android_audio_position(1, &playing); if (playing) return 5;
    for (int i=0;i<4;++i) if (out[i]) return 6;
    const unsigned char padded_pcm[] = {0, 64, 0, 192, 255};
    if (!android_audio_buffer(2, padded_pcm, sizeof padded_pcm, 2, 16, 48000)) return 7;
    android_audio_control(2, 1, 0, 0, 0, 0, 0);
    pthread_mutex_lock(&audio_lock); mix(out, 2, 48000); pthread_mutex_unlock(&audio_lock);
    if (out[0] != .5f || out[1] != -.5f || out[2] || out[3]) return 8;
    android_audio_reset();
    return 0;
}

#endif // __ANDROID__
