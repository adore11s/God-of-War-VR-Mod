# God of War VR Mod — Guide

Everything beyond the basics in the [README](README.md).

- [What the mod does](#what-the-mod-does)
- [Game settings](#game-settings)
- [Controls](#controls)
- [Settings file (gowvr.ini)](#settings-file-gowvrini)
- [What the mod changes on your PC](#what-the-mod-changes-on-your-pc)
- [Troubleshooting](#troubleshooting)

---

## What the mod does

- **Real stereo 3D.** Every frame is drawn twice, once per eye, with full head tracking (turning and leaning).
- **A separate picture per eye.** Anti-aliasing keeps its own history for each eye. DLSS does too, but it still makes edges shimmer, so leave it off for now.
- **No depth-of-field blur** while VR is on, so nothing blurs where you look.
- **A sharp HUD.** The interface is drawn at full resolution and fitted into the lenses. While aiming, the reticle shows exactly where the throw goes.
- **Menus, videos and loading screens** show on a big screen floating in front of you.
- **First person.**
  - Kratos' arms, the Leviathan Axe, the Blades of Chaos and the shield follow your controllers.
  - You attack with real swings, punches and throws.
- **First person in cutscenes.**
- **Xbox or PlayStation button icons (F10 / F11).**
  - In PlayStation mode the game sees a PlayStation controller. That is handy with PSVR2 Sense controllers.
  - No extra drivers (such as ViGEmBus) are needed.
- **Rumble.** The game's controller vibration plays on your VR controllers, in both modes.

---

## Game settings

Set these in the game's graphics settings before you play:

| Setting | Set to | Why |
|---|---|---|
| VSync | Off | The headset sets the pace; VSync to your monitor holds VR back |
| Frame rate limit | Off | A cap below the headset's refresh rate makes VR judder |
| Motion blur | 0 | It smears with every head movement |
| DLSS (NVIDIA cards) | Off for now | It makes every edge shimmer in the headset |
| Graphics preset | **Low**, or **Original** if your PC can handle it | VR draws the game twice, once per eye |
| Resolution | **2K (2560 × 1440) or higher** | The headset picture is made from it |

The installer also sets the frame rate limit, motion blur and HDR for you. It records your old values, and the uninstaller puts them back.

Also recommended:

| Setting | Value |
|---|---|
| Film grain | 0 (it reads as noise in a headset) |
| Display mode | Windowed or Borderless (both fine) |

**Never started God of War before?** Its settings file does not exist yet. Leave the installer window open, play, and quit the game; the settings are applied as you exit.

---

## Controls

### Keyboard

These work while the game window or a SteamVR window is in front.

| Key | What it does |
|---|---|
| `F9` | VR on / off |
| `F8` | Recenter your view (SteamVR's own recenter works too) |
| `F10` | Xbox button icons (the game sees an Xbox controller) |
| `F11` | PlayStation button icons (the game sees a PlayStation controller) |
| `F5` / `F6` | Tilt Kratos' wrists down / up. With `Ctrl`: turn them, with `Shift`: roll them. Saved for your headset type |

### VR controllers in third person (like a gamepad)

| Controller | Does |
|---|---|
| Left stick / right stick | Move / camera |
| Left trigger | Aim |
| Right trigger | Heavy attack |
| Right grip | Light attack |
| Left grip | Shield / block |
| Right lower / upper face button | Interact / jump, and dodge |
| Left lower / upper face button | Atreus, and recall the axe |
| Left stick click | Sprint |
| Right stick click | Lock on |
| Right stick click **+ push** | D-pad: left = Blades, right = sheathe, up = arrow type, down = quick turn |
| Both stick clicks together | Spartan Rage |
| Right trigger **next to your head** | First person on / off; **hold** it there for first person in cutscenes |

**Pause menu:** press all four face buttons together (Meta: A X Y B, PlayStation: ✕ ○ □ △). On Valve Index you can also squeeze the trackpads.

### First person: motion controls

| Motion | Does |
|---|---|
| Swing the right controller (axe in hand) | Light attack |
| Swing with the right trigger held, or chop down hard | Heavy attack |
| Hold the right trigger with your hand above your head | Charged heavy attack |
| Wave back and forth fast | Continuous attack |
| Punch with your fingers resting on the grip buttons (no weapon in hand) | Fist attack (touching the grips makes the fists; squeezing the left grip fully is the shield) |
| Swing with the Blades drawn | Blades attack |
| Left grip over your left shoulder | Equip the Blades of Chaos |
| Both hands above your head, then both down fast | Raging Smash / Chaos Slam |
| Left trigger (aim) + a throwing motion | Throw the axe |
| Right grip over your right shoulder | Equip / put away the axe |
| Right grip, or the left upper button (△ / Y), after a throw | Recall the axe |
| Close your left hand (grip) | Shield |
| Hold your left hand next to your head | Show your health, Spartan Rage and runic bars (first person hides them) |

---

## Settings file (gowvr.ini)

`gowvr.ini` sits next to `GoW.exe`.
- Every setting has a short note explaining it.
- Changes you save while the game runs are picked up.
- The installer never overwrites your copy.
- The mod also stores things it learns there, such as the weapon grips. Leave keys you don't know alone.

Most useful settings:

| Setting | Default | Meaning |
|---|---|---|
| `autovr` | 1 | 0 = start flat and press F9 for VR |
| `ui_scale` / `ui_scale_fp` | 0.35 / 0.5 | HUD size in third / first person |
| `pad_mode` | 0 | 0 = Xbox icons, 1 = PlayStation icons (F10 / F11 change it) |
| `rumble_strength` | 1.0 | Controller rumble strength (0 = off) |
| `hide_body` | 0 | 1 = only the arms in first person (F7) |
| `screen_dist` / `screen_width` | 2.2 / 3.4 | Distance and size of the floating screen, in metres |

---

## What the mod changes on your PC

Nothing in the game is replaced or patched on disk.
- The mod is one file, `version.dll`, next to `GoW.exe`. Windows loads it into the game when it starts.
- If another mod's `version.dll` is already there, the installer stops and leaves it alone.

| File / folder | What it is |
|---|---|
| `version.dll` | The mod |
| `gowvr.ini` | The mod's settings |
| `gowvr_logs\` | The mod's log (`gowvr.log`) and crash reports (`crash_reports\`) |
| `settings.ini` | The game's settings: three values changed (see above), restored by the uninstaller |

---

## Troubleshooting

**The game runs flat, not in the headset**
- Is SteamVR running and the headset tracking? VR turns on as soon as the game starts; if it did not, press **F9**.
- Open `gowvr_logs\gowvr.log` in the game folder: `xr: setup failed` means SteamVR did not answer. The mod keeps trying every 3 seconds.

**SteamVR restarted or crashed while playing**
The mod tries to reconnect once SteamVR is back. If it does not, press **F9** twice or restart the game.

**Button icons look wrong**
Press **F10** (Xbox) or **F11** (PlayStation). The icons switch as soon as you press a button or move a stick.

**The game crashed**
- A crash report is saved in `gowvr_logs\crash_reports\crash_<date>.txt`.
- It holds your PC and headset details, the mod's state and where the crash happened. It contains no user name or personal files.
- Please send it along with a few words on what you were doing.
