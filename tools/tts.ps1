# Text to speech -> WAV via the WinRT (OneCore) voices. Otto speaks German (de-DE).
# Usage: tts.ps1 -Voice Katja -Text "Guten Morgen!" -Out C:\path\morgen.wav [-Lang de-DE] [-Rate 1.0] [-Pitch 1.0]
#        tts.ps1 -List
param([string]$Voice = "", [string]$Text = "", [string]$Out = "", [string]$Lang = "de-DE",
      [double]$Rate = 1.0, [double]$Pitch = 1.0, [switch]$List)

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Media.SpeechSynthesis.SpeechSynthesizer, Windows.Media.SpeechSynthesis, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.DataReader, Windows.Storage.Streams, ContentType = WindowsRuntime]

$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
  $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
function Await($op, [Type]$t) {
  $task = $asTask.MakeGenericMethod($t).Invoke($null, @($op))
  $task.Wait() | Out-Null
  $task.Result
}

$all = [Windows.Media.SpeechSynthesis.SpeechSynthesizer]::AllVoices
if ($List) { $all | ForEach-Object { "$($_.DisplayName) | $($_.Language) | $($_.Gender)" }; return }

$v = $all | Where-Object { $_.Language -eq $Lang -and $_.DisplayName -match $Voice } | Select-Object -First 1
if (-not $v) { throw "No $Lang voice matching '$Voice' (Settings > Time & language > Speech > Add voices)" }
$s = New-Object Windows.Media.SpeechSynthesis.SpeechSynthesizer
$s.Voice = $v
$s.Options.SpeakingRate = $Rate
$s.Options.AudioPitch = $Pitch
$stream = Await ($s.SynthesizeTextToStreamAsync($Text)) ([Windows.Media.SpeechSynthesis.SpeechSynthesisStream])
$size = [uint32]$stream.Size
$reader = New-Object Windows.Storage.Streams.DataReader($stream.GetInputStreamAt(0))
$null = Await ($reader.LoadAsync($size)) ([uint32])
$bytes = New-Object byte[] $size
$reader.ReadBytes($bytes)
[System.IO.File]::WriteAllBytes($Out, $bytes)
"$Out ($size bytes, $($v.DisplayName))"
