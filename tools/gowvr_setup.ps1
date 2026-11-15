# ===========================================================================
#  GOD OF WAR - VR MOD : installer / uninstaller
#
#  Called by the .cmd files in the GOW_VR_MOD folder:
#    "Install VR mod.cmd"    ->  -Mode Install    (once: version.dll next to GoW.exe)
#    "Uninstall VR mod.cmd"  ->  -Mode Uninstall  (the mod's own files to the Recycle Bin)
#    "Copy my save.cmd"      ->  -Mode Save       (a COPY of the player's saves as a zip on the Desktop, to attach to a
#                                                  bug report; the saves themselves are only read, never changed)
#
#  The mod is a single file, version.dll, next to GoW.exe. Windows loads it into the game whichever way the game
#  is started (Steam, Epic, GOG, the exe) - nothing is injected and no game file is replaced or patched on disk. The
#  mod writes its own gowvr.ini and gowvr_logs folder next to GoW.exe.
#
#  The uninstaller NEVER deletes anything for good: everything it removes goes to the Recycle Bin, and only from a
#  fixed list of the mod's own files - never an .exe, never a folder holding one, never the game folder itself or
#  anything outside it (an older uninstaller deleted the folder it ran from; with the mod's files copied straight into
#  the game folder that was the game).
#
#  Kept ASCII only: Windows PowerShell 5.1 reads a script without a BOM in the ANSI code page.
# ===========================================================================
param([ValidateSet('Install', 'Uninstall', 'Save')][string]$Mode = 'Install')

$ErrorActionPreference = 'Stop'
$ModDir = Split-Path -Parent $PSScriptRoot
$Files = Join-Path $ModDir 'files'
$StateGame = Join-Path $ModDir 'game_dir.txt'           # where the mod was installed (for the uninstaller)
$StateSettings = Join-Path $ModDir 'settings_changed.txt' # settings.ini values an older launcher changed: KEY=original
# the values that older launcher set (put back by the uninstaller only while they are still these)
$Wanted = [ordered]@{ 'FPSLimit' = 'Off'; 'MotionBlur' = '0'; 'HDR' = 'Off' }

# Owner 2026-11-15: as few key presses as possible. Choices take ONE key (no Enter); the window closes by itself after a
# clean run; it waits for a key only when there is something the player must read (a warning, an error, settings to
# set by hand).
$script:NeedRead = $false
function Say([string]$t) { Write-Host "   $t" }
function Warn([string]$t) { $script:NeedRead = $true; Write-Host "   [!] $t" -ForegroundColor Yellow }
function Bad([string]$t) { $script:NeedRead = $true; Write-Host "   [X] $t" -ForegroundColor Red }

# One key from $allowed (upper case), shown after the prompt. Other keys are ignored. Without a console (redirected
# input) it falls back to a typed answer + Enter.
function Read-Key([string]$prompt, [string[]]$allowed) {
    Write-Host "   $prompt " -NoNewline
    try {
        while ($true) {
            $c = ([string]([Console]::ReadKey($true).KeyChar)).ToUpper()
            if ($allowed -contains $c) { Write-Host $c; return $c }
        }
    } catch {
        while ($true) {
            $c = ([string](Read-Host)).Trim().ToUpper()
            if ($c.Length -gt 0 -and $allowed -contains $c.Substring(0, 1)) { return $c.Substring(0, 1) }
        }
    }
}

function Done([int]$code) {
    Write-Host ''
    if ($code -ne 0 -or $script:NeedRead) {
        Write-Host '   Press any key to close'
        try { [void][Console]::ReadKey($true) } catch { [void](Read-Host) }
        exit $code
    }
    try {
        for ($s = 10; $s -gt 0; $s--) {
            Write-Host "`r   Closing in $s s - press any key to close now " -NoNewline
            for ($i = 0; $i -lt 10; $i++) {
                if ([Console]::KeyAvailable) { [void][Console]::ReadKey($true); Write-Host ''; exit $code }
                Start-Sleep -Milliseconds 100
            }
        }
        Write-Host ''
    } catch { Start-Sleep -Seconds 10 }
    exit $code
}

