# ===========================================================================
#  GOD OF WAR - VR MOD : installer / launcher / uninstaller
#
#  Called by the two .cmd files in the GOW_VR_MOD folder:
#    "Play God of War in VR.cmd"  ->  -Mode Play       (install or update, then start the game)
#    "Uninstall VR mod.cmd"       ->  -Mode Uninstall  (put the game back as it was)
#
#  The mod is a single file, version.dll, placed next to GoW.exe. Windows loads it into the game
#  whichever way the game is started (Steam, Epic, the exe) - nothing is injected and no game file
#  is replaced or patched on disk. The only game file edited is settings.ini (three values, below);
#  the original values are recorded and put back by the uninstaller.
#
#  Kept ASCII only: Windows PowerShell 5.1 reads a script without a BOM in the ANSI code page.
# ===========================================================================
param([ValidateSet('Play', 'Uninstall')][string]$Mode = 'Play')

$ErrorActionPreference = 'Stop'
$ModDir = Split-Path -Parent $PSScriptRoot
$Files = Join-Path $ModDir 'files'
$StateGame = Join-Path $ModDir 'game_dir.txt'           # where the mod was installed (for the uninstaller)
$StateSettings = Join-Path $ModDir 'settings_changed.txt' # settings.ini values the mod changed: KEY=original

# settings.ini values VR needs. Everything else is left to the player (see README).
#   FPSLimit=Off   - a frame cap below the headset's refresh rate makes VR judder
#   MotionBlur=0   - motion blur smears with every head movement (comfort)
#   HDR=Off        - the headset image is built from the SDR picture; HDR breaks its colours
$Wanted = [ordered]@{ 'FPSLimit' = 'Off'; 'MotionBlur' = '0'; 'HDR' = 'Off' }

