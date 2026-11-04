# GoSRecomp 1.0.1

- Xbox controllers: LT and RT work again (dodge), in game and when rebinding.
  The d-pad on Xbox pads reads correctly too.
- New option: `dpad_move=1` under `[controller]` in `gosr.ini` lets the d-pad
  move your character.
- The log now says whether the game found your controller through Unity's
  Input System, to help with controller reports.

Update: extract into a new folder and run `Setup.bat` again, your save is kept.
Download `GoSRecomp-1.0.1.zip` below (not "Source code").

# GoSRecomp 1.0.0

First release. Castlevania: Grimoire of Souls running natively on Windows.

**You need your own copy of the macOS app, version 1.4.2, as a `.dmg`.** There's
no game data in here; `Setup.bat` takes it from your image.

## Installing

1. Download `GoSRecomp-1.0.0.zip` below (not "Source code") and extract it
   into a new, empty folder.
2. Drag your `.dmg` onto `Setup.bat`.
3. Start the game with `Play.bat`.

## What's in it

- Fully offline, no account
- Direct3D 11 with the post-processing and the particle effects
- Xbox and PlayStation controllers, keyboard and mouse (mouse buttons can be bound)
- Frame rate cap, V-Sync, window mode and resolution in `gosr.ini`
- Every language of the game
- Debug menu on F8 (it changes your save for good, back it up first)

## Known issues

- An effect can show up a few frames late the first time you see it.
- The game was made for 60 fps.

Bugs: start with `Play with crash log.bat` and attach the `crash_report.zip` it
makes to an issue.
