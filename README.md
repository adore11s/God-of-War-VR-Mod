# God of War VR Mod

Play **God of War (2018, PC)** in VR. Every frame is drawn in true 3D for each eye with full head tracking, and you can switch to a first-person view through Kratos' eyes and fight with your own hands. Works with the Steam, Epic and GOG versions of the game.

> ⚠️ **Work in progress.** It is playable but not finished; expect rough edges. First person and the motion controls are still being tuned. **Tested:** only the first 2 hours of the game. Later parts may break or crash.

## What the mod does

**In VR**

- **True 3D for each eye.** The game is drawn separately for each eye, with full head tracking.
- **Rendering method:** AFR (one eye per frame, the default), Native (both eyes every frame) or Mono (one picture for both eyes, the fastest), set in the VR settings panel.
- **DLSS in VR.** Each eye keeps its own DLSS history and motion, so edges stay sharp and stable.
- **A square VR picture** that matches your headset's resolution automatically (or a size you choose), whatever your monitor's resolution.
- **The window on your monitor** is 1280 x 720 while VR runs. Drag its edges to resize it; it stays 16:9. Your normal window comes back when VR ends.
- **Game settings:** no need to change them. The installer does it.
- **VR settings:** in the game's settings menu, a panel appears on the right. Point a controller and pull its trigger.
- **Menus:** tap all **four face buttons** for Options / ☰ (Meta Quest: the left **☰** button works too). In menus the sticks also work as the arrow buttons (D-pad).
- **Xbox or PlayStation (PS VR2) controls**, with matching button icons in the game (VR settings panel).
- **Head-based aiming with any controller:** aim (VR left trigger, gamepad L2 or Left Ctrl on the keyboard) and the throw goes where you look.
- **Rumble.** The game's controller vibration plays on your VR controllers.

**First person** (only playing in third person? Skip this part: your controllers work like a gamepad)

- **First person on / off:** hold your right hand next to your head and press **○ (PS VR2) / B (Meta Quest, Index)**. Press it while walking for gameplay, or during a cutscene for cutscenes. By default it is on for gameplay and off for cutscenes (also in the VR settings panel). You can see your whole body.
- **Motion controls:**
  - **Fists:** light attacks.
  - **Leviathan Axe:** light attacks, throwing and recalling (**right grip** or **△ / Y**).
  - **Shield:** squeeze the left grip fully.
  - **Spartan Rage:** light attacks; move your hands from up to down for the heavy attack.
  - **Blades of Chaos:** light attacks (more attacks are on the way).
- **Finger detection.** Hover your fingers over the grip buttons and Kratos' fingers curl.
- **Health and Spartan Rage bars** above your **left hand** (while the game shows them).
- **3D audio:** sounds follow your head.
- **You can feel walls.** Touching a wall makes your controllers vibrate, and your hands and head can't go through walls.
- **Finishers** play in third person; first person can be picked in the VR settings panel.
- **In the VR settings panel:** smooth or snap turning; motion controls on or off (off: the controllers work like a gamepad); axe weight (the axe trails your hand like a heavy weapon); manual climbing (grip walls, ledges, ropes and chains and pull yourself along); manual rowing (row the boat with the paddle in both hands); compass Always or Head (Head: only while your left hand is at the left side of your head).

## Install

1. Open your God of War folder. In Steam: right-click the game → **Manage** → **Browse local files**. In the Epic Games Launcher: the game's **⋯** menu → **Manage** → the folder icon. In GOG Galaxy: the game's settings icon → **Manage installation** → **Show folder**.
2. Copy this whole **`GOW_VR_MOD`** folder into it.
3. Double-click **`Install VR mod.cmd`** in the `GOW_VR_MOD` folder. You only do this **once** (and again after downloading a new version).
4. From now on, put your headset on and start the game as usual, from Steam, Epic, GOG or `GoW.exe`.

VR turns on by itself as soon as the game starts, and SteamVR starts with it. Using Virtual Desktop (**VDXR**) or another OpenXR runtime instead? Connect your headset to it first. The mod's settings file (`gowvr.ini`) is created by itself the first time; every setting is explained in **[GUIDE.md](GUIDE.md)**.

## Controls

| Button | What it does |
|---|---|
| **○ (PS VR2) / B (Meta Quest, Index) next to your head** | First person on / off - press it while walking for gameplay, during a cutscene for cutscenes |
| **Tap all four face buttons** | Options / ☰ |

**First person with motion controls**

| Action | How |
|---|---|
| **Equipping the axe** | Reach over your right shoulder and squeeze the grip |
| **Equipping the Blades** | Reach over your left shoulder and squeeze the grip |
| **Throwing the axe / Blades** | Hold the left trigger (Quest / Index) or L2 (PS VR2), then throw |
| **Fist combat** | PS VR2 / Index: rest your fingers on the grips. Meta Quest: press Y |

## Uninstall

Double-click **`Uninstall VR mod.cmd`** in the `GOW_VR_MOD` folder. It moves the mod's own files to the **Recycle Bin** (nothing is deleted for good). **Your saves and the game's files are never touched.**

## Something went wrong?

Open an issue on the mod's GitHub page (**Issues** → **New issue**) with a few words on what you were doing, and attach:

| What happened | Send |
|---|---|
| **The game crashed** | The newest **`crash_<date>.txt`** from **`gowvr_logs\crash_reports`** in your game folder |
| **Something looks or works wrong, the game froze, or VR did not start** | **`gowvr_logs\gowvr.log`** from your game folder, right after quitting the game (if you have started the game again since, send **`gowvr.prev.log`** too) |
| **The install or uninstall window showed an error** | A screenshot of that window |
| **The bug happens at a certain place in the game** | A copy of your save (see below) |

The mod writes these files itself while the game runs (`gowvr.log` is started fresh each time; the run before is kept as `gowvr.prev.log`).

None of these files contain your user name or other personal information.

**Sending your save:** close the game, then double-click **`Copy my save.cmd`** in the `GOW_VR_MOD` folder. It puts a zip named **`GoW_VR_save_<date>.zip`** on your Desktop. Attach that zip to your issue. It works for the Steam, Epic and GOG copies of the game. **Your saves are only read, never changed, moved or deleted.** The tool checks that each copy matches the original exactly, and it won't run while the game is open. The zip holds only the game's save file (`game.sav`), with no account name or ID.

<details><summary>Copying the save by hand</summary>

All three stores keep the save in the same place: **`%USERPROFILE%\Saved Games\God of War`** (paste that into the Explorer address bar). Inside is a folder named with a string of numbers or letters (your store account). Copy the **`game.sav`** file from it, with the game closed. Copy it, don't move it.
</details>

## Known bugs (fixes coming soon)

- **Combat isn't perfect yet.** Kratos sometimes teleports a little during attacks.
- **Native rendering: light rays sometimes shoot out of flames or lights.** For now use AFR (VR settings panel: Rendering method; AFR is the default).

## Open source

The mod is free and open source under the **MIT license**. The full source code is in the [`source`](source) folder.
Anyone can fix, improve or build on it: see **[CONTRIBUTING.md](CONTRIBUTING.md)**.
The mod includes the official OpenXR loader and JsonCpp: see **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**.

## Support the mod ☕

The mod is free for everyone, and it always will be. Donating is never required. If you enjoy it and want to help me make more VR mods, any support on **[Ko-fi](https://ko-fi.com/frenchiecore)** is very much appreciated. Thank you!

*Not affiliated with Sony Interactive Entertainment or Santa Monica Studio. No game files are included.*
