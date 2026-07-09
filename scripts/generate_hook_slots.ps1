param(
    [string]$SlotsDef = (Join-Path $PSScriptRoot "..\src\hooks\slots.def"),
    [string]$Output = (Join-Path $PSScriptRoot "..\src\hooks\tweak_hooks_slots.inc")
)

$lines = Get-Content -Path $SlotsDef | Where-Object { $_ -match '^\s*JST_HOOK_SLOT\((\w+)\)\s*$' }
$slots = foreach ($line in $lines) {
    if ($line -match '^\s*JST_HOOK_SLOT\((\w+)\)\s*$') {
        [PSCustomObject]@{
            Name = $Matches[1]
        }
    }
}

if (-not $slots -or $slots.Count -eq 0) {
    throw "No JST_HOOK_SLOT entries found in $SlotsDef"
}

$builder = New-Object System.Text.StringBuilder
[void]$builder.AppendLine("; AUTO-GENERATED from slots.def -- do not edit")
[void]$builder.AppendLine("; Regenerate: scripts/generate_hook_slots.ps1")
[void]$builder.AppendLine("")

$index = 0
foreach ($slot in $slots) {
    $macroName = "SLOT_$($slot.Name.ToUpper())"
    [void]$builder.AppendLine("$macroName EQU $index")
    ++$index
}

$content = $builder.ToString().TrimEnd() + [Environment]::NewLine

# If another project's pre-build step already wrote the same content,
# we can skip the write entirely and avoid the lock contention.
$skipWrite = $false
if (Test-Path -LiteralPath $Output) {
    try {
        $existing = [System.IO.File]::ReadAllText($Output)
        if ($existing -ceq $content) {
            $skipWrite = $true
        }
    } catch {
        # File not readable (locked or missing) — fall through to write.
    }
}

if (-not $skipWrite) {
    $maxAttempts = 3
    $attempt = 0
    $written = $false
    while (-not $written -and $attempt -lt $maxAttempts) {
        ++$attempt
        try {
            Set-Content -Path $Output -Value $content -NoNewline -Encoding ASCII
            $written = $true
        } catch {
            if ($attempt -eq $maxAttempts) {
                throw
            }
            Start-Sleep -Milliseconds 200
        }
    }
}

Write-Host "Generated $Output with $($slots.Count) slot(s)."