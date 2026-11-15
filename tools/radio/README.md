# Radio Studio

This tool makes the car radio's DJ breaks and Mantis Motors ads. It is a single file, `radio_studio.py`, and needs only Python 3. tkinter comes with Python. numpy is optional and makes it faster.

```
python3 radio_studio.py                                  # the window
python3 radio_studio.py --new radio_project.json         # a project with every script slot, no window
python3 radio_studio.py --build radio_project.json E:\   # builds the card from a project, no window
```

## Making the recordings

1. Every row is a slot: a station, a kind of line, and a script to read. Edit the scripts as you like. **Add slot** adds more lines, and **Remove slot** drops one.
2. Record each line however you like (phone, laptop, DAW). Read it in character, and leave a little silence at each end. The tool trims it.
3. Select the row, then **Load recording...**. WAV files always work. mp3, m4a, ogg and flac work when ffmpeg is installed.
4. **Play as the radio** plays exactly what the car will play: the radio EQ, the compression and the 4-bit squeeze.
5. **Save** the project (a .json file listing the scripts and recordings).
6. **Build SD card...** and pick the card. This writes `radio/radio.bin` and `radio/manifest.txt`. The game reads the bank once at boot, into PSRAM. It never touches the card while playing.

**Rate:** 6000 Hz is the smallest, about 3 KB per second of voice. 8000 Hz is clearer, about 4 KB per second. The bar at the top shows the estimated size against the 1 MB budget. All 49 default lines at about 5 s each come to about 0.7 MB at 6000 Hz.

## What plays when

- **Station ID:** every fourth break.
- **Talk:** most other breaks.
- **Morning / Daytime / Evening / Night:** chosen by the game's time of day.
- **Rain / Snow / Fog / Clear skies:** chosen by the current weather.
- **Ad:** after the DJ, every third break, on any station.

If a kind has no recording, the station falls back to a Talk line, then to an ID. A slot set to "Any station" plays on all three stations. Without a card or without the file, each station plays its own short musical ident between songs.

## The file (shared with `src/radio.cpp`)

`"MRAD"`, then `u16` version = 1 and `u16` count. That is followed by count × 16-byte entries:

```
{u8 station, u8 kind, u16 rate, u32 samples, u32 offset from the end of the table, u32 bytes}
```

Then come the clips. Each is IMA ADPCM at 4 bits a sample, low nibble first, and the decoder starts at predictor 0, index 0.

- **Station:** 0 any, 1 lo-fi, 2 math rock, 3 country.
- **Kind:** 0 ID, 1 talk, 2–5 morning/day/evening/night, 6–9 rain/snow/fog/clear, 10 ad.
- **Limits:** at most 160 clips. The game reads at most 1.5 MB.
