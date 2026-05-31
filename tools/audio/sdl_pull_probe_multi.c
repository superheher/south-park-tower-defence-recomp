// Multi-stream variant of sdl_pull_probe.c. Opens N playback streams EXACTLY like
// SDLAudioDriver::Initialize (48000 Hz, F32LE, 6ch) -- one per registered XAudio
// render-driver client -- and measures the AGGREGATE and PER-STREAM pull rate.
//
// Hypothesis under test: the game's observed "3x get-callback over-pull" is NOT a
// single stream pulling 3x; it is the game opening 3 SDL streams (3 XAudio clients),
// each pulling a correct 1x, summed in one --audio_dump file. If so, this probe with
// N=3 reproduces the 3x AGGREGATE while each stream stays ~1.0x.
//
// Usage: probe_multi [N]   (default N=3). Build:
//   cc -O2 -I<sdk>/out/install/.../include sdl_pull_probe_multi.c \
//      <sdk>/out/install/.../lib64/libSDL3.a -lm -ldl -lpthread -lrt -o probe_multi
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define MAXN 8
static atomic_llong g_bytes[MAXN];   // per-stream bytes requested (additional_amount)
static atomic_int   g_calls[MAXN];
static int g_chan = 6;

static void SDLCALL cb(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    (void)total_amount;
    const int idx = (int)(intptr_t)userdata;
    atomic_fetch_add(&g_calls[idx], 1);
    const int chunk_bytes = (int)sizeof(float) * 256 * g_chan;  // one 256-sample 6ch frame
    static float buf[256 * 8];
    memset(buf, 0, sizeof(buf));
    while (additional_amount > 0) {
        SDL_PutAudioStreamData(stream, buf, chunk_bytes);
        additional_amount -= chunk_bytes;
        atomic_fetch_add(&g_bytes[idx], chunk_bytes);
    }
}

int main(int argc, char **argv) {
    int N = (argc > 1) ? atoi(argv[1]) : 3;
    if (N < 1) N = 1; if (N > MAXN) N = MAXN;

    // Replicate SDLAudioDriver::Initialize's hints exactly.
    SDL_SetHintWithPriority(SDL_HINT_TIMER_RESOLUTION, "0", SDL_HINT_OVERRIDE);
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "init: %s\n", SDL_GetError()); return 1; }
    printf("SDL audio driver: %s   opening N=%d streams (6ch/48000/F32LE)\n",
           SDL_GetCurrentAudioDriver(), N);

    SDL_AudioStream *streams[MAXN] = {0};
    for (int i = 0; i < N; i++) {
        SDL_AudioSpec desired = {0};
        desired.freq = 48000; desired.format = SDL_AUDIO_F32LE; desired.channels = 6;
        streams[i] = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired, cb, (void*)(intptr_t)i);
        if (!streams[i]) { fprintf(stderr, "open %d: %s\n", i, SDL_GetError()); return 1; }
        SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(streams[i]);
        SDL_AudioSpec obtained = {0}; int sf = 0;
        if (SDL_GetAudioDeviceFormat(dev, &obtained, &sf))
            printf("  stream#%d DEVICE freq=%d ch=%d sample_frames=%d\n", i, obtained.freq, obtained.channels, sf);
        SDL_ResumeAudioDevice(dev);
    }

    const double SECONDS = 4.0;
    Uint64 t0 = SDL_GetTicksNS();
    SDL_Delay((Uint32)(SECONDS * 1000));
    double elapsed = (SDL_GetTicksNS() - t0) / 1e9;

    double rt_bytes = 48000.0 * 6 * sizeof(float) * elapsed;  // 1x for ONE stream
    long long agg = 0; int aggc = 0;
    printf("\n--- over %.2fs ---\n", elapsed);
    for (int i = 0; i < N; i++) {
        long long b = atomic_load(&g_bytes[i]); int c = atomic_load(&g_calls[i]);
        agg += b; aggc += c;
        printf("  stream#%d: calls=%d (%.1f/s)  pulled=%.3f MB  per-stream ratio=%.3fx\n",
               i, c, c/elapsed, b/1e6, b/rt_bytes);
    }
    printf("  AGGREGATE: calls=%d (%.1f/s)  pulled=%.3f MB  ratio-vs-1x-stream=%.3fx\n",
           aggc, aggc/elapsed, agg/1e6, agg/rt_bytes);
    printf("  (game observed: ~140 calls/s aggregate, 3.00x. If N=3 here ~matches, the\n");
    printf("   '3x' is N streams x 1x each, NOT a single-stream over-pull.)\n");

    for (int i = 0; i < N; i++) SDL_DestroyAudioStream(streams[i]);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return 0;
}
