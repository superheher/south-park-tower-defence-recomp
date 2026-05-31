// Standalone probe: opens the default playback device EXACTLY like
// SDLAudioDriver::Initialize (48000 Hz, F32LE, 6ch), then measures how fast SDL
// pulls from the get-callback vs realtime. Reproduces (or refutes) the "audio
// dump grows at 3x realtime / 70% silence" finding without the game or an SDK build.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

static atomic_llong g_bytes = 0;   // total bytes requested (additional_amount)
static atomic_int g_calls = 0;
static int g_chan = 6;

static void SDLCALL cb(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    (void)userdata; (void)total_amount;
    atomic_fetch_add(&g_calls, 1);
    // mimic the driver: fill `additional_amount` with 256-sample frames of silence
    const int chunk_samples = 256 * g_chan;
    const int chunk_bytes = (int)sizeof(float) * chunk_samples;
    static float buf[256 * 8];
    memset(buf, 0, sizeof(buf));
    while (additional_amount > 0) {
        SDL_PutAudioStreamData(stream, buf, chunk_bytes);
        additional_amount -= chunk_bytes;
        atomic_fetch_add(&g_bytes, chunk_bytes);
    }
}

int main(void) {
    // Replicate SDLAudioDriver::Initialize's hints exactly (the only env difference vs the bare probe).
    SDL_SetHintWithPriority(SDL_HINT_TIMER_RESOLUTION, "0", SDL_HINT_OVERRIDE);
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "init: %s\n", SDL_GetError()); return 1; }
    printf("SDL audio driver: %s\n", SDL_GetCurrentAudioDriver());

    SDL_AudioSpec desired = {0};
    desired.freq = 48000;
    desired.format = SDL_AUDIO_F32LE;
    desired.channels = 6;
    g_chan = 6;

    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired, cb, NULL);
    if (!stream) { fprintf(stderr, "open: %s\n", SDL_GetError()); return 1; }
    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(stream);

    SDL_AudioSpec obtained = {0};
    int sample_frames = 0;
    if (SDL_GetAudioDeviceFormat(dev, &obtained, &sample_frames))
        printf("DEVICE obtained: freq=%d channels=%d format=0x%04X sample_frames(buffer)=%d\n",
               obtained.freq, obtained.channels, (unsigned)obtained.format, sample_frames);
    else
        printf("GetAudioDeviceFormat failed: %s\n", SDL_GetError());

    // also report the stream's own src/dst formats
    SDL_AudioSpec src = {0}, dst = {0};
    if (SDL_GetAudioStreamFormat(stream, &src, &dst))
        printf("STREAM src: freq=%d ch=%d fmt=0x%04X | dst: freq=%d ch=%d fmt=0x%04X\n",
               src.freq, src.channels, (unsigned)src.format, dst.freq, dst.channels, (unsigned)dst.format);

    SDL_ResumeAudioDevice(dev);

    const double SECONDS = 3.0;
    Uint64 t0 = SDL_GetTicksNS();
    SDL_Delay((Uint32)(SECONDS * 1000));
    double elapsed = (SDL_GetTicksNS() - t0) / 1e9;

    long long bytes = atomic_load(&g_bytes);
    int calls = atomic_load(&g_calls);
    double rt_bytes = 48000.0 * 6 * sizeof(float) * elapsed;  // realtime expectation for 48k/6ch/F32
    printf("\n--- over %.2fs: callbacks=%d  pulled=%lld bytes (%.3f MB)\n", elapsed, calls, bytes, bytes/1e6);
    printf("    realtime expectation (48k/6ch/F32) = %.3f MB\n", rt_bytes/1e6);
    printf("    PULL RATIO = %.3fx realtime   (1.0=correct; ~3.0 reproduces the game bug)\n", bytes/rt_bytes);
    printf("    => audio-content seconds pulled = %.2fs of source per %.2fs wall\n",
           bytes/(48000.0*6*sizeof(float)), elapsed);

    SDL_DestroyAudioStream(stream);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return 0;
}
