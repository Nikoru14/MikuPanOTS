## MikuPan PS5 v0.1.2

The PlayStation 5 build of MikuPan v0.1.4-over-shoulder.

- **Gyro aiming on the DualSense** (from MikuPan, with the console's motion
  sensor now read): while the Camera Obscura is raised, turning the
  controller aims it, alongside the stick. **Settings › Controls › Gyro** sets
  the sensitivity, yaw or roll to turn, and the inversions.
- **Gyro recenter behavior** (same page): the right half of the touch pad with
  the Camera Obscura raised holds the gyro still while pressed, so the
  controller can be re-gripped, and then **Zero** leaves the view where it is,
  **Level** tips it back to level, **Level and back to start** also turns it
  back to where Miku faced. The touch pad's left half stays Select.
- **Walking through a camera change no longer fights the over-the-shoulder
  camera.** Holding the stick through a door, a search or an event shot keeps
  Miku walking the way she was, and back on the over-the-shoulder camera the
  direction now eases onto the camera's within a few frames instead of
  sticking until the stick is let go.

## Updating

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