function Say([string]$t) { Write-Host "   $t" }
function Warn([string]$t) { Write-Host "   [!] $t" -ForegroundColor Yellow }
function Bad([string]$t) { Write-Host "   [X] $t" -ForegroundColor Red }
function Done([int]$code) {
    Write-Host ''
    [void](Read-Host '   Press Enter to close')
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
    # 1. this folder sits inside the game folder (the normal way, see README)
    $parent = Split-Path -Parent $ModDir
    if (Test-GameDir $parent) { return $parent }
    # 2. where the last install went
    if (Test-Path -LiteralPath $StateGame) {
        $d = (Get-Content -LiteralPath $StateGame -Raw).Trim()
        if (Test-GameDir $d) { return $d }
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
    Say 'Easiest fix: move this whole GOW_VR_MOD folder INTO your God of War folder'
    Say '(the one that contains GoW.exe) and run this again.'
    Write-Host ''
    $d = (Read-Host '   Or paste the God of War folder here and press Enter').Trim().Trim('"')
    if (Test-GameDir $d) { return (Resolve-Path -LiteralPath $d).Path }
    return $null
}

function Test-GoWRunning { return [bool](Get-Process -Name 'GoW' -ErrorAction SilentlyContinue) }

# ---- version.dll -------------------------------------------------------------------------------------------------
# Ours = a gowvr build (its log line "gowvr <version> linked ..." is in the file). Anything else named version.dll
# belongs to another mod / tool and is never overwritten or deleted.
function Test-OurDll([string]$path) {
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
    return $text.Contains('gowvr ') -and $text.Contains('linked')
}

# ---- settings.ini ------------------------------------------------------------------------------------------------
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
        # keep the comments lined up: pad a shorter value, take a longer one's extra width from the spaces after it
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

function Apply-Settings([string]$game) {
    $ini = Join-Path $game 'settings.ini'
    if (-not (Test-Path -LiteralPath $ini)) { return $false }
    $data = Read-Ini $ini
    $rec = Read-Recorded
    $changed = @()
    foreach ($k in $Wanted.Keys) {
        $cur = Get-IniValue $data.Lines $k
        if ($null -eq $cur -or $cur -eq $Wanted[$k]) { continue }
        $lines = $data.Lines
        [void](Set-IniValue ([ref]$lines) $k $Wanted[$k])
        if (-not $rec.Contains($k)) { $rec[$k] = $cur }  # the player's own value, recorded once
        $changed += "$k $cur -> $($Wanted[$k])"
    }
    if ($changed.Count -gt 0) {
        if (-not (Test-Path -LiteralPath ($ini + '.vrmod-backup'))) { Copy-Item -LiteralPath $ini -Destination ($ini + '.vrmod-backup') }
        Write-Ini $ini $data
        Set-Content -LiteralPath $StateSettings -Value ($rec.Keys | ForEach-Object { "$_=$($rec[$_])" }) -Encoding ASCII
        foreach ($c in $changed) { Say "settings.ini: $c" }
    } else {
        Say 'settings.ini: VR settings already in place'
    }
    return $true
}

function Restore-Settings([string]$game) {
    $ini = Join-Path $game 'settings.ini'
    $rec = Read-Recorded
    if ($rec.Count -eq 0 -or -not (Test-Path -LiteralPath $ini)) { return }
    $data = Read-Ini $ini
    foreach ($k in $rec.Keys) {
        $cur = Get-IniValue $data.Lines $k
        if ($null -ne $cur -and $Wanted.Contains($k) -and $cur -eq $Wanted[$k]) {  # still what the mod set
            $lines = $data.Lines
            [void](Set-IniValue ([ref]$lines) $k $rec[$k])
            Say "settings.ini: $k back to $($rec[$k])"
        } else {
            Say "settings.ini: $k left at $cur (changed by you since)"
        }
    }
    Write-Ini $ini $data
    Remove-Item -LiteralPath $StateSettings -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath ($ini + '.vrmod-backup') -Force -ErrorAction SilentlyContinue
}

# ---- starting the game -------------------------------------------------------------------------------------------
function Start-Game([string]$game) {
    # Steam copy: through Steam (an appmanifest in the library whose installdir is this folder)
    $common = Split-Path -Parent $game
    if ((Split-Path -Leaf $common) -eq 'common') {
        $steamapps = Split-Path -Parent $common
        $dir = Split-Path -Leaf $game
        foreach ($m in Get-ChildItem -LiteralPath $steamapps -Filter 'appmanifest_*.acf' -ErrorAction SilentlyContinue) {
            $raw = Get-Content -LiteralPath $m.FullName -Raw -ErrorAction SilentlyContinue
            if (-not $raw) { continue }
            $inst = [regex]::Match($raw, '"installdir"\s+"([^"]+)"').Groups[1].Value
            $id = [regex]::Match($raw, '"appid"\s+"(\d+)"').Groups[1].Value
            if ($inst -eq $dir -and $id) {
                Say "Starting God of War through Steam (app $id)"
                Start-Process "steam://rungameid/$id"
                return
            }
        }
    }
    # Epic copy: through the Epic Games Launcher (its manifest for this folder)   [UNTESTED]
    $man = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
    if (Test-Path -LiteralPath $man) {
        foreach ($f in Get-ChildItem -LiteralPath $man -Filter '*.item' -ErrorAction SilentlyContinue) {
            try { $j = Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json } catch { continue }
            if (-not $j.InstallLocation) { continue }
            $loc = $j.InstallLocation.TrimEnd('\', '/')
            if ($loc -ieq $game.TrimEnd('\', '/') -and $j.CatalogNamespace -and $j.CatalogItemId -and $j.AppName) {
                Say 'Starting God of War through the Epic Games Launcher'
                Start-Process ("com.epicgames.launcher://apps/" + $j.CatalogNamespace + '%3A' + $j.CatalogItemId + '%3A' +
                               $j.AppName + '?action=launch&silent=true')
                return
            }
        }
    }
    Say 'Starting GoW.exe'
    Start-Process -FilePath (Join-Path $game 'GoW.exe') -WorkingDirectory $game
}

# The OpenXR runtime the mod will use: XR_RUNTIME_JSON if set, else the system's active runtime (as the mod reads it)
function Get-ActiveXrRuntime {
    if ($env:XR_RUNTIME_JSON) { return $env:XR_RUNTIME_JSON }
    try {
        return (Get-ItemProperty -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1' -Name 'ActiveRuntime' -ErrorAction Stop).ActiveRuntime
    } catch { return $null }
}

function Start-SteamVRIfNeeded {
    # SteamVR only when it is the active OpenXR runtime: Virtual Desktop (VDXR), Oculus / Meta, WMR and other
    # runtimes run without it
    $rt = Get-ActiveXrRuntime
    if ($rt -and $rt -notmatch 'steamvr|steamxr') {
        Say "OpenXR runtime: $rt"
        Say 'SteamVR is not needed for it - make sure that runtime is running and your headset is connected.'
        return
    }
    if (Get-Process -Name 'vrserver' -ErrorAction SilentlyContinue) { Say 'SteamVR is running'; return }
    if (Test-Path -LiteralPath 'Registry::HKEY_CLASSES_ROOT\steam') {
        Say 'Starting SteamVR...'
        Start-Process 'steam://run/250820'
    } else {
        Warn 'SteamVR is not running - start it (or your OpenXR runtime) now.'
    }
}

# =================================================================================================================
if ($Mode -eq 'Play') {
    Banner 'GOD OF WAR  -  VR MOD'
    $payload = Join-Path $Files 'version.dll'
    if (-not (Test-Path -LiteralPath $payload)) { Bad 'files\version.dll is missing - re-extract the mod.'; Done 1 }

    $game = Find-GameDir
    if (-not $game) { Bad 'No God of War folder - nothing was changed.'; Done 1 }
    Say "Game folder: $game"
    Set-Content -LiteralPath $StateGame -Value $game -Encoding UTF8

    if (Test-GoWRunning) {
        Warn 'God of War is running. Close it, then run this again (the mod is installed while the game is closed).'
        Done 1
    }

    # version.dll
    $dst = Join-Path $game 'version.dll'
    if (Test-Path -LiteralPath $dst) {
        if (-not (Test-OurDll $dst)) {
            Bad 'There is already a version.dll in the game folder that is NOT this mod'
            Say '(another mod or tool). It was left alone. Move it somewhere else if you want'
            Say 'this VR mod instead, then run this again.'
            Done 1
        }
        if ((Get-FileHash -LiteralPath $dst).Hash -eq (Get-FileHash -LiteralPath $payload).Hash) {
            Say 'version.dll: installed and up to date'
        } else {
            Copy-Item -LiteralPath $payload -Destination $dst -Force
            Say 'version.dll: updated to this release'
        }
    } else {
        Copy-Item -LiteralPath $payload -Destination $dst
        Say 'version.dll: installed'
    }
    try { Unblock-File -LiteralPath $dst } catch { }

    # gowvr.ini - the player's settings are never overwritten
    $ini = Join-Path $game 'gowvr.ini'
    if (-not (Test-Path -LiteralPath $ini)) {
        Copy-Item -LiteralPath (Join-Path $Files 'gowvr.ini') -Destination $ini
        Say 'gowvr.ini: default VR settings created'
    } else {
        Say 'gowvr.ini: your VR settings kept'
    }

    $settingsDone = Apply-Settings $game
    if (-not $settingsDone) {
        Write-Host ''
        Warn 'settings.ini does not exist yet - the game writes it the first time it runs.'
        Say 'Keep this window open: once you QUIT the game, the VR settings are applied for'
        Say 'your next launch (frame limit off, motion blur off, HDR off).'
    }

    Write-Host ''
    Start-SteamVRIfNeeded
    Write-Host ''
    Say 'In the game:'
    Say '  - VR starts by itself as soon as the game starts'
    Say '  - F9 = VR on / off      F8 = recenter the view'
    Say '  - F1 = first person (motion controls)   F3 = first person in cutscenes'
    Say '  - the right trigger pressed at your head also toggles F1 (held = F3)'
    Say '  - F10 / F11 = Xbox / PlayStation button icons'
    Say '  - more in GUIDE.md'
    Write-Host ''
    [void](Read-Host '   Put the headset on, then press Enter to start the game')
    Start-Game $game

    if (-not $settingsDone) {
        Say 'Waiting for the game to start...'
        $t0 = Get-Date
        while (-not (Test-GoWRunning) -and ((Get-Date) - $t0).TotalMinutes -lt 5) { Start-Sleep -Seconds 2 }
        if (Test-GoWRunning) {
            Say 'Game running. Applying the VR settings once you quit it...'
            while (Test-GoWRunning) { Start-Sleep -Seconds 3 }
            Start-Sleep -Seconds 2
            if (-not (Apply-Settings $game)) { Warn 'settings.ini still missing - run this again after playing once.' }
        } else {
            Warn 'The game did not start within 5 minutes - run this again to apply the settings.'
        }
        Done 0
    }
    Say 'The mod stays installed: starting the game from Steam / Epic / its exe also runs in VR'
    Say '(set autovr=0 in gowvr.ini to start flat and switch with F9). Uninstall with "Uninstall VR mod.cmd".'
    Start-Sleep -Seconds 4
    exit 0
}

# =================================================================================================================
if ($Mode -eq 'Uninstall') {
    Banner 'GOD OF WAR VR MOD  -  UNINSTALL'
    $game = Find-GameDir
    if (-not $game) { Bad 'No God of War folder found - nothing was changed.'; Done 1 }
    Say "Game folder: $game"
    Write-Host ''
    Say 'This will:'
    Say '  1. remove the mod (version.dll) from the game folder'
    Say '  2. put back the settings.ini values the mod changed (unless you changed them since)'
    Say '  3. optionally delete gowvr.ini (your VR settings) and the gowvr_logs folder'
    Say '  4. delete this whole folder:'
    Say "     $ModDir"
    Say 'Your saves and every other game file are left alone.'
    Write-Host ''
    $a = Read-Host '   Type YES and press Enter to uninstall (anything else cancels)'
    if ($a -ne 'YES' -and $a -ne 'yes') { Say 'Cancelled. Nothing was changed.'; Done 0 }
    if (Test-GoWRunning) { Warn 'God of War is running - close it first, then run this again.'; Done 1 }
    Write-Host ''

    $dll = Join-Path $game 'version.dll'
    if (Test-Path -LiteralPath $dll) {
        if (Test-OurDll $dll) { Remove-Item -LiteralPath $dll -Force; Say 'Removed version.dll (the mod)' }
        else { Warn 'version.dll in the game folder is not this mod - left alone' }
    } else { Say 'version.dll: not installed' }

    Restore-Settings $game

    $k = Read-Host '   Also delete gowvr.ini (your VR settings) and the gowvr_logs folder? Type Y to delete'
    if ($k -eq 'Y' -or $k -eq 'y') {
        foreach ($f in @('gowvr.ini', 'gowvr.ini.tmp', 'gowvr_movenames.txt')) {
            $p = Join-Path $game $f
            if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; Say "Removed $f" }
        }
        $logs = Join-Path $game 'gowvr_logs'
        if (Test-Path -LiteralPath $logs) { Remove-Item -LiteralPath $logs -Recurse -Force; Say 'Removed gowvr_logs' }
    } else {
        Say 'gowvr.ini and gowvr_logs kept'
    }

    Write-Host ''
    Say 'Done. God of War will start normally on your monitor again.'
    Say 'This folder is deleted when you close this window.'
    [void](Read-Host '   Press Enter to finish')
    # The running scripts live in this folder: a detached shell deletes it once they have exited.
    $parent = Split-Path -Parent $ModDir
    Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ping -n 4 127.0.0.1 >nul & rd /s /q "' + $ModDir + '"') `
        -WorkingDirectory $parent -WindowStyle Hidden
    exit 0
}
