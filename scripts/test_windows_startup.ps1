param(
  [Parameter(Mandatory = $true)]
  [string]$Executable,
  [string]$ExpectedVersion = ""
)

$ErrorActionPreference = "Stop"
$exe = (Resolve-Path -LiteralPath $Executable).Path
$ExpectedVersion = $ExpectedVersion -replace "^v", ""

foreach ($argument in @("--help", "--version")) {
  $startInfo = New-Object System.Diagnostics.ProcessStartInfo
  $startInfo.FileName = $exe
  $startInfo.Arguments = $argument
  $startInfo.WorkingDirectory = Split-Path -Parent $exe
  $startInfo.UseShellExecute = $false
  $startInfo.CreateNoWindow = $true
  $startInfo.RedirectStandardOutput = $true
  $startInfo.RedirectStandardError = $true
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $startInfo
  try {
    if (-not $process.Start()) { throw "Could not start $exe" }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
      $process.Kill()
      throw "$exe $argument did not exit within 30 seconds"
    }
    $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
    Write-Host $output
    if ($process.ExitCode -ne 0) {
      throw "$exe $argument exited with $($process.ExitCode)"
    }
    if ($argument -eq "--help" -and $output -notmatch "Usage:") {
      throw "$exe did not print command-line help"
    }
    if ($argument -eq "--version" -and $ExpectedVersion -and -not $output.Contains($ExpectedVersion)) {
      throw "$exe did not report expected version $ExpectedVersion"
    }
  } finally {
    $process.Dispose()
  }
}