function Banner([string]$title) {
    Write-Host ''
    Write-Host '  ================================================================'
    Write-Host "    $title"
    Write-Host '  ================================================================'
    Write-Host ''
}

# ---- the game folder ---------------------------------------------------------------------------------------------
function Test-GameDir([string]$d) { return $d -and (Test-Path -LiteralPath (Join-Path $d 'GoW.exe')) }

function Find-GameDir {
    # 0. the mod's files were copied straight into the game folder: that folder (never another install)
    if (Test-GameDir $ModDir) { return (Resolve-Path -LiteralPath $ModDir).Path }
    # 1. this GOW_VR_MOD folder sits inside the game folder (the normal way, see README)
    $parent = Split-Path -Parent $ModDir
    if (Test-GameDir $parent) { return (Resolve-Path -LiteralPath $parent).Path }
    # 2. where the last install went
    if (Test-Path -LiteralPath $StateGame) {
        $d = (Get-Content -LiteralPath $StateGame -Raw).Trim()
        if (Test-GameDir $d) { return (Resolve-Path -LiteralPath $d).Path }
    }
    # 3. Steam libraries
    $cands = @()
    try {
        $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
        if ($steam) {
            $libs = @($steam)
            $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
            if (Test-Path -LiteralPath $vdf) {
                foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                    $libs += ($m.Groups[1].Value -replace '\\\\', '\')
                }
            }
            foreach ($l in ($libs | Select-Object -Unique)) { $cands += (Join-Path $l 'steamapps\common\God of War') }
        }
    } catch { }
    # 4. Epic Games Launcher installs
    $man = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
    if (Test-Path -LiteralPath $man) {
        foreach ($f in Get-ChildItem -LiteralPath $man -Filter '*.item' -ErrorAction SilentlyContinue) {
            try { $j = Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json } catch { continue }
            if ($j.InstallLocation) { $cands += $j.InstallLocation }
        }
    }
    foreach ($c in $cands) { if (Test-GameDir $c) { return (Resolve-Path -LiteralPath $c).Path } }
    # 5. ask
    Warn 'Could not find God of War by itself.'
    Say 'Easiest fix: put this whole GOW_VR_MOD folder INTO your God of War folder'
    Say '(the one that contains GoW.exe) and run this again.'
    Write-Host ''
    $d = (Read-Host '   Or paste the God of War folder here and press Enter').Trim().Trim('"')
    if (Test-GameDir $d) { return (Resolve-Path -LiteralPath $d).Path }
    return $null
}

function Test-GoWRunning { return [bool](Get-Process -Name 'GoW' -ErrorAction SilentlyContinue) }

# ---- version.dll -------------------------------------------------------------------------------------------------
# Ours = a gowvr build (its log line "gowvr <version> linked ..." is in the file). Anything else named version.dll
# belongs to another mod / tool and is never overwritten or removed.
function Test-OurDll([string]$path) {
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
    return $text.Contains('gowvr ') -and $text.Contains('linked')
}

# ---- the Recycle Bin, with every check ---------------------------------------------------------------------------
Add-Type -AssemblyName Microsoft.VisualBasic

