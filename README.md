# God of War VR Mod

Play **God of War (2018, PC)** in VR. Every frame is drawn in true 3D for each eye with full head tracking, and you can switch to a first-person view through Kratos' eyes and fight with your own hands. Works with the Steam, Epic and GOG versions of the game.

> ⚠️ **Work in progress.** It is playable but not finished; expect rough edges. First person and the motion controls are still being tuned.

## Important: read before playing

- **Remember the two first-person shortcuts.**
  - **First person during gameplay:** press the **right trigger next to your head**.
  - **First person during cutscenes:** **hold** the **right trigger next to your head**.
- **The first-person view sits slightly off Kratos' head, on purpose.** Seen from exactly where his eyes are, his shoulder armour blocks the view, so the camera sits a little in front of and above that spot.
- **Tested only up to the World Serpent scene.** After that you may get crashes or wrong body animations.
- **Your hands feel in the wrong place or don't move right?** Switch from first person to third person and back to first person (press the **right trigger next to your head** twice).
- **You can phase through walls during combat.** Walking backwards into a wall while fighting can push you through it, so keep this in mind.
- **Don't use DLSS for now.** It makes every edge shimmer in the headset. Turn it off in the game's graphics settings.
- **Opening the menu:** press all **four face buttons** together on your VR controllers (Meta: **A X Y B**, PlayStation: **✕ ○ □ △**).
- **VR settings:** in the game's settings menu, a **VR settings panel** appears to the right of the menu screen: turning (smooth or snap), controller buttons, wall collision and the VR picture size. Point a controller at an option and pull its trigger.
- **Your monitor shows a small square window while VR runs.** That is on purpose: the headset still gets the full picture.
- **Equipping the axe:** reach over your **right shoulder** and press the **grip button**. After throwing the axe, recall it with the **right grip button** or the **left controller's upper button** (PlayStation: **△**, Meta: **Y**).
- **Fist combat needs your fingers resting on the grip buttons.** Just touching the grips turns the fists on. That is because fully squeezing the left grip brings out the shield.

## What the mod does

- **Native stereo rendering.** The game is drawn separately for each eye, with full head tracking.
- **First person** in normal gameplay and in cutscenes. You can see your whole body.
- **Motion controls in first person:**
  - **Fists:** light attacks. Rest your fingers on the grip buttons to make fists.
  - **Leviathan Axe:** light attacks, throwing and recalling. Equip it by reaching over your right shoulder.
  - **Shield:** press the left grip to defend yourself.
  - **Spartan Rage:** light attacks; move your hands from up to down for the heavy attack.
  - **Blades of Chaos:** light attacks (more attacks are on the way). Equip them by reaching over your left shoulder.
- **Finger curling.** Hover your fingers over the grip buttons and Kratos' fingers curl.
- **8D audio** in first person: sounds follow your head.
- **You can feel walls.** Touching a wall makes your controllers vibrate, and your hands and head can't go through walls.
- **Xbox or PlayStation (PSVR2) controls**, with matching button icons in the game (set in the VR settings panel).
- **Smooth or snap turning** in first person, set in the VR settings panel.
- **A square VR picture** in the size you choose, whatever your monitor's resolution.
- **Rumble.** The game's controller vibration plays on your VR controllers.

## Before you install

Start the game normally once and, in its graphics settings:

- turn **VSync** off,
- turn the **frame rate limit** off,
- set **Motion blur** to **0**,
- turn **DLSS** off (it makes edges shimmer in VR),
- turn **HDR** off,

**Graphics preset:** Play on the **Low** preset. If your PC can handle it, try the **Original** preset.

## Install

1. Open your God of War folder. In Steam: right-click the game → **Manage** → **Browse local files**. In the Epic Games Launcher: the game's **⋯** menu → **Manage** → the folder icon. In GOG Galaxy: the game's settings icon → **Manage installation** → **Show folder**.
2. Copy this whole **`GOW_VR_MOD`** folder into it.
3. Double-click **`Install VR mod.cmd`** in the `GOW_VR_MOD` folder. You only do this **once** (and again after downloading a new version).
4. From now on, put your headset on and start the game as usual, from Steam, Epic, GOG or `GoW.exe`.

VR turns on by itself as soon as the game starts, and SteamVR starts with it. Using Virtual Desktop (**VDXR**) or another OpenXR runtime instead? Connect your headset to it first. The mod's settings file (`gowvr.ini`) is created by itself the first time.

## Controls

| Button / key | What it does |
|---|---|
| **Right trigger next to your head** | First person on / off |
| **Hold the right trigger next to your head** | First person in cutscenes |
| **All four face buttons** | Game menu |
| **F9** | VR off / on (play on your monitor, press again to go back to VR) |

➡️ All controls, motion gestures, settings and fixes: **[GUIDE.md](GUIDE.md)**

## Uninstall

Double-click **`Uninstall VR mod.cmd`** in the `GOW_VR_MOD` folder. It moves the mod's own files to the **Recycle Bin** (nothing is deleted for good). **Your saves and the game's files are never touched.**

## Something went wrong?

Open an issue on the mod's GitHub page (**Issues** → **New issue**) with a few words on what you were doing, and attach:

| What happened | Send |
|---|---|
| **The game crashed** | The newest **`crash_<date>.txt`** from **`gowvr_logs\crash_reports`** in your game folder |
| **Something looks or works wrong, the game froze, or VR did not start** | **`gowvr_logs\gowvr.log`** from your game folder, right after quitting the game (if you have started the game again since, send **`gowvr.prev.log`** too) |
| **The install or uninstall window showed an error** | A screenshot of that window |

The mod writes these files itself while the game runs (`gowvr.log` is started fresh each time; the run before is kept as `gowvr.prev.log`).

None of these files contain your user name or other personal information.

## Known bugs (fixes coming soon)

- **Some of the interface is missing in first person** (the health and Spartan Rage bars).
- **The Leviathan Axe's frost icon when aiming and Atreus' aiming icon are broken.**
- **Body glitches:** your hands can end up in the wrong place (see *Important: read before playing* for the workaround).
- **Phasing through walls during combat** (see *Important: read before playing*).
- **Combat isn't perfect yet.** Kratos sometimes teleports a little during attacks, and the fists' hit area is too big: you can punch enemies from about 2 metres away.
- **DLSS makes edges shimmer** (see *Important: read before playing*).
## Open source

The mod is free and open source under the **MIT license**. The full source code is in the [`source`](source) folder.
Anyone can fix, improve or build on it: see **[CONTRIBUTING.md](CONTRIBUTING.md)**.

*Not affiliated with Sony Interactive Entertainment or Santa Monica Studio. No game files are included.*
