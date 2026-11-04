GoSRecomp - unofficial Windows port of Castlevania: Grimoire of Souls

You need your own copy of the macOS app, version 1.4.2, as a disk image (.dmg).

SETUP
1. Extract this archive into a new, empty folder.
2. Drag your .dmg onto Setup.bat (or run it and paste the path).
   It copies the game data out of the image and converts it for Windows.
   Needs about 3 GB of free space while it runs.
3. Start the game with Play.bat.

To update: extract the new version into a new folder and run Setup.bat
again. Your save is kept (it is stored outside the game folder).

SETTINGS (gosr.ini)
  [game]       language=    empty = your Windows language
  [display]    fps= vsync= mode= width= height=
  [controller] layout=pc    (mac = the macOS tables)
               dpad_move=0  the d-pad no longer moves your character
               xinput=0     if an Xbox controller misbehaves
               standard=2   if another controller's buttons are wrong
  [effects]    flip_fix=0   experimental fix for upside-down effects (off)
  [mouse]      buttons=1    mouse buttons in the keyboard options
  [debug]      menu=1 hotkey=F8
Each setting is explained in gosr.ini.

CONTROLS
Keyboard, mouse and controller bindings are set in the game's own settings.
Mouse buttons show up there as Mouse L / R / M / 4 / 5.

DEBUG MENU (F8)
Testing tools: resources, every item by name, unlocks, quests, summons,
skip the tutorial, invincibility, damage multiplier.
Every action changes your save immediately and cannot be undone, and it
skips the game's progression. Back up your save first:
  %USERPROFILE%\AppData\LocalLow\Konami Digital Entertainment Co_, Ltd_\Castlevania_ Grimoire of Souls

TROUBLESHOOTING
- Setup.bat says "not a GoSRecomp release": you downloaded "Source code".
  Download GoSRecomp-x.y.z.zip from the Releases page instead.
- Setup.bat says the files are not the expected ones: the .dmg must be the
  macOS app, version 1.4.2, unmodified.
- Characters stand in a T-pose in the hub or roster: update your graphics
  driver (an NVIDIA driver update fixed it for players who had it).
- Controller buttons or sticks are wrong:
  * PlayStation and Xbox controllers: press Default in the game's controller
    options once.
  * Other controllers (8BitDo, Switch Pro, ...): use their X-input (Xbox) or
    Switch mode. If buttons are still wrong, set standard=2 in gosr.ini.
  * An Xbox controller misbehaves since 1.0.3: set xinput=0 in gosr.ini.
  * The d-pad moves your character; dpad_move=0 turns that off.
- An effect looks upside down: report it with a crash report (below).
- The game is slow the first time an effect shows: it converts the effect
  once, then keeps it in gosr_spfx_cache.

PROBLEMS
Run "Play with crash log.bat". When the game exits it builds
crash_report.zip: logs, crash dumps, a copy of your save and settings, and
a dxdiag report (Windows version, GPU, drivers). Attach it to your report.

Castlevania is a trademark of Konami. This project is not affiliated with
or endorsed by Konami, Apple or Unity Technologies.