# Sends `path` to the Recycle Bin - never deletes for good. Refuses (and says so): anything outside the game folder,
# the game folder itself, an .exe, a folder that holds an .exe anywhere inside it.
function Send-ToRecycleBin([string]$path, [string]$game) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $full = [IO.Path]::GetFullPath($path).TrimEnd('\')
    $g = [IO.Path]::GetFullPath($game).TrimEnd('\')
    if ($full -ieq $g -or -not $full.StartsWith($g + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Warn "Not removed (not a file inside the game folder): $full"
        return $false
    }
    if (Test-Path -LiteralPath $full -PathType Leaf) {
        if ([IO.Path]::GetExtension($full) -ieq '.exe') { Warn "Not removed (a program file): $full"; return $false }
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($full, 'OnlyErrorDialogs', 'SendToRecycleBin')
    } else {
        $exe = Get-ChildItem -LiteralPath $full -Recurse -Force -File -Filter '*.exe' -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($exe) { Warn "Not removed (it holds a program file, $($exe.Name)): $full"; return $false }
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($full, 'OnlyErrorDialogs', 'SendToRecycleBin')
    }
    return $true
}

# ---- settings.ini (only what an older launcher changed) ----------------------------------------------------------
function Read-Recorded {
    $rec = [ordered]@{}
    if (Test-Path -LiteralPath $StateSettings) {
        foreach ($l in Get-Content -LiteralPath $StateSettings) {
            $i = $l.IndexOf('=')
            if ($i -gt 0) { $rec[$l.Substring(0, $i)] = $l.Substring($i + 1) }
        }
    }
    return $rec
}

# Sets KEY=value in settings.ini keeping the line's comment; returns the old value, or $null if the key is absent.
function Set-IniValue([ref]$lines, [string]$key, [string]$value) {
    for ($i = 0; $i -lt $lines.Value.Count; $i++) {
        $m = [regex]::Match($lines.Value[$i], '^(' + [regex]::Escape($key) + '=)(\S*)(.*)$')
        if (-not $m.Success) { continue }
        $old = $m.Groups[2].Value
        $new = $value
        $rest = $m.Groups[3].Value
        if ($new.Length -lt $old.Length) { $new = $new.PadRight($old.Length) }
        elseif ($new.Length -gt $old.Length) {
            $spaces = $rest.Length - $rest.TrimStart(' ').Length
            $rest = $rest.Substring([Math]::Min($new.Length - $old.Length, [Math]::Max($spaces - 1, 0)))
        }
        $lines.Value[$i] = $m.Groups[1].Value + $new + $rest
        return $old
    }
    return $null
}

function Get-IniValue([string[]]$lines, [string]$key) {
    foreach ($l in $lines) {
        $m = [regex]::Match($l, '^' + [regex]::Escape($key) + '=(\S*)')
        if ($m.Success) { return $m.Groups[1].Value }
    }
    return $null
}

function Read-Ini([string]$path) {
    $raw = [IO.File]::ReadAllText($path)
    $nl = "`n"; if ($raw.Contains("`r`n")) { $nl = "`r`n" }
    return @{ Lines = [System.Collections.ArrayList]@($raw -split "`r?`n"); NewLine = $nl }
}

function Write-Ini([string]$path, $ini) {
    $text = ($ini.Lines -join $ini.NewLine)
    [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding $false))  # no BOM, like the game's own file
}

# A recorded entry is "original" (an older launcher: what it set is in $Wanted) or "original|set" (this installer).
function Split-Recorded([string]$k, [string]$v) {
    $i = $v.IndexOf('|')
    if ($i -ge 0) { return @($v.Substring(0, $i), $v.Substring($i + 1)) }
    if ($Wanted.Contains($k)) { return @($v, $Wanted[$k]) }
    return @($v, $null)
}

function Restore-Settings([string]$game) {
    $ini = Join-Path $game 'settings.ini'
    $rec = Read-Recorded
    if ($rec.Count -eq 0 -or -not (Test-Path -LiteralPath $ini)) { return }
    $data = Read-Ini $ini
    $back = @()
    foreach ($k in $rec.Keys) {
        $orig, $set = Split-Recorded $k $rec[$k]
        $cur = Get-IniValue $data.Lines $k
        if ($null -ne $cur -and $null -ne $set -and $cur -eq $set) {  # still what the mod's setup set
            $lines = $data.Lines
            [void](Set-IniValue ([ref]$lines) $k $orig)
            $back += "$k $orig"
        } else {
            Say "settings.ini: $k left at $cur (changed by you since)"
        }
    }
    if ($back.Count -gt 0) { Say ('Game settings put back as they were: ' + ($back -join ', ')) }  # (one line, owner 2026-11-15)
    Write-Ini $ini $data
    [void](Send-ToRecycleBin ($ini + '.vrmod-backup') $game)
    [void](Send-ToRecycleBin $StateSettings $game)
}

# ---- the game's graphics settings for VR (owner 2026-10-19) ------------------------------------------------------
# Display mode Windowed, VSync off, frame rate limit off, motion blur 0, HDR off, and DLSS on with the chosen preset
# (or off) - the player is asked about DLSS. The original values are recorded (settings_changed.txt) so the
# uninstaller puts them back while they are still what this set. Anything that cannot be changed is listed with what
# to set by hand in the game's graphics settings.
function Set-VrGameSettings([string]$game) {
    $manual = @{
        'DisplayMode' = 'Display mode: Windowed'; 'Vsync' = 'VSync: Off'; 'FPSLimit' = 'Frame rate limit: Off'
        'MotionBlur' = 'Motion blur: 0'; 'HDR' = 'HDR: Off'; 'DLSSQuality' = 'DLSS'; 'FSRQuality' = 'FSR: Off'
    }
    Write-Host ''
    Say 'GAME SETTINGS FOR VR'
    $nvidia = $false
    try {
        $nvidia = [bool](Get-CimInstance Win32_VideoController -ErrorAction Stop | Where-Object { $_.Name -match 'NVIDIA' })
    } catch { }
    if ($nvidia) { Say 'An NVIDIA graphics card was found - DLSS is recommended.' }
    else { Say 'No NVIDIA graphics card was found - DLSS needs one (NVIDIA RTX).' }
    $dlss = (Read-Key 'Are you going to use DLSS? Press Y or N:' @('Y', 'N')) -eq 'Y'
    $preset = 'Off'
    $presetName = 'off'
    if ($dlss) {
        $names = @{ '1' = @('UltraPerformance', 'Ultra Performance'); '2' = @('Performance', 'Performance');
                    '3' = @('Balanced', 'Balanced'); '4' = @('Quality', 'Quality') }
        $pick = Read-Key 'Which DLSS preset? 1 = Ultra Performance  2 = Performance  3 = Balanced  4 = Quality:' @('1', '2', '3', '4')
        $preset = $names[$pick][0]
        $presetName = $names[$pick][1]
    }
    $want = [ordered]@{ 'DisplayMode' = 'Windowed'; 'Vsync' = 'Off'; 'FPSLimit' = 'Off'; 'MotionBlur' = '0'; 'HDR' = 'Off';
                        'DLSSQuality' = $preset }
    if ($dlss) { $want['FSRQuality'] = 'Off' }  # (DLSS and FSR are one choice in the game)
    $manual['DLSSQuality'] = $(if ($dlss) { "DLSS: $presetName" } else { 'DLSS: Off' })
    # owner 2026-10-21: with NVIDIA (DLSS chosen) also NVIDIA Reflex Low Latency On + Boost and latency markers
    # (settings.ini LatencyEnable / LatencyBoost / LatencyMarkers). Owner 2026-11-15: NOT latency flash any more (a white
    # square flashing on screen for latency-measuring hardware) - an older install's LatencyFlash=On is put back below
    if ($dlss) {
        $want['LatencyEnable'] = 'On'; $want['LatencyBoost'] = 'On'; $want['LatencyMarkers'] = 'On'
        $manual['LatencyEnable'] = 'NVIDIA Reflex Low Latency: On'
        $manual['LatencyBoost'] = 'NVIDIA Reflex Low Latency: On + Boost'
        $manual['LatencyMarkers'] = 'Latency markers: On'
    }
    # the short words for the one summary line
    $short = [ordered]@{ 'DisplayMode' = 'Windowed'; 'Vsync' = 'VSync off'; 'FPSLimit' = 'frame limit off';
                         'MotionBlur' = 'motion blur 0'; 'HDR' = 'HDR off'
                         'DLSSQuality' = $(if ($dlss) { "DLSS $presetName" } else { 'DLSS off' }); 'FSRQuality' = 'FSR off'
                         'LatencyEnable' = 'NVIDIA Reflex On'; 'LatencyBoost' = '+ Boost'; 'LatencyMarkers' = 'latency markers on' }

    $ini = Join-Path $game 'settings.ini'
    $failed = @()
    if (-not (Test-Path -LiteralPath $ini)) {
        Warn 'The game settings file (settings.ini) was not found - the game creates it the first time it runs.'
        Warn '(Or start the game once, close it, and run this installer again.)'
        $failed = @($want.Keys)
    } else {
        try {
            if (-not (Test-Path -LiteralPath ($ini + '.vrmod-backup'))) { Copy-Item -LiteralPath $ini -Destination ($ini + '.vrmod-backup') }
            $data = Read-Ini $ini
            $rec = Read-Recorded
            $lines = $data.Lines
            # an older install turned latency flash on: back to the player's own value while it is still what it set
            if ($rec.Contains('LatencyFlash')) {
                $orig, $set = Split-Recorded 'LatencyFlash' $rec['LatencyFlash']
                if ($null -ne $set -and (Get-IniValue $lines 'LatencyFlash') -eq $set) { [void](Set-IniValue ([ref]$lines) 'LatencyFlash' $orig) }
                $rec.Remove('LatencyFlash')
            }
            foreach ($k in $want.Keys) {
                $old = Set-IniValue ([ref]$lines) $k $want[$k]
                if ($null -eq $old) { $failed += $k; continue }
                # the first original is kept (a second install must not record the mod's own values as yours)
                $orig = $old
                if ($rec.Contains($k)) { $orig = (Split-Recorded $k $rec[$k])[0] }
                $rec[$k] = "$orig|$($want[$k])"
            }
            Write-Ini $ini $data
            $out = foreach ($k in $rec.Keys) { "$k=$($rec[$k])" }
            Set-Content -LiteralPath $StateSettings -Value $out -Encoding ASCII
            # read back: the file really holds the values now
            $check = Read-Ini $ini
            foreach ($k in $want.Keys) {
                if ($failed -contains $k) { continue }
                if ((Get-IniValue $check.Lines $k) -ne $want[$k]) { $failed += $k }
            }
        } catch {
            Warn "The game settings could not be changed ($($_.Exception.Message))."
            $failed = @($want.Keys)
        }
    }
    $done = @($want.Keys | Where-Object { $failed -notcontains $_ })
    if ($done.Count -gt 0) {
        $parts = @($short.Keys | Where-Object { $done -contains $_ } | ForEach-Object { $short[$_] })
        Say ('Game settings set for VR: ' + (($parts -join ', ') -replace ', \+ Boost', ' + Boost'))
    }
    if ($failed.Count -gt 0) {
        Write-Host ''
        Warn 'These game settings could NOT be changed by the installer.'
        Warn 'Please set them yourself in the game (Settings -> Graphics / Display):'
        foreach ($k in $failed) { Warn "   - $($manual[$k])" }
    }
}

# ---- the mod's own starting choices (owner 2026-10-21; AFR owner 2026-11-15) -------------------------------------
# First person while walking on, first person in cutscenes off, finishers in third person, AFR rendering. A new
# gowvr.ini (written by the mod on its first start) has them already. An existing one gets them ONCE per defaults
# version: the key installer_defaults (now 2 - AFR added) marks that it was done - from then on the player's own
# changes stay (an install that already had 1 gets only what version 2 added: AFR).
$DefaultsVersion = 2
function Set-VrModDefaults([string]$game) {
    $ini = Join-Path $game 'gowvr.ini'
    $what = 'first person while walking on, in cutscenes off, finishers in third person, AFR rendering'
    if (-not (Test-Path -LiteralPath $ini)) {
        Say "VR settings: $what"
        return
    }
    try {
        $lines = [System.Collections.Generic.List[string]]([System.IO.File]::ReadAllLines($ini))
        $have = 0
        foreach ($l in $lines) {
            if ($l -match '^\s*installer_defaults\s*=\s*([0-9.]+)') { $have = [double]$Matches[1] }
        }
        if ($have -ge $DefaultsVersion) {
            Say 'VR settings: your own choices kept'
            return
        }
        $want = [ordered]@{ 'afr_rendering' = '1' }
        if ($have -lt 1) {
            $want['first_person_walking'] = '1'; $want['first_person_cutscenes'] = '0'; $want['finisher_third_person'] = '1'
        }
        foreach ($k in $want.Keys) {
            $found = $false
            for ($i = 0; $i -lt $lines.Count; $i++) {
                if ($lines[$i] -match ('^\s*' + [regex]::Escape($k) + '\s*=')) { $lines[$i] = "$k=$($want[$k])"; $found = $true }
            }
            if (-not $found) { $lines.Add("$k=$($want[$k])") }
        }
        # (one marker line: the older one and its comment replaced)
        for ($i = $lines.Count - 1; $i -ge 0; $i--) {
            if ($lines[$i] -match '^\s*installer_defaults\s*=' -or $lines[$i] -match '^# Set once by the installer') { $lines.RemoveAt($i) }
        }
        $lines.Add("# Set once by the installer ($what) - your own changes are kept.")
        $lines.Add("installer_defaults=$DefaultsVersion")
        [System.IO.File]::WriteAllLines($ini, $lines, (New-Object System.Text.UTF8Encoding $false))
        if ($have -ge 1) { Say 'VR settings: AFR rendering on (your other choices kept)' } else { Say "VR settings: $what" }
    } catch {
        Warn "The VR settings (gowvr.ini) could not be changed ($($_.Exception.Message))."
    }
}

# =================================================================================================================
if ($Mode -eq 'Install') {
    Banner 'GOD OF WAR VR MOD  -  INSTALL'
    $payload = Join-Path $Files 'version.dll'
    if (-not (Test-Path -LiteralPath $payload)) { Bad 'files\version.dll is missing - unzip the mod again.'; Done 1 }
    $game = Find-GameDir
    if (-not $game) { Bad 'No God of War folder found - nothing was changed.'; Done 1 }
    Say "Game folder: $game"
    if (Test-GoWRunning) { Warn 'God of War is running. Close it, then run this again.'; Done 1 }
    Set-Content -LiteralPath $StateGame -Value $game -Encoding UTF8

    $dst = Join-Path $game 'version.dll'
    if (Test-Path -LiteralPath $dst) {
        if (-not (Test-OurDll $dst)) {
            Bad 'There is already a version.dll in the game folder that is NOT this mod'
            Say '(another mod or tool). It was left alone. Move it somewhere else if you want'
            Say 'this VR mod instead, then run this again.'
            Done 1
        }
        if ((Get-FileHash -LiteralPath $dst).Hash -eq (Get-FileHash -LiteralPath $payload).Hash) {
            Say 'version.dll: already installed and up to date'
        } else {
            Copy-Item -LiteralPath $payload -Destination $dst -Force
            Say 'version.dll: updated to this release'
        }
    } else {
        Copy-Item -LiteralPath $payload -Destination $dst
        Say 'version.dll: installed'
    }
    try { Unblock-File -LiteralPath $dst } catch { }

    Set-VrGameSettings $game
    Set-VrModDefaults $game

    Write-Host ''
    Say 'Done. From now on just start God of War as usual with your headset on - VR starts by itself.'
    Say 'Uninstall any time with "Uninstall VR mod.cmd" in this folder.'
    Done 0
}

if ($Mode -eq 'Uninstall') {
    Banner 'GOD OF WAR VR MOD  -  UNINSTALL'
    $game = Find-GameDir
    if (-not $game) { Bad 'No God of War folder found - nothing was changed.'; Done 1 }
    Say "Game folder: $game"
    # (before the question: never make the player confirm and only then say the game must be closed)
    if (Test-GoWRunning) { Warn 'God of War is running - close it first, then run this again.'; Done 1 }
    Write-Host ''
    Say 'This will move to the Recycle Bin (nothing is deleted for good):'
    Say '  - version.dll (the mod) from the game folder'
    Say '  - if you want: gowvr.ini (your VR settings) and the gowvr_logs folder'
    Say '  - this GOW_VR_MOD folder'
    Say 'Your saves, GoW.exe and every other game file are left alone.'
    Write-Host ''
    $a = Read-Host '   Type YES and press Enter to uninstall (anything else cancels)'
    if ($a -ne 'YES' -and $a -ne 'yes') { Say 'Cancelled. Nothing was changed.'; Done 0 }
    if (Test-GoWRunning) { Warn 'God of War is running - close it first, then run this again.'; Done 1 }
    Write-Host ''

    $dll = Join-Path $game 'version.dll'
    if (Test-Path -LiteralPath $dll) {
        if (Test-OurDll $dll) {
            if (Send-ToRecycleBin $dll $game) { Say 'version.dll (the mod) -> Recycle Bin' }
        } else { Warn 'version.dll in the game folder is not this mod - left alone' }
    } else { Say 'version.dll: not installed' }

    Restore-Settings $game

    $k = Read-Key 'Also move gowvr.ini (your VR settings) and the gowvr_logs folder to the Recycle Bin? Press Y or N:' @('Y', 'N')
    if ($k -eq 'Y') {
        foreach ($f in @('gowvr.ini', 'gowvr.ini.tmp', 'gowvr_movenames.txt', 'gowvr_logs')) {
            if (Send-ToRecycleBin (Join-Path $game $f) $game) { Say "$f -> Recycle Bin" }
        }
    } else {
        Say 'gowvr.ini and gowvr_logs kept'
    }

    # this folder: only when it is really the mod's own folder - inside the game folder but not the game folder,
    # with the mod's files in it and no program file anywhere inside (Send-ToRecycleBin checks the last two again)
    $modOk = (Test-Path -LiteralPath (Join-Path $ModDir 'tools\gowvr_setup.ps1')) -and
             (Test-Path -LiteralPath (Join-Path $ModDir 'Uninstall VR mod.cmd')) -and
             -not (Test-Path -LiteralPath (Join-Path $ModDir 'GoW.exe'))
    Write-Host ''
    if ($modOk -and (Send-ToRecycleBin $ModDir $game)) {
        Say "$ModDir -> Recycle Bin"
    } else {
        Say "This folder was left where it is: $ModDir"
        Say 'Delete it yourself if you do not need it any more.'
    }
    Write-Host ''
    Say 'Done. God of War starts normally on your monitor again.'
    Done 0
}

# ---- a copy of the player's saves for a bug report (owner 2026-11-15) ---------------------------------------------
# Where God of War (2018) keeps its saves on PC, for the Steam, Epic and GOG copies alike: <Saved Games>\God of War\
# <a folder per account (the store's account ID)>\game.sav - every save slot is in that one file (Sony's support page
# "Find save data files"; the owner's own install: Saved Games\God of War\<Steam ID>\game.sav). Steam Cloud only syncs
# that folder. "Saved Games" is read from Windows' own setting (User Shell Folders) because it can be moved (OneDrive,
# another drive); %USERPROFILE%\Saved Games otherwise.
# SAFETY - the saves are never changed: refused while the game runs (no half-written save copied); each file is opened
# for READING only (FileShare.ReadWrite: never locked against the game); its SHA-256 is taken before and after and the
# copy's must match; nothing is written, moved, renamed or deleted under Saved Games. The copy goes to a new folder in
# %TEMP%, is zipped onto the Desktop, and only that temporary copy is removed. Folders named backup* (the player's own
# backups) are skipped; each account folder is named profile_1, profile_2 ... in the zip (no account ID in it).
function Get-SavedGamesDir {
    try {
        $v = (Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders' `
              -ErrorAction Stop).'{4C5C32FF-BB9D-43b0-B5B4-2D72E54EAAA4}'
        if ($v) {
            $p = [Environment]::ExpandEnvironmentVariables($v)
            if (Test-Path -LiteralPath $p) { return $p }
        }
    } catch { }
    return (Join-Path $env:USERPROFILE 'Saved Games')
}

function Get-Sha256([string]$path) {
    $s = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $h = [Security.Cryptography.SHA256]::Create()
        return [BitConverter]::ToString($h.ComputeHash($s))
    } finally { $s.Dispose() }
}

