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
- **A separate picture per eye.** Anti-aliasing and DLSS keep their own history and motion for each eye, so edges stay sharp and stable. DLSS is recommended.
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

`Install VR mod.cmd` sets these for you (and asks about DLSS and its preset); if it could not, it lists what to set yourself in the game's graphics settings:

| Setting | Set to | Why |
|---|---|---|
| VSync | Off | The headset sets the pace; VSync to your monitor holds VR back |
| Frame rate limit | Off | A cap below the headset's refresh rate makes VR judder |
| Motion blur | 0 | It smears with every head movement |
| DLSS (NVIDIA cards) | **On** | Recommended: sharp, stable edges and better performance |
| NVIDIA Reflex Low Latency (with DLSS) | On + Boost, latency markers and latency flash on | Less delay between your input and the picture |
| HDR | Off | The headset picture is made from the normal (SDR) picture |

Also recommended:

| Setting | Value |
|---|---|
| Film grain | 0 (it reads as noise in a headset) |
| Display mode | **Windowed** (needed for the square VR picture and the small window; set by the installer) |

**The VR picture size** - the square picture the game draws for each eye while VR runs, whatever your monitor's resolution - is set in the **VR settings panel** (the game's settings menu) or as `render_size` in `gowvr.ini`. By default it is **Auto**: the size your headset asks for (SteamVR: its resolution setting; Virtual Desktop: its VR Graphics Quality), so every headset gets a sharp picture without changing anything. Pick a fixed size instead if the game runs too slowly; bigger is sharper but slower. While it runs, the game's window on your monitor is a small square (`desktop_window`); the headset still gets the full picture.

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
| Right stick click **+ push** | D-pad: left = Blades, right = sheathe, up = Atreus' arrow type, down = quick turn. For up / down you can also push first and then click |
| Both stick clicks together | Spartan Rage |
| ○ (PS VR2) / B (Meta, Index) **next to your head** | First person on / off - for gameplay while walking, for cutscenes during a cutscene |

**Menus:** tap all four face buttons together (Meta: A X Y B, PlayStation: ✕ ○ □ △) for Options / ☰ (the three-lines button); hold them for the game menu (map, skills, inventory). On Meta Quest the left controller's ☰ button is Options / ☰ too. In the game's settings menu the VR settings panel appears next to it. On Valve Index you can also squeeze the trackpads.

**Arrow buttons in menus:** on the pause menu, settings and other menu screens, pushing a stick also presses the arrow (D-pad) that way - the right stick in all four directions, the left stick left / right. This works on every VR controller.

### First person: motion controls

