param(
  [string]$Companion = 'build-client/release/nebula_companion.exe',
  [switch]$UseEdgeDevTools
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
Push-Location -LiteralPath $repo
try {
  $env:YANTU_COMPANION_EXE = (Resolve-Path -LiteralPath $Companion).Path
  $scratch = Join-Path $repo ('build-function3/tls-study-' + [Guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $scratch | Out-Null
  $certPath = Join-Path $scratch 'synthetic-cert.pem'
  $keyPath = Join-Path $scratch 'synthetic-key.pem'
  $rsa = [System.Security.Cryptography.RSA]::Create(2048)
  try {
    $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
      'CN=localhost', $rsa, [System.Security.Cryptography.HashAlgorithmName]::SHA256,
      [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $names = [System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
    $names.AddDnsName('localhost')
    $request.CertificateExtensions.Add($names.Build())
    $usage = [System.Security.Cryptography.OidCollection]::new()
    [void]$usage.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.1'))
    $request.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($usage, $false))
    $cert = $request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-2), [DateTimeOffset]::UtcNow.AddHours(1))
    [IO.File]::WriteAllText($certPath, $cert.ExportCertificatePem())
    [IO.File]::WriteAllText($keyPath, $rsa.ExportPkcs8PrivateKeyPem())
    $env:YANTU_COMPANION_CERT = $certPath
    $env:YANTU_COMPANION_KEY = $keyPath
    $env:YANTU_COMPANION_PIN = [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($cert.RawData)).ToLowerInvariant()
    $browserTest = if ($UseEdgeDevTools) { 'tests/client/yantu_native_edge.cjs' } else { 'tests/client/yantu_account_browser.cjs' }
    node $browserTest
    $testExit = $LASTEXITCODE
  } finally {
    if ($cert) { $cert.Dispose() }
    $rsa.Dispose()
    Remove-Item -LiteralPath $certPath,$keyPath -ErrorAction SilentlyContinue
  }
} finally {
  Pop-Location
}
exit $testExit
