# God of War VR Mod

Play **God of War (2018, PC)** in VR. Every frame is drawn in true 3D for each eye with full head tracking, and you can switch to a first-person view through Kratos' eyes and fight with your own hands. Works with the Steam, Epic and GOG versions of the game.

> ⚠️ **Work in progress.** It is playable but not finished; expect rough edges. First person and the motion controls are still being tuned.

## Important: read before playing

- **Only playing in third person?** Skip the first-person points below. Your controllers work like a gamepad.
- **Game settings:** no need to change them. The installer does it.
- **First person during gameplay:** press the **right trigger next to your head**.
- **First person during cutscenes:** **hold** the **right trigger next to your head**.
- **Tested:** only the first 2 hours of the game. Later parts may break or crash.
- **Hands wrong or button prompt missing:** switch to third person and back (**right trigger next to your head** twice).
- **Axe wrong in first person:** swing it a few times in third person, then switch back. Or put it away, switch to third person and back.
- **Menus:** **tap** all **four face buttons** for Options / ☰. **Hold** them for the game menu. Meta Quest: the left **☰** button works too.
- **VR settings:** in the game's settings menu, a panel appears on the right. Point a controller and pull its trigger.
- **Axe:** reach over your **right shoulder** with either hand and press **grip**. Throw: **hold the left trigger** and throw. Recall: **right grip** or **△ / Y**.
- **Blades of Chaos:** reach over your **left shoulder** with either hand and press **grip**.
- **Finishers:** face the enemy from the front. Pick first or third person in the VR settings panel.
- **Fists:** rest your fingers on the grips. Squeezing the left grip fully is the shield. Meta Quest: **hold Y**.

## What the mod does

- **Native stereo rendering.** The game is drawn separately for each eye, with full head tracking.
- **DLSS in VR.** Each eye keeps its own DLSS history and motion, so edges stay sharp and stable.
- **First person** in normal gameplay and in cutscenes. You can see your whole body.
- **Motion controls in first person:**
  - **Fists:** light attacks. Rest your fingers on the grip buttons to make fists (Meta Quest: hold **Y**).
  - **Leviathan Axe:** light attacks, throwing and recalling. Equip it by reaching over your right shoulder with either hand.
  - **Shield:** press the left grip to defend yourself.
  - **Spartan Rage:** light attacks; move your hands from up to down for the heavy attack.
  - **Blades of Chaos:** light attacks (more attacks are on the way). Equip them by reaching over your left shoulder with either hand.
- **Finger curling.** Hover your fingers over the grip buttons and Kratos' fingers curl.
- **8D audio** in first person: sounds follow your head.
- **You can feel walls.** Touching a wall makes your controllers vibrate, and your hands and head can't go through walls.
- **Xbox or PlayStation (PSVR2) controls**, with matching button icons in the game (set in the VR settings panel).
- **Smooth or snap turning** in first person, set in the VR settings panel.
- **Finishers in first or third person**, set in the VR settings panel.
- **A square VR picture** that matches your headset's resolution automatically (or a size you choose), whatever your monitor's resolution.
- **Rumble.** The game's controller vibration plays on your VR controllers.

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
| **Tap all four face buttons** | Options / ☰ |
| **Hold all four face buttons** | Game menu (map, skills, inventory) |
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
- **Body glitches:** your hands can end up in the wrong place (see *Important: read before playing* for the workaround).
- **Combat isn't perfect yet.** Kratos sometimes teleports a little during attacks.
- **VDXR (Virtual Desktop) has rendering issues**, such as flickering lights. Try AFR rendering (VR settings panel: Rendering Method).
- **Button prompts can go missing in first person** (see *Important: read before playing* for the workaround).

## Open source

The mod is free and open source under the **MIT license**. The full source code is in the [`source`](source) folder.
Anyone can fix, improve or build on it: see **[CONTRIBUTING.md](CONTRIBUTING.md)**.
The mod includes the official OpenXR loader and JsonCpp: see **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**.

*Not affiliated with Sony Interactive Entertainment or Santa Monica Studio. No game files are included.*
