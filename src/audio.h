#pragma once
#include <cstdint>
#include <SDL3/SDL.h>

// Tiny wav playback layer on top of SDL3's built-in audio. No SDL_mixer or
// other extension is used.
//
// Model:
//   * sfx_load() decodes a .wav once via SDL_LoadWAV; PCM is cached in Sfx.
//   * audio_init() opens a fixed pool of SDL_AudioStreams, each bound to the
//     default playback device. SDL sums their output automatically, so the
//     pool gives us cheap polyphony without writing a mixer.
//   * sfx_play() picks a free voice (or steals one) and queues the cached
//     PCM into it. SDL's audio thread pulls it out on its own.
//
// Current limitations (intentionally small surface; expand when needed):
//   * Voice pool is fixed-size (AUDIO_VOICE_COUNT). When every voice is in
//     use, sfx_play() steals the voice with the most data still queued and
//     cuts off whatever was playing on it.
//   * No looping, no per-voice pitch shifting, no positional / 3D audio.
//   * No streaming: the entire decoded PCM is held in memory per Sfx.
//   * No master/bus volume -- only the per-call `volume` gain.
//   * No hot-reload: replacing a wav on disk requires sfx_free + sfx_load.
//   * Device is opened once at audio_init(); device hot-swap (e.g. user
//     plugging in headphones at runtime) is not handled.

struct Sfx {
    SDL_AudioSpec spec;   // source format as reported by SDL_LoadWAV
    uint8_t*      pcm;    // SDL_LoadWAV-owned buffer; released via SDL_free
    uint32_t      bytes;  // size of `pcm` in bytes
};

#define AUDIO_VOICE_COUNT 8

struct AudioState {
    SDL_AudioStream* voices[AUDIO_VOICE_COUNT];
    bool             ready;
};

// Opens the default playback device and allocates the voice pool.
// Requires SDL_Init to have been called with SDL_INIT_AUDIO.
bool   audio_init(AudioState* audio);
void   audio_shutdown(AudioState* audio);

// Returns false (and leaves *out zeroed) if the wav can't be loaded.
bool   sfx_load(Sfx* out, const char* wav_path);
void   sfx_free(Sfx* s);

// Linear gain. 1.0 = unity, 0.5 = -6 dB, 0.0 = silent. Values >1 are allowed
// but may clip on the device output.
void   sfx_play(AudioState* audio, const Sfx* s, float volume);

// Drops any audio currently queued on every voice (useful on scene changes).
void   sfx_stop_all(AudioState* audio);
