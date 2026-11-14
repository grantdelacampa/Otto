# Generate Otto's voice clips with a Windows German TTS voice, into data/clips/.
# Needs: Windows Settings > Time & language > Speech > Add voices > Deutsch (Deutschland).
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File tools\make_clips.ps1 [-Voice Stefan]
# Then upload to the board: pio run -t uploadfs
#
# Output is 16 kHz mono 16-bit WAV, which is what the firmware plays. Any clip that's missing on
# the board plays a short chirp instead, so this can be done any time.
param([string]$Voice = "", [double]$Rate = 1.0, [double]$Pitch = 0.9)   # "" = first German voice; a bit lower = unflappable
$clips = [ordered]@{
  "morgen"       = "Guten Morgen!"               # morning
  "tag"          = "Guten Tag!"                  # daytime
  "abend"        = "Guten Abend!"                # evening
  "willkommen"   = "Willkommen zurück!"          # anytime: welcome back
  "hallo"        = "Hallo!"                      # anytime
  "losgehts"     = "Los geht's!"                 # anytime + drag tree armed
  "bereit"       = "Bereit."                     # anytime: ready
  "muede"        = "Müde..."                     # late night: sleepy
  "geburtstag"   = "Alles Gute zum Geburtstag!"  # birthday
  "neujahr"      = "Prost Neujahr!"              # January 1
  "rutsch"       = "Guten Rutsch!"               # December 31
  "liebe"        = "Alles Liebe!"                # Valentine's Day
  "glueckwunsch" = "Herzlichen Glückwunsch!"     # July 4th, drive milestones
  "halloween"    = "Süßes oder Saures!"          # Halloween
  "erntedank"    = "Frohes Erntedankfest!"       # Thanksgiving
  "weihnachten"  = "Frohe Weihnachten!"          # Christmas
  "patent"       = "Alles Gute, Automobil!"      # January 29: Benz's 1886 patent
  "rekord"       = "Neuer Rekord!"               # drag race: new best
  "langenicht"   = "Lange nicht gesehen!"        # parked 7+ days
  "pause"        = "Zeit für eine Pause?"        # 2 h drive
  "heiss"        = "Heiß!"                       # hot cabin
  "kalt"         = "Kalt!"                       # cold cabin
  "sehrgut"      = "Sehr gut."                   # 10 minutes of smooth driving
  "nochkalt"     = "Noch kalt. Sanft, bitte."    # OBD: cold engine after a start
  "warm"         = "Schön warm."                 # OBD: reached operating temperature
}

# Loudness: raw TTS sits around -25 dBFS RMS, ~11 dB under the firmware's chirps (sine at 9000 =
# -14 dBFS RMS), so clips sounded much quieter than beeps. Boost each clip to -14 dBFS RMS and
# soft-limit the peaks (tanh) so nothing clips.
$TargetRmsDb = -14.0
Add-Type -TypeDefinition @"
using System;
public static class ClipLevel {
  // returns the applied gain in dB
  public static double Normalize(string path, double targetRmsDb) {
    byte[] b = System.IO.File.ReadAllBytes(path);
    int data = 12, len = 0;
    while (data + 8 <= b.Length) {                    // find the "data" chunk
      string id = System.Text.Encoding.ASCII.GetString(b, data, 4);
      int sz = BitConverter.ToInt32(b, data + 4);
      if (id == "data") { len = sz; data += 8; break; }
      data += 8 + sz + (sz & 1);
    }
    int n = Math.Min(len, b.Length - data) / 2;
    double sum = 0;
    for (int i = 0; i < n; i++) { double x = BitConverter.ToInt16(b, data + 2 * i) / 32768.0; sum += x * x; }
    double rms = Math.Sqrt(sum / Math.Max(1, n));
    double gain = Math.Pow(10, targetRmsDb / 20) / Math.Max(rms, 1e-6);
    for (int i = 0; i < n; i++) {
      double x = BitConverter.ToInt16(b, data + 2 * i) / 32768.0 * gain;
      short y = (short)Math.Round(Math.Tanh(x) * 32000);  // soft limit, never hard-clips
      b[data + 2 * i] = (byte)(y & 0xFF); b[data + 2 * i + 1] = (byte)((y >> 8) & 0xFF);
    }
    System.IO.File.WriteAllBytes(path, b);
    return 20 * Math.Log10(gain);
  }
}
"@

$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "data\clips"
foreach ($name in $clips.Keys) {
  & (Join-Path $PSScriptRoot "tts.ps1") -Voice $Voice -Lang "de-DE" -Text $clips[$name] -Rate $Rate -Pitch $Pitch `
      -Out (Join-Path $out "$name.wav")
  $db = [ClipLevel]::Normalize((Join-Path $out "$name.wav"), $TargetRmsDb)
  "  {0}: boosted {1:N1} dB" -f $name, $db
}
