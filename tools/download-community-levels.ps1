<#
.SYNOPSIS
  Catalogs a community level download category (by default the TresCom levels page, the source of the development corpus)
  and downloads what can be downloaded without a browser. Nothing downloaded is run.

.PARAMETER OutputRoot
  The folder of the corpus: downloads, metadata, COMMUNITY_LEVELS_CATALOG.md and .json.

.PARAMETER RefreshCatalog
  Reads every page again and looks for files that changed; a changed file is kept beside the earlier one.

.PARAMETER RetryFailed
  Tries again what failed, was unavailable or needed a person.

.PARAMETER Exclusions
  The local exclusions file (default: $env:JPT_CORPUS_EXCLUSIONS or corpus_exclusions.json in OutputRoot). Excluded entries
  are never downloaded (docs/PROVENANCE.md).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputRoot,
    [string]$Source = 'https://www.trescom.org/download-category/levels/',
    [string[]]$Only = @(),
    [switch]$Resume,
    [switch]$RefreshCatalog,
    [switch]$RetryFailed,
    [switch]$CatalogOnly,
    [string]$Exclusions = ''
)

$arguments = @('-3', '-B', (Join-Path $PSScriptRoot 'download-community-levels.py'), '--output-root', $OutputRoot, '--source', $Source)
if ($Only) { $arguments += @('--only', ($Only -join ',')) }
if ($Resume) { $arguments += '--resume' }
if ($RefreshCatalog) { $arguments += '--refresh-catalog' }
if ($RetryFailed) { $arguments += '--retry-failed' }
if ($CatalogOnly) { $arguments += '--catalog-only' }
if ($Exclusions) { $arguments += @('--exclusions', $Exclusions) }
& py @arguments
exit $LASTEXITCODE
