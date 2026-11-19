#ifndef MIKUPAN_MIKUPAN_PS5_H
#define MIKUPAN_MIKUPAN_PS5_H

#ifdef __PROSPERO__

#ifdef __cplusplus
extern "C" {
#endif

/// The writable folder for settings, the baked IMG_TEX.BIN, saves and the
/// log, with a trailing separator: /data/MikuPan/ when the title can write
/// there, like other homebrew, otherwise the title's own /download0/MikuPan/.
/// Never returns NULL. Implemented in mikupan_file.cpp.
const char *MikuPan_GetPs5UserDirectory(void);

/// Whether a file or folder exists, found by opening it (fopen, then
/// opendir). Implemented in mikupan_file.cpp.
bool MikuPan_Ps5PathExists(const char *path);

/// Shows a PS5 system notification (the pop-up in the top corner), and
/// writes the same text to the log. Useful while the game has no other way
/// to show anything on the console.
void MikuPan_Ps5Notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// Sends SDL's own log messages (SDL_GPU's reasons for refusing a driver
/// among them) to MikuPan's log, at every priority for SDL's GPU, video and
/// system categories. Call before SDL_Init.
void MikuPan_Ps5ForwardSdlLog(void);

/// The opt-in sampling profiler (/data/MikuPan/profile.on at launch): start
/// it from the main thread, and report each frame's duration to it.
void MikuPan_Ps5ProfilerStart(void);
void MikuPan_Ps5ProfilerFrame(double frame_ms);

/// Whether the game should stay paused this frame: true while the console's
/// home screen or a system menu has the controller. Pauses the game's audio
/// on the way in and resumes it on the way out. Call once a frame.
bool MikuPan_Ps5ShouldPause(void);

/// Walks through what SDL_GPU's Vulkan backend needs from RADV (an
/// instance, a GPU, its features and extensions, a graphics queue, the
/// display) and logs each answer, to say why SDL_GPU refused Vulkan.
void MikuPan_Ps5VulkanReport(void);

#ifdef __cplusplus
}
#endif

#endif // __PROSPERO__

#endif // MIKUPAN_MIKUPAN_PS5_H
