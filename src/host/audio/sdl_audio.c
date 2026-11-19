#include "sdl_audio.h"

#include <SDL3/SDL.h>

#define CHUNK_FRAMES 1024
#define DEVICE_SAMPLE_FRAMES "1024"

static struct {
    SDL_Mutex* mutex;
    SDL_AudioStream* stream;
    audio_mixer_t* mixer;
    int subsystem;
    float chunk[CHUNK_FRAMES * 2];
} A;

static void mixer_lock(void* context) {
    (void)context;
    SDL_LockMutex(A.mutex);
}

static void mixer_unlock(void* context) {
    (void)context;
    SDL_UnlockMutex(A.mutex);
}

static void SDLCALL feed(void* userdata, SDL_AudioStream* stream, int additional, int total) {
    int frames = additional / (int)(2 * sizeof(float));
    (void)userdata;
    (void)total;
    while (frames > 0) {
        int n = frames < CHUNK_FRAMES ? frames : CHUNK_FRAMES;
        audio_mixer_mix(A.mixer, A.chunk, (uint32_t)n);
        if (!SDL_PutAudioStreamData(stream, A.chunk, n * (int)(2 * sizeof(float)))) return;
        frames -= n;
    }
}

static SDL_AudioSpec stream_spec(uint32_t rate) {
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = (int)rate;
    return spec;
}

static void sdl_close(void) {
    if (A.stream) SDL_DestroyAudioStream(A.stream);
    audio_mixer_destroy(A.mixer);
    if (A.mutex) SDL_DestroyMutex(A.mutex);
    if (A.subsystem) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    SDL_zero(A);
}

static audio_mixer_t* sdl_open(uint32_t rate) {
    audio_lock_ops_t ops = { NULL, mixer_lock, mixer_unlock };
    SDL_AudioSpec spec = stream_spec(rate);
    if (!rate || A.mixer) return NULL;
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "wasapi");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, DEVICE_SAMPLE_FRAMES);
    A.subsystem = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!A.subsystem) return NULL;
    A.mutex = SDL_CreateMutex();
    A.mixer = A.mutex ? audio_mixer_create(rate, &ops) : NULL;
    if (A.mixer) A.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!A.stream || !SDL_ResumeAudioStreamDevice(A.stream)) {
        sdl_close();
        return NULL;
    }
    return A.mixer;
}

static void sdl_set_rate(uint32_t rate) {
    SDL_AudioSpec spec = stream_spec(rate);
    if (!A.stream || !rate) return;
    if (SDL_SetAudioStreamFormat(A.stream, &spec, NULL)) audio_mixer_set_rate(A.mixer, rate);
}

static const char* sdl_name(void) {
    const char* name = SDL_GetCurrentAudioDriver();
    return name ? name : "none";
}

const audio_backend_t* sdl_audio_backend(void) {
    static const audio_backend_t backend = { sdl_open, sdl_set_rate, sdl_close, sdl_name };
    return &backend;
}
