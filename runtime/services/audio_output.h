#pragma once

#ifdef _WIN32
#include <SDL3/SDL_audio.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_output_init(void);
// Physical playback IDs are valid for this SDL session. The default sentinel follows Windows.
SDL_AudioDeviceID audio_output_preference(void);
void audio_output_select(SDL_AudioDeviceID device);
// The renderer uses stereo until a requested 5.1 layout has a compatible endpoint.
bool audio_output_surround(void);
void audio_output_set_surround(bool enabled);

#ifdef __cplusplus
}
#endif
#endif
