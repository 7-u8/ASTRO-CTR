# Fixes the launcher error after an update:
#   KeyError: 'ui/octane_cupselect_pt.lng'   (run.py -> online_mode.py -> cup_package)
# The launcher's package lists a language file that some game copies do not have; this makes it skip missing files.
# Usage (PowerShell), from anywhere:   .\fix_launcher_cupselect.ps1 "C:\path\to\CTRNF EU"
# The folder is the one that holds run.py and scripts\. A backup online_mode.py.astro_backup is kept.
param([Parameter(Mandatory = $true)][string]$LauncherDir)

$file = Join-Path $LauncherDir 'scripts\online_mode.py'
if (-not (Test-Path $file)) { Write-Host "Not found: $file (give the folder that holds run.py)"; exit 1 }

$text = [System.IO.File]::ReadAllText($file)
if ($text.Contains('if path not in files:')) { Write-Host 'Already fixed.'; exit 0 }

# the file may use CRLF or LF: keep whichever it has
$nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$old = "        seen.add(path)$nl        needed.add(path)$nl        pak, entry = files[path]$nl"
if (-not $text.Contains($old)) { Write-Host 'The launcher code looks different from the one this fix was made for. Nothing changed.'; exit 2 }

$new = "        if path not in files:   # listed by the package but not shipped in this game copy (e.g. a missing language file)$nl" +
       "            continue$nl" + $old
$backup = "$file.astro_backup"
if (-not (Test-Path $backup)) { Copy-Item $file $backup }
[System.IO.File]::WriteAllText($file, $text.Replace($old, $new), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Fixed. Backup: $backup"
