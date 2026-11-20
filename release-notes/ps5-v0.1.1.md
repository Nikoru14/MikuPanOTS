## MikuPan PS5 v0.1.1

The PlayStation 5 build of MikuPan v0.1.3-over-shoulder, now built from the
same repository as the PC releases.

- **Camera collision against what is on screen.** The over-the-shoulder
  camera collides with the room as it is drawn instead of the game's walking
  floor plan: it looks over low furniture instead of zooming in on Miku,
  walking through an open doorway no longer pulls it in, and walls, tall
  furniture and closed doors still stop it.
- **Camera collision readout** in **Debug › Camera › Show Camera World Info**
  (overlay: L3 + R3).

## Updating from v0.1.0

Replace `/data/homebrew/PPSA39390` with the `PPSA39390` folder from this zip.
Everything in `/data/MikuPan/` (game files, settings, saves, converted
textures) stays as it is.

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
