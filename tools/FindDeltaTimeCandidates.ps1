param(
    [Parameter(Mandatory = $true)]
    [string[]] $InputPath,

    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$deltaPattern = '(?i)\b(?:DeltaTime|DeltaSeconds|DeltaSecond|fDeltaTime|mCurrentDeltaTime|DeltaSec)\b'
$rawNumericStepPattern = '(?i)__NFUN_(?:161|162|184|185)__\s*\([^\r\n,]+,\s*(?:float\()?[-+]?\d+(?:\.\d+)?\)?\s*\)'
$frameOperationPattern = '(?i)__NFUN_(?:165|166)__\s*\([^\r\n)]*(?:Frame|Counter|Count|Timer|Wait|Intvl|Interval|Time)[^\r\n)]*\)'
$floatPattern = '(?<![A-Za-z0-9_])[-+]?\d+\.\d+(?![A-Za-z0-9_])'
$motionPattern = '(?i)\b(?:MoveSmooth|SetLocation|Velocity|Acceleration|SetRotation|FaceRotation|RotationToTarget|AddLocation|SetPhysics)\b'

$rows = foreach ($input in $InputPath) {
    $resolved = (Resolve-Path -LiteralPath $input).Path
    $sourceName = [IO.Path]::GetFileNameWithoutExtension($resolved) -replace '_functions$', ''
    $text = Get-Content -LiteralPath $resolved -Raw
    $blocks = [regex]::Split($text, '(?m)(?=^// ===== )') |
        Where-Object { $_ -match '^// ===== ' }

    foreach ($block in $blocks) {
        $deltaMatches = [regex]::Matches($block, $deltaPattern)
        if ($deltaMatches.Count -eq 0) {
            continue
        }

        $firstLine = ($block -split "`r?`n", 2)[0]
        $functionPath = $firstLine -replace '^// ===== ', '' -replace ' =====$', ''
        $lines = $block -split "`r?`n"
        $deltaSymbols = @($deltaMatches | ForEach-Object { $_.Value } | Sort-Object -Unique)
        $rawSteps = @($lines | Where-Object { $_ -match $rawNumericStepPattern } |
            ForEach-Object { $_.Trim() } | Sort-Object -Unique)
        $frameOps = @($lines | Where-Object { $_ -match $frameOperationPattern } |
            ForEach-Object { $_.Trim() } | Sort-Object -Unique)
        $constants = @([regex]::Matches($block, $floatPattern) |
            ForEach-Object { $_.Value } | Sort-Object {[double]$_} -Unique)

        $withoutDeclarations = $block -replace '(?mi)^\s*local\s+float\s+(?:DeltaTime|DeltaSeconds|DeltaSecond|fDeltaTime|mCurrentDeltaTime|DeltaSec)\s*;\s*$', ''
        $deltaUsedOutsideDeclaration = [regex]::IsMatch($withoutDeclarations, $deltaPattern)
        $flags = @()
        if ($rawSteps.Count -ne 0) { $flags += 'raw-numeric-compound-step' }
        if ($frameOps.Count -ne 0) { $flags += 'frame-like-increment/decrement' }
        if (-not $deltaUsedOutsideDeclaration) { $flags += 'delta-declared-but-not-read' }
        if (-not $deltaUsedOutsideDeclaration -and $block -match $motionPattern) {
            $flags += 'motion-logic-without-delta-read'
        }
        if ($block -match '(?mi)^\s*native\b') { $flags += 'native-stub' }

        [pscustomobject]@{
            Source = $sourceName
            Function = $functionPath
            DeltaSymbols = $deltaSymbols -join '; '
            DeltaOccurrences = $deltaMatches.Count
            Flags = $flags -join '; '
            RawNumericSteps = $rawSteps -join ' | '
            FrameLikeOperations = $frameOps -join ' | '
            FloatConstants = $constants -join '; '
        }
    }
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$allPath = Join-Path $OutputDirectory 'DELTA_TIME_FUNCTIONS.tsv'
$candidatePath = Join-Path $OutputDirectory 'RAW_FRAME_STEP_CANDIDATES.tsv'

$rows = @($rows | Sort-Object Source, Function)
$rows | Export-Csv -LiteralPath $allPath -Delimiter "`t" -NoTypeInformation -Encoding utf8
$candidateRows = @($rows | Where-Object {
    $_.Flags -match 'raw-numeric-compound-step|frame-like-increment/decrement|delta-declared-but-not-read'
})
$candidateRows | Export-Csv -LiteralPath $candidatePath -Delimiter "`t" -NoTypeInformation -Encoding utf8

$summary = @(
    '# DeltaTime cooked-function filter',
    '',
    "Generated from $($InputPath.Count) decompiled cooked script packages.",
    '',
    "- DeltaTime-related functions: $($rows.Count)",
    "- Mechanically flagged candidates: $($candidateRows.Count)",
    "- Raw numeric compound-step functions: $(@($rows | Where-Object { $_.Flags -match 'raw-numeric-compound-step' }).Count)",
    "- Frame-like increment/decrement functions: $(@($rows | Where-Object { $_.Flags -match 'frame-like-increment/decrement' }).Count)",
    "- Delta declared but not read in decompiled body: $(@($rows | Where-Object { $_.Flags -match 'delta-declared-but-not-read' }).Count)",
    "- Motion-related bodies without a DeltaTime read: $(@($rows | Where-Object { $_.Flags -match 'motion-logic-without-delta-read' }).Count)",
    '',
    'These flags are an intentionally broad mechanical filter, not proof of a bug. Constants used as coordinate offsets, clamps, state numbers, or one-shot transitions require behavioral review.'
)
$summary | Set-Content -LiteralPath (Join-Path $OutputDirectory 'FILTER_SUMMARY.md') -Encoding utf8

Write-Output "Wrote $($rows.Count) DeltaTime functions to $allPath"
Write-Output "Wrote $($candidateRows.Count) flagged functions to $candidatePath"
