[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..')
)
$violations = New-Object System.Collections.Generic.List[string]

function Add-Violation {
    param([Parameter(Mandatory)][string]$Message)

    if (-not $violations.Contains($Message)) {
        $violations.Add($Message)
    }
}

$trackedFiles = @(& git -C $repoRoot ls-files)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to enumerate tracked repository files.'
}

$allowedAuthoredAssets = @(
    'maps/blender_bake_showcase.blend',
    'maps/blender_bake_showcase.skate'
)
$forbiddenDirectories = @(
    'out/',
    'build/',
    'generated/',
    'game/',
    'runtime/',
    'dlc/',
    'saves/',
    'cache/',
    'logs/',
    '.cel-update/',
    '.cel-steam/'
)
$forbiddenExtensions = @(
    '.7z',
    '.bat',
    '.con',
    '.dll',
    '.dmp',
    '.exe',
    '.iso',
    '.live',
    '.log',
    '.obj',
    '.pdb',
    '.pirs',
    '.rar',
    '.stfs',
    '.xex',
    '.xexp',
    '.zip'
)
$forbiddenNames = @(
    'steam_api64.dll',
    'steam_appid.txt',
    'skate3.toml',
    'CMakeUserPresets.json'
)

foreach ($rawPath in $trackedFiles) {
    $path = $rawPath.Replace('\', '/')
    $lower = $path.ToLowerInvariant()

    foreach ($directory in $forbiddenDirectories) {
        if ($lower.StartsWith(
                $directory,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            Add-Violation "Local/generated directory is tracked: $path"
        }
    }

    $extension = [System.IO.Path]::GetExtension($path).ToLowerInvariant()
    if ($forbiddenExtensions -contains $extension) {
        Add-Violation "Forbidden generated/binary file is tracked: $path"
    }

    if (($extension -eq '.blend' -or $extension -eq '.skate') -and
        $allowedAuthoredAssets -notcontains $path) {
        Add-Violation "Unreviewed authored asset is tracked: $path"
    }

    if ($forbiddenNames -contains [System.IO.Path]::GetFileName($path)) {
        Add-Violation "Machine-local configuration is tracked: $path"
    }
}

$contentRules = @(
    @{
        Name = 'personal Windows user path'
        Pattern = '[A-Za-z]:[\\/]+Users[\\/]+[^\\/]+[\\/]'
    },
    @{
        Name = 'personal macOS user path'
        Pattern = '/Users/[^/]+/'
    },
    @{
        Name = 'GitHub token signature'
        Pattern = '(gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})'
    },
    @{
        Name = 'AWS access-key signature'
        Pattern = 'AKIA[0-9A-Z]{16}'
    },
    @{
        Name = 'private-key material'
        Pattern = '-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----'
    },
    @{
        Name = 'Discord webhook'
        Pattern = 'discord(app)?\.com/api/webhooks/'
    }
)

foreach ($rule in $contentRules) {
    $matches = @(
        & git -C $repoRoot grep -I -l -E -e $rule.Pattern -- . `
            ':(exclude)scripts/Test-RepositoryHygiene.ps1' 2>$null
    )
    $grepExit = $LASTEXITCODE
    if ($grepExit -gt 1) {
        throw "Repository content scan failed for $($rule.Name)."
    }
    foreach ($path in $matches) {
        Add-Violation "$($rule.Name) found in tracked file: $path"
    }
}

if ($violations.Count -gt 0) {
    Write-Error (
        "Repository hygiene failed:`n- " +
        ($violations -join "`n- ")
    )
    exit 1
}

Write-Host (
    "Repository hygiene passed for $($trackedFiles.Count) tracked files."
)
exit 0
