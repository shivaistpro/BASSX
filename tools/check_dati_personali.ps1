# Controlla che nei file versionati non ci siano dati personali o percorsi del PC.
# Uso: powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_dati_personali.ps1
# Exit 0 = pulito, 1 = trovati (elencati). Lo lancia build_windows.bat prima del push.
$patterns = @(
    'CLAUDE SPACE',            # cartella di lavoro
    '[A-Za-z]:[\\/]Users[\\/]', '/Users/[A-Za-z]', '/home/[a-z]',
    '\b[DE]:[\\/]',            # dischi di lavoro
    '@gmail\.com', '@hotmail\.', '@outlook\.', '@libero\.it'
)
$self = 'tools/check_dati_personali.ps1'
$hits = @()
foreach ($f in (git ls-files)) {
    if ($f -eq $self) { continue }
    if ($f -match '\.(png|jpg|jpeg|wav|ttf|otf|bin|ico|icns)$') { continue }
    $n = 0
    foreach ($line in [IO.File]::ReadLines((Join-Path (Get-Location) $f))) {
        $n++
        foreach ($p in $patterns) { if ($line -match $p) { $hits += "${f}:${n}: $($line.Trim())"; break } }
    }
}
if ($hits.Count -gt 0) {
    Write-Output "DATI PERSONALI / PERCORSI LOCALI TROVATI:"
    $hits | ForEach-Object { Write-Output "  $_" }
    exit 1
}
Write-Output "Nessun dato personale nei file versionati."
exit 0