| Motion | Does |
|---|---|
| Swing the right controller (axe in hand) | Light attack |
| Swing with the right trigger held, or chop down hard | Heavy attack |
| Hold the right trigger with your hand above your head | Charged heavy attack |
| Wave back and forth fast | Continuous attack |
| Punch with your fingers resting on the grip buttons (no weapon in hand) | Fist attack (touching the grips makes the fists; squeezing the left grip fully is the shield) |
| Swing with the Blades drawn | Blades attack |
| Either hand's grip over your left shoulder | Equip / put away the Blades of Chaos |
| Both hands above your head, then both down fast | Raging Smash / Chaos Slam |
| Left trigger (aim) + a throwing motion | Throw the axe |
| Either hand's grip over your right shoulder | Equip / put away the axe |
| Right grip, or the left upper button (△ / Y), after a throw | Recall the axe |
| Close your left hand (grip) | Shield |
| Hold your left hand next to or over your head (grip released) | Show your health, Spartan Rage and runic bars, bigger and nearer the middle. Works best in PlayStation button mode (F11) |

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
| `ui_size` | 0.75 | Overall HUD size on top of those (1 = the older, bigger HUD). The shop's screens keep their size |
| `ui_same_angle` / `ui_ref_tan` | 1 / 2.3 | The HUD looks the same size on every headset (as on a PS VR2). 0 = a share of the picture, as before (small on narrower headsets) |
| `pad_mode` | 0 | 0 = Xbox icons, 1 = PlayStation icons (F10 / F11 change it) |
| `rumble_strength` | 1.0 | Controller rumble strength (0 = off) |
| `screen_dist` / `screen_width` | 2.2 / 3.4 | Distance and size of the floating screen, in metres |
| `render_size` | 2048 | VR picture size per eye (square) when `render_size_auto` is 0; 0 = the game's own resolution |
| `render_size_auto` | 1 | The picture size matches what the headset asks for ("Auto" in the VR settings panel). 0 = use `render_size` |
| `turn_snap` / `turn_speed` / `snap_turn_degrees` | 0 / 1.0 / 45 | First-person turning: smooth at a speed, or snap turns |
| `walls_f1` | 1 | 0 = no wall collision in first person |
| `finisher_third_person` | 1 | Finishers play in third person (the game's own camera). 0 = in first person |
| `first_person_walking` / `first_person_cutscenes` | 1 / 0 | First person while walking / in cutscenes when the game starts (after your save has loaded). Switching in game saves them |
| `hud_layer` | 1 | 0 = the HUD drawn into the game's picture (less sharp), as before |
| `hud_layer_min_eye_px` | 2048 | The HUD layer stays sharp when the headset asks for small eye images (counted as at least this many pixels across) |
| `xr_loader` | 1 | 0 = start the VR runtime directly instead of through the official OpenXR loader (only if VR stopped starting) |
| `xr_dither` | 1 | 0 = no dithering of the picture into the headset (dark gradients may band) |
| `afr_rendering` | 0 | 1 = AFR: one eye per frame, the eyes alternating (faster; "Rendering Method" in the VR settings panel) |
| `motion_controls` | 1 | First person: 0 = no motion controls - the controllers work like a gamepad, attacks play their normal animations ("Motion controls" in the VR settings panel) |
| `aim_key` | 162 | The keyboard key that aims (Windows key code; 162 = Left Ctrl), for head-based aiming with a keyboard |
| `hud_layer_head_aligned` | 1 | The menus / HUD straight ahead of both eyes (fixes double menus on Pimax). 0 = per eye, as before |
| `lod_scale` | 1 | Distant model detail. Lower (e.g. 0.6) = finer models farther out, more GPU load |
| `parallel_eyes` | 1 | Headsets with outward-turned displays (Pimax): both eyes are drawn parallel, like Pimax's "Parallel Projections" (fixes double vision). 2 = always, 0 = off |
| `hud_bars_hand` | 1 | First person: the health / Rage / runic bars always shown above your left hand; 0 = at your head, shown when you hold your left hand next to it ("Health bars" in the VR settings panel: Head / Left hand). `hand_bars_width` (0.24 m) and `hand_bars_up` (0.10 m) set their size and height |
| `aim_icon_hide` | 0 | 1 = Kratos' aiming icon (axe / Blades) is hidden; Atreus' bow crosshair still shows |
| `aim_toggle` | 0 | 1 = one pull of the left trigger starts aiming, the next pull stops it ("Aiming mode" in the VR settings panel: Hold / Press). Menus, loading screens and cutscenes end the aim |
| `hud_peek_zoom` / `hud_peek_pull` | 1.3 / 0.35 | First person, left hand at your head: how much bigger the health / Rage / runic bars get, and how far they move from the corners toward the middle (0 = not at all). `hud_peek_move` 0 leaves them in the corners |
| `hud_obj_right` / `hud_obj_down` | 0.07 / 0.07 | First person: how far the objective (top left of the HUD) moves toward the middle, as a share of the HUD's width / height. `hud_obj_move` 0 leaves it in the corner |
| `low_health_vignette` | 1 | At 20 % health or less (`low_health_frac` 0.2), red edges around your whole view in the headset (the game's low-health warning, made visible in VR). 0 = off; `low_health_alpha` (0.5) sets how strong, `low_health_inner_deg` / `low_health_outer_deg` (16 / 40) where it starts / is strongest; `low_health_test` 1 shows it at any health to try it |
| `f1_combat_hud` | 2 | The health / Rage / runic bars in first person: 2 = hidden until your left hand is at your head (the game's "Touch"), 0 = always hidden, 1 = always shown. Third person keeps your own setting from the game's menu |
| `hand_smoothing` | 1 | Smooths small hand jitter in first person and the VR settings panel's pointer (fast moves are not slowed). 0 = off |
| `fist_on_y` | 1 | Controllers without finger sensing (Meta Quest): hold Y for fists. 0 = a light squeeze of the right grip |
| `hit_reaction_freeze` | 1 | First person: Kratos' body and your view stay still when he is hit. 0 = off |
| `dlss_fixes_auto` | 1 | The DLSS stereo fixes follow the game's DLSS setting. 0 = always on |
| `hold_bad_poses` | 1 | When the headset loses tracking for a moment (more common over Virtual Desktop or Link), the view holds still instead of jumping. 0 = off |
| `crop_to_eye_fov` | 2 | Sends each eye's picture in exactly the shape the headset asks for (fixes doubled pictures on Quest Link and Pimax). 2 = on every runtime except SteamVR, and on SteamVR with a Pimax; 1 = always, 0 = never |

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
- Set the **VR picture size** in the VR settings panel (settings menu) to **Auto** (one step above the largest size).
- Auto follows your runtime's resolution: raise SteamVR's resolution setting, or Virtual Desktop's **VR Graphics Quality** (e.g. Ultra or Godlike). Lower it if the game runs too slowly.

**Menus, videos or the HUD look too big**
Edit `gowvr.ini` next to `GoW.exe`, then restart the game:
- `screen_width` - width of the floating screen for menus and videos, in metres (default 3.4; try 2.4).
- `screen_dist` - how far away that screen is, in metres (default 2.2; further away looks smaller).
- `ui_scale` / `ui_scale_fp` - size of the HUD and captions in third / first person (defaults 0.35 / 0.5), times `ui_size` (0.75; not the shop's screens).

**Kratos' hands don't match your controllers (tilted, turned or twisted)**
Use **Hand angle** in the VR settings panel (in the game's settings menu): **−** / **+** tilt the hands and the axe down / up, 5° a click. Or, in first person, press **F5 / F6** to tilt the hands down / up, **Ctrl + F5 / F6** to turn them and **Shift + F5 / F6** to roll (twist) them. The values are saved in `gowvr.ini` for your headset type.

**SteamVR restarted or crashed while playing**
The mod tries to reconnect once SteamVR is back. If it does not, press **F9** twice or restart the game.

**Button icons look wrong**
Press **F10** (Xbox) or **F11** (PlayStation). The icons switch as soon as you press a button or move a stick.

**The game crashed**
- A crash report is saved in `gowvr_logs\crash_reports\crash_<date>.txt`.
- It holds your PC and headset details, the mod's state and where the crash happened. It contains no user name or personal files.
- Please send it along with a few words on what you were doing.
