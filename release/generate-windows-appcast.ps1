param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$ReleaseNotes,
    [Parameter(Mandatory = $true)][string]$BaseUrl,
    [Parameter(Mandatory = $true)][string]$PrivateKey,
    [Parameter(Mandatory = $true)][string]$ToolPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$Architecture = "windows-x64"
)

$ErrorActionPreference = "Stop"

foreach ($path in @($Installer, $ReleaseNotes, $PrivateKey, $ToolPath)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required file not found: $path"
    }
}

$installerItem = Get-Item -LiteralPath $Installer
$notesItem = Get-Item -LiteralPath $ReleaseNotes
$signature = (& $ToolPath sign --private-key-file $PrivateKey $installerItem.FullName | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $signature -notmatch '^[A-Za-z0-9+/]+={0,2}$') {
    throw "WinSparkle signing failed"
}

$base = $BaseUrl.TrimEnd('/')
$sparkleNamespace = "http://www.andymatuschak.org/xml-namespaces/sparkle"
$settings = [System.Xml.XmlWriterSettings]::new()
$settings.Indent = $true
$settings.Encoding = [System.Text.UTF8Encoding]::new($false)

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

$writer = [System.Xml.XmlWriter]::Create($OutputPath, $settings)
try {
    $writer.WriteStartDocument()
    $writer.WriteStartElement("rss")
    $writer.WriteAttributeString("xmlns", "sparkle", $null, $sparkleNamespace)
    $writer.WriteAttributeString("version", "2.0")
    $writer.WriteStartElement("channel")
    $writer.WriteElementString("title", "AegisyClient Windows Updates")
    $writer.WriteStartElement("item")
    $writer.WriteElementString("title", $Version)
    $writer.WriteElementString("pubDate", [DateTime]::UtcNow.ToString("r", [Globalization.CultureInfo]::InvariantCulture))
    $writer.WriteElementString("link", "https://aegisy.cc")
    $writer.WriteElementString("sparkle", "version", $sparkleNamespace, $Version)
    $writer.WriteElementString("sparkle", "shortVersionString", $sparkleNamespace, $Version)
    $writer.WriteElementString("sparkle", "minimumSystemVersion", $sparkleNamespace, "10.0")
    $writer.WriteElementString(
        "sparkle", "releaseNotesLink", $sparkleNamespace,
        "$base/$($notesItem.Name)")
    $writer.WriteStartElement("enclosure")
    $writer.WriteAttributeString("url", "$base/$($installerItem.Name)")
    $writer.WriteAttributeString("length", $installerItem.Length.ToString([Globalization.CultureInfo]::InvariantCulture))
    $writer.WriteAttributeString("type", "application/octet-stream")
    $writer.WriteAttributeString("sparkle", "os", $sparkleNamespace, $Architecture)
    $writer.WriteAttributeString("sparkle", "edSignature", $sparkleNamespace, $signature)
    $writer.WriteAttributeString(
        "sparkle", "installerArguments", $sparkleNamespace,
        "/CURRENTUSER /VERYSILENT /SUPPRESSMSGBOXES /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS")
    $writer.WriteEndElement()
    $writer.WriteEndElement()
    $writer.WriteEndElement()
    $writer.WriteEndElement()
    $writer.WriteEndDocument()
}
finally {
    $writer.Dispose()
}

& $ToolPath verify `
    --public-key "9DkeQEQcqz6Trrcmr1XRlZHDQHyeOj3DaVA2rtS3WA0=" `
    --signature $signature `
    $installerItem.FullName
if ($LASTEXITCODE -ne 0) {
    throw "Generated update signature could not be verified"
}

Write-Host "Generated Windows appcast: $OutputPath"
