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
| HDR | Off | The headset picture is made from the normal (SDR) picture |
| Resolution | **2K (2560 × 1440) or higher** | The menu screen in the headset uses it (the 3D picture's size is the VR picture size, see below) |

Also recommended:

| Setting | Value |
|---|---|
| Film grain | 0 (it reads as noise in a headset) |
| Display mode | Windowed or Borderless (both fine) |

**The VR picture size** - the square picture the game draws for each eye while VR runs, whatever your monitor's resolution - is set in the **VR settings panel** (settings menu: hold the four face buttons) or as `render_size` in `gowvr.ini`. Bigger is sharper but slower. While it runs, the game's window on your monitor is a small square (`desktop_window`); the headset still gets the full picture.

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

**Menus:** press all four face buttons together (Meta: A X Y B, PlayStation: ✕ ○ □ △) for the game menu (map, skills, inventory); **hold** them for the settings menu, where the VR settings panel appears next to it. On Valve Index you can also squeeze the trackpads.

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
- The mod creates it the first time the game starts, and never overwrites it.
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
| `render_size` | 2048 | VR picture size per eye (square), 0 = the game's own resolution |
| `turn_snap` / `turn_speed` / `snap_turn_degrees` | 0 / 1.0 / 45 | First-person turning: smooth at a speed, or snap turns |
| `walls_f1` | 1 | 0 = no wall collision in first person |
| `hud_layer` | 1 | 0 = the HUD drawn into the game's picture (less sharp), as before |

---

## What the mod changes on your PC

Nothing in the game is replaced or patched on disk.
- The mod is one file, `version.dll`, next to `GoW.exe`. Windows loads it into the game when it starts.
- Another mod's `version.dll` in the same place would be replaced, so check first if you use other mods.

| File / folder | What it is |
|---|---|
| `version.dll` | The mod |
| `gowvr.ini` | The mod's settings |
| `gowvr_logs\` | The mod's log (`gowvr.log`) and crash reports (`crash_reports\`) |
| `settings.ini` | The game's own settings: only what you change in the game (an older version's launcher changed three of them; the uninstaller puts those back) |

---

## Troubleshooting

**The game runs flat, not in the headset**
- Is SteamVR running and the headset tracking? VR turns on as soon as the game starts; if it did not, press **F9**.
- Open `gowvr_logs\gowvr.log` in the game folder: `xr: setup failed` means SteamVR did not answer. The mod keeps trying every 3 seconds.

**Playing with Virtual Desktop (VDXR), Meta / Oculus or another OpenXR runtime instead of SteamVR**
- The mod uses the active OpenXR runtime; SteamVR is only started when it is that runtime.
- Virtual Desktop: in the Streamer app set the OpenXR runtime to **VDXR**, connect the headset, then start the game.
- To use a runtime for this game only, without changing your PC's default: set the `XR_RUNTIME_JSON` environment variable to that runtime's `.json` file.

**The picture in the headset is blurry or low resolution**
- Raise the **VR picture size** in the VR settings panel (settings menu), for example to 2560 or 2816.
- Blurry menus only? The menu screen uses the game's own resolution: set it to 2K or higher. Monitor smaller than that? Turn on **NVIDIA DSR** (NVIDIA Control Panel → Manage 3D settings → DSR - Factors) or **AMD Virtual Super Resolution** (AMD Software → Display), then pick the higher resolution in the game.
- Virtual Desktop: also raise its **VR Graphics Quality** (e.g. Ultra or Godlike).

**Menus, videos or the HUD look too big**
Edit `gowvr.ini` next to `GoW.exe`, then restart the game:
- `screen_width` - width of the floating screen for menus and videos, in metres (default 3.4; try 2.4).
- `screen_dist` - how far away that screen is, in metres (default 2.2; further away looks smaller).
- `ui_scale` / `ui_scale_fp` - size of the HUD and captions in third / first person (defaults 0.35 / 0.5).

**Kratos' hands don't match your controllers (tilted, turned or twisted)**
In first person press **F5 / F6** to tilt the hands down / up, **Ctrl + F5 / F6** to turn them and **Shift + F5 / F6** to roll (twist) them. The values are saved in `gowvr.ini` for your headset type.

**SteamVR restarted or crashed while playing**
The mod tries to reconnect once SteamVR is back. If it does not, press **F9** twice or restart the game.

**Button icons look wrong**
Press **F10** (Xbox) or **F11** (PlayStation). The icons switch as soon as you press a button or move a stick.

**The game crashed**
- A crash report is saved in `gowvr_logs\crash_reports\crash_<date>.txt`.
- It holds your PC and headset details, the mod's state and where the crash happened. It contains no user name or personal files.
- Please send it along with a few words on what you were doing.
