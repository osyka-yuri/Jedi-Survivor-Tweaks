param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ExePath
)

$ErrorActionPreference = 'Stop'

$expectedTimestamp = 0x66DF3498
$expectedImageSize = 0x0722A000
$expectedTickRva = 0x00A4BB40
$executeFlag = 0x20000000
$signature = [byte[]](
    0x40, 0xBB, 0xA4, 0x00, 0x96, 0xCB, 0xA4, 0x00,
    0x0C, 0xFC, 0x04, 0x06, 0xA0, 0xCB, 0xA4, 0x00,
    0xF1, 0xCB, 0xA4, 0x00, 0x74, 0xF6, 0x04, 0x06,
    0x00, 0xCC, 0xA4, 0x00, 0x34, 0xCC, 0xA4, 0x00,
    0x74, 0xF6, 0x04, 0x06, 0x40, 0xCC, 0xA4, 0x00,
    0x86, 0xCC, 0xA4, 0x00, 0xD4, 0xF6, 0x04, 0x06,
    0x90, 0xCC, 0xA4, 0x00, 0xDC, 0xCC, 0xA4, 0x00,
    0xB8, 0xF9, 0x04, 0x06
)

function Read-SectionName([System.IO.BinaryReader]$Reader) {
    $bytes = $Reader.ReadBytes(8)
    $length = [Array]::IndexOf($bytes, [byte]0)
    if ($length -lt 0) { $length = $bytes.Length }
    return [Text.Encoding]::ASCII.GetString($bytes, 0, $length)
}

$stream = [System.IO.File]::Open(
    (Resolve-Path -LiteralPath $ExePath),
    [System.IO.FileMode]::Open,
    [System.IO.FileAccess]::Read,
    [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
$reader = [System.IO.BinaryReader]::new($stream)
try {
    if ($reader.ReadUInt16() -ne 0x5A4D) {
        throw 'Invalid DOS signature.'
    }
    $stream.Position = 0x3C
    $peOffset = $reader.ReadUInt32()
    $stream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x00004550) {
        throw 'Invalid PE signature.'
    }

    $machine = $reader.ReadUInt16()
    $sectionCount = $reader.ReadUInt16()
    $timestamp = $reader.ReadUInt32()
    $stream.Position += 8
    $optionalHeaderSize = $reader.ReadUInt16()
    $stream.Position += 2
    $optionalOffset = $stream.Position
    if ($machine -ne 0x8664 -or $reader.ReadUInt16() -ne 0x020B) {
        throw 'Expected an x64 PE32+ executable.'
    }
    $stream.Position = $optionalOffset + 56
    $imageSize = $reader.ReadUInt32()

    if ($timestamp -ne $expectedTimestamp -or $imageSize -ne $expectedImageSize) {
        throw ('Unsupported game build: timestamp=0x{0:X8}, image=0x{1:X8}.' -f
            $timestamp, $imageSize)
    }

    $sections = @()
    $stream.Position = $optionalOffset + $optionalHeaderSize
    for ($index = 0; $index -lt $sectionCount; ++$index) {
        $name = Read-SectionName $reader
        $virtualSize = $reader.ReadUInt32()
        $virtualAddress = $reader.ReadUInt32()
        $rawSize = $reader.ReadUInt32()
        $rawOffset = $reader.ReadUInt32()
        $stream.Position += 12
        $characteristics = $reader.ReadUInt32()
        $sections += [pscustomobject]@{
            Name = $name
            VirtualSize = $virtualSize
            VirtualAddress = $virtualAddress
            RawSize = $rawSize
            RawOffset = $rawOffset
            Characteristics = $characteristics
        }
    }

    $pdata = $sections | Where-Object Name -eq '.pdata' | Select-Object -First 1
    if (-not $pdata -or $pdata.RawSize -lt $signature.Length) {
        throw 'The .pdata section is unavailable or too small.'
    }
    $stream.Position = $pdata.RawOffset
    $pdataBytes = $reader.ReadBytes($pdata.RawSize)
    $matches = @()
    for ($offset = 0; $offset -le $pdataBytes.Length - $signature.Length; ++$offset) {
        $equal = $true
        for ($byteIndex = 0; $byteIndex -lt $signature.Length; ++$byteIndex) {
            if ($pdataBytes[$offset + $byteIndex] -ne $signature[$byteIndex]) {
                $equal = $false
                break
            }
        }
        if ($equal) { $matches += $offset }
    }
    if ($matches.Count -ne 1) {
        throw "FEngineLoop::Tick unwind signature matched $($matches.Count) times."
    }

    $tickRva = [BitConverter]::ToUInt32($pdataBytes, $matches[0])
    if ($tickRva -ne $expectedTickRva) {
        throw ('Unexpected Tick RVA 0x{0:X8}.' -f $tickRva)
    }
    $targetSection = $sections | Where-Object {
        $span = [Math]::Max($_.VirtualSize, $_.RawSize)
        $tickRva -ge $_.VirtualAddress -and
        $tickRva -lt $_.VirtualAddress + $span
    } | Select-Object -First 1
    if (-not $targetSection -or
        ($targetSection.Characteristics -band $executeFlag) -eq 0) {
        throw 'Tick RVA is not inside an executable PE section.'
    }

    Write-Output ('Compatible JediSurvivor.exe: timestamp=0x{0:X8}, image=0x{1:X8}, Tick=0x{2:X8} ({3}).' -f
        $timestamp, $imageSize, $tickRva, $targetSection.Name)
} finally {
    $reader.Dispose()
    $stream.Dispose()
}
