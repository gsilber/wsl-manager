# Regenerates installer\License.rtf (shown by the MSI's licence page) from LICENSE.
# Run after changing LICENSE:  powershell -ExecutionPolicy Bypass -File installer\make-license-rtf.ps1
#
# LICENSE is hard-wrapped at ~72 columns; the installer's licence box is narrower,
# so lines within a paragraph are joined and only blank lines start new paragraphs.

$root = Split-Path $PSScriptRoot -Parent
$text = [IO.File]::ReadAllText((Join-Path $root 'LICENSE')) -replace "`r", ''

$paragraphs = ($text.Trim() -split "\n[ \t]*\n") | ForEach-Object {
    $joined = (($_ -split "\n") | ForEach-Object { $_.Trim() }) -join ' '
    # Escape RTF control characters; the GPL text is plain ASCII otherwise.
    $joined.Replace('\', '\\').Replace('{', '\{').Replace('}', '\}')
}

$rtf = New-Object Text.StringBuilder
[void]$rtf.Append('{\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\fswiss\fcharset0 Segoe UI;}}')
[void]$rtf.Append('\viewkind4\uc1\pard\sa120\f0\fs16 ')
[void]$rtf.Append('\b ' + $paragraphs[0] + '\b0\par' + "`r`n")
for ($i = 1; $i -lt $paragraphs.Count; $i++) {
    [void]$rtf.Append($paragraphs[$i] + '\par' + "`r`n")
}
[void]$rtf.Append('}')

[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'License.rtf'), $rtf.ToString(), [Text.Encoding]::ASCII)
Write-Host "Wrote installer\License.rtf ($($paragraphs.Count) paragraphs)"
