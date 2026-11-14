Voice clips for Otto (German) go in this folder.

16-bit PCM WAV, mono, 16 kHz. Any missing clip plays a short two-note chirp instead, so they can
be added one at a time.

  morgen.wav        Guten Morgen!               (morning)
  tag.wav           Guten Tag!                  (daytime)
  abend.wav         Guten Abend!                (evening)
  willkommen.wav    Willkommen zurück!          (anytime)
  hallo.wav         Hallo!                      (anytime)
  losgehts.wav      Los geht's!                 (anytime, drag tree armed)
  bereit.wav        Bereit.                     (anytime)
  muede.wav         Müde...                     (late night)
  sehrgut.wav       Sehr gut.                   (10 min of smooth driving)

Holidays:
  geburtstag.wav    Alles Gute zum Geburtstag!  (birthday)
  neujahr.wav       Prost Neujahr!              (Jan 1)
  patent.wav        Alles Gute, Automobil!      (Jan 29: the car's birthday)
  liebe.wav         Alles Liebe!                (Valentine's)
  glueckwunsch.wav  Herzlichen Glückwunsch!     (July 4th, drive milestones)
  halloween.wav     Süßes oder Saures!          (Halloween)
  erntedank.wav     Frohes Erntedankfest!       (Thanksgiving)
  weihnachten.wav   Frohe Weihnachten!          (Christmas)
  rutsch.wav        Guten Rutsch!               (Dec 31)

Extras:
  rekord.wav        Neuer Rekord!               (new best time)
  langenicht.wav    Lange nicht gesehen!        (parked 7+ days)
  pause.wav         Zeit für eine Pause?        (2 h drive)
  heiss.wav         Heiß!                       (hot cabin)
  kalt.wav          Kalt!                       (cold cabin)

OBD-II (with the adapter):
  nochkalt.wav      Noch kalt. Sanft, bitte.    (cold engine after a start: go easy)
  warm.wav          Schön warm.                 (engine reached operating temperature)

Generate them all with a Windows German voice (add one first: Settings > Time & language >
Speech > Add voices > Deutsch (Deutschland)), from the repo root:

  powershell -ExecutionPolicy Bypass -File tools\make_clips.ps1            (first German voice)
  powershell -ExecutionPolicy Bypass -File tools\make_clips.ps1 -Voice Stefan

Or convert your own recordings with ffmpeg:
  ffmpeg -i input.m4a -ac 1 -ar 16000 -sample_fmt s16 morgen.wav

Upload to the board (separate from the firmware):
  pio run -t uploadfs
