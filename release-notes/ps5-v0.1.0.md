## MikuPan PS5 v0.1.0

The first release of MikuPan for a jailbroken PlayStation 5: a native title
that ShadowMount+ mounts, with the over-the-shoulder camera from this fork.

- **Native PS5 title.** Rendering is SDL_GPU on Vulkan, through
  [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)'s port of Mesa's RADV
  driver, linked into the title. 1920x1080 at 60 fps.
- **Console drivers for SDL3:** video on the console's display, sound through
  AudioOut, and up to four DualSense controllers.
- **Pauses at the home screen:** while the home screen, the control centre or a
  system dialog has the controller, the game, its sound and its movies wait
  until you are back.
- **Over-the-shoulder camera**, Scare Camera and the rest of this fork's
  options. Desktop-only options (window mode, HDR, GPU backend, keyboard
  bindings, data folder) are hidden.
- **Overlay:** L3 + R3 shows and hides the overlay menu bar (L3 + Options opens
  the console's own debug menu on a jailbroken PS5).
- Tested through Night 1 and into Night 2: game over, movies, saves, puzzles
  and ghost battles.

## Installing

This package is for the **US (NTSC-U)** disc.

1. Copy the `PPSA39390` folder to `/data/homebrew/` on the console (or
   `homebrew/` on a USB drive) for ShadowMount+ to mount. It appears on the home
   screen as **MikuPan PS5**.
2. Copy `IMG_HD.BIN` and `IMG_BD.BIN` from your disc to `/data/MikuPan/`.
3. For the FMV cutscenes, also copy the disc's `MOVIE` and `MOVIE2` folders to
   `/data/MikuPan/`, so that `/data/MikuPan/MOVIE/SCN0010.PSS` exists.
   Upper or lower case names both work.
4. Start it. The first launch converts the game's textures once (about two
   minutes); later launches start straight away.

Settings, saves, the converted textures, the shader cache and `mikupan.log`
are kept in `/data/MikuPan/`. If the game crashes, `crash.log` there says
where; please include it and `mikupan.log` when reporting a problem.

## Known issues

- There is no on-screen keyboard and no file browser, so PS2 save transfer is
  not available on the console.
