# Cubesat Hunter (sat_tracker)

Satellite list, pass prediction, automatic tuning with Doppler correction and a radar overlay on
the spectrum, for desktop and Android.

## Data
- Transmitters (frequency, mode, baud, service, status) and names: SatNOGS DB
  (`db.satnogs.org/api/transmitters`, `/api/satellites`).
- Orbits: SatNOGS DB TLEs plus the CelesTrak amateur group; the newer element set wins.
- Cached in `<root>/sat_tracker/`, refreshed automatically every N days (default 3) or with
  **Update data**. Desktop downloads with `curl` (Windows 10+ ships `curl.exe`), Android with
  `HttpURLConnection`.

## Use
1. Set the ground station: latitude / longitude / altitude or a Maidenhead locator; on Windows
   **Locate (Windows)** (Location Services, Wi-Fi based without GPS), anywhere **Locate by IP**
   (city level, only when pressed); on Android **Follow phone GPS**.
2. Filter by band (2 m, 70 cm, 23 cm, S-band 2.2 GHz, 13 cm 2.4 GHz, HF, other), amateur only,
   active only, minimum pass elevation, search text (name, NORAD number, mode).
3. Tap a satellite: the first matching downlink is tuned, the demodulator is set from the mode
   (FM / AFSK -> NFM 16 kHz, linear transponder / SSB -> USB, CW -> CW, data -> NFM wide enough
   for the baud rate) and Doppler tracking starts. Below the horizon the receiver waits on the
   AOS frequency.
4. Tuning by hand while tracking (waterfall, frequency box) is kept as an offset in the
   satellite's frame, so tracking continues across a linear transponder. **Reset** clears it.
5. The radar (top right of the spectrum, tap to enlarge) shows the pass track (green AOS, red
   LOS) and the satellite. Android **Compass**: phone flat, the radar turns with the phone; phone
   raised, a crosshair shows where the back of the phone points, with "right / up" guidance and
   ON TARGET within 10 deg. Magnetic declination comes from the phone's position.

Accuracy: Doppler and look angles match Skyfield to 1e-3 deg / 1e-5 km/s; AOS / LOS to 1 s.

## Third-party code
`src/sgp4/`: libsgp4 by Daniel Warner (https://github.com/dnwrnr/sgp4, commit c6ebb90),
Apache License 2.0, see `src/sgp4/LICENSE`. `CsvTleLoader` left out.

## Classroom demo
**Simulate this pass now** shifts the module's clock to just before the next AOS of the selected
satellite (radar shows SIMULATED; back to real time automatically after LOS or with **Real time**).
With the **Demo (RTL-SDR)** source the satellite's downlink is synthesised at its real Doppler
frequency (FM voice-like tones, CW, AFSK/FSK bursts or SSB/CW users on a linear transponder),
stronger at high elevation, so Doppler tracking can be shown without an antenna.