if ($Mode -eq 'Save') {
    Banner 'GOD OF WAR VR MOD  -  COPY MY SAVE'
    if (Test-GoWRunning) { Warn 'God of War is running. Close it first (a save is copied only while the game is closed), then run this again.'; Done 1 }
    $root = Join-Path (Get-SavedGamesDir) 'God of War'
    if (-not (Test-Path -LiteralPath $root)) { Bad "No God of War saves were found (looked in $root)."; Done 1 }
    $accounts = @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notmatch '^(?i)backup' -and (Test-Path -LiteralPath (Join-Path $_.FullName 'game.sav')) })
    # a game.sav straight in God of War\ (no account folder - possible for the DRM-free GOG copy) counts as a profile too
    if (Test-Path -LiteralPath (Join-Path $root 'game.sav')) { $accounts = @(Get-Item -LiteralPath $root) + $accounts }
    if ($accounts.Count -eq 0) { Bad "No game.sav was found in $root."; Done 1 }
    Say "Your saves: $root"
    Say 'They are only READ - nothing in that folder is changed, moved or deleted.'
    $stamp = Get-Date -Format 'yyyy-MM-dd_HHmmss'
    $tmp = Join-Path $env:TEMP "gowvr_save_copy_$stamp"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $zip = $null
    try {
        $i = 0
        $copied = 0
        foreach ($acc in $accounts) {
            $i++
            $dst = Join-Path $tmp "profile_$i"
            New-Item -ItemType Directory -Force -Path $dst | Out-Null
            # only the save itself (game.sav, and any game.sav.* the game keeps beside it) - nothing else from the folder
            foreach ($f in Get-ChildItem -LiteralPath $acc.FullName -File -Filter 'game.sav*' -ErrorAction SilentlyContinue) {
                $before = Get-Sha256 $f.FullName
                $to = Join-Path $dst $f.Name
                $in = [IO.File]::Open($f.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
                try {
                    $out = [IO.File]::Open($to, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                    try { $in.CopyTo($out) } finally { $out.Dispose() }
                } finally { $in.Dispose() }
                if ((Get-Sha256 $to) -ne $before) { Bad "The copy of $($f.Name) did not match the original - nothing was sent."; Done 1 }
                if ((Get-Sha256 $f.FullName) -ne $before) { Warn "$($f.Name) changed while it was copied (is the game starting?) - run this again with the game closed."; Done 1 }
                $copied++
            }
        }
        $desk = [Environment]::GetFolderPath('Desktop')
        if (-not $desk -or -not (Test-Path -LiteralPath $desk)) { $desk = $env:USERPROFILE }
        $zip = Join-Path $desk "GoW_VR_save_$stamp.zip"
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::CreateFromDirectory($tmp, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
    } catch {
        Bad "The save could not be copied ($($_.Exception.Message)). Your saves were not changed."
        Done 1
    } finally {
        # only this tool's own temporary copy (a new folder under %TEMP%) - never anything under Saved Games
        $t = [IO.Path]::GetFullPath($tmp)
        if ($t.StartsWith([IO.Path]::GetFullPath($env:TEMP), [StringComparison]::OrdinalIgnoreCase) -and $t -match 'gowvr_save_copy_') {
            Remove-Item -LiteralPath $t -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    Say "Saved a copy: $zip ($copied file(s), $($accounts.Count) profile(s); your saves are unchanged)"
    Say 'Attach that zip to your report on the mod''s GitHub page (Issues).'
    try { Start-Process explorer.exe "/select,`"$zip`"" } catch { }
    Done 0
}
