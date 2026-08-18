# Deskwork

**An unnecessarily tiny solar dynamic wallpaper daemon for KDE Plasma, written in C.**

Deskwork changes your desktop wallpaper throughout the day based on the real position of the Sun.

Instead of using fixed times such as "switch to the evening wallpaper at 18:00", Deskwork calculates solar events from your location and the current date.

The same wallpaper set can therefore behave differently depending on where you are and the time of year.

No GUI. No database of sunrise times. No constant polling.

Calculate. Change wallpaper. Sleep.

## How it works

Deskwork uses latitude, longitude and the current date to calculate the position of the Sun.

The day is divided into eight visual phases:

| Phase | Name | Solar condition |
|------:|------|----------------|
| 1 | Night | Sun below approximately -18° |
| 2 | Dawn | Morning crossing around -12° |
| 3 | Civil Dawn | Morning crossing around -6° |
| 4 | Morning | Sunrise, around -0.833° |
| 5 | Day | Sun rising through +15° |
| 6 | Evening | Sun descending through +15° |
| 7 | Sunset | Sunset, around -0.833° |
| 8 | Dusk | Evening crossing around -6° |

The exact switching times therefore change naturally throughout the year.

Deskwork calculates the current phase, sets the corresponding wallpaper and sleeps until another check is required.

## Wallpaper set

Deskwork expects eight images.

For example:

```text
Deskwork_wallpapers/
├── 01-night.png
├── 02-dawn.png
├── 03-civil-dawn.png
├── 04-morning.png
├── 05-day.jpg
├── 06-evening.png
├── 07-sunset.png
└── 08-dusk.png
```

JPEG, PNG, WEBP and JPEG files are supported.

The wallpapers do not need to depict the same scene. You can create any set you want as long as the images represent the eight phases of the day.

Deskwork itself does not distribute a wallpaper pack.

## Requirements

Deskwork currently targets Linux with KDE Plasma.

You need:

- a C compiler such as GCC
- KDE Plasma
- `qdbus6` or `qdbus`
- `curl` for automatic IP-based location detection
- systemd if you want automatic startup

## Compile

Clone the repository and compile Deskwork:

```bash
gcc -O2 -Wall -Wextra -std=c11 deskwork.c -o deskwork -lm
```

Create the Deskwork directory:

```bash
mkdir -p ~/.local/bin/deskwork
```

Move the executable there:

```bash
cp deskwork ~/.local/bin/deskwork/
```

You can optionally keep the source there too:

```bash
cp deskwork.c ~/.local/bin/deskwork/
```

## Configuration

Create the configuration directory:

```bash
mkdir -p ~/.config/deskwork
```

Copy the example configuration:

```bash
cp config.example.conf ~/.config/deskwork/config.conf
```

Then edit:

```text
~/.config/deskwork/config.conf
```

Example:

```ini
wallpaper_dir = "/home/USER/Pictures/Deskwork_wallpapers"
cache_file = "/home/USER/.cache/deskwork/location.cache"
```

Change `USER` and the wallpaper directory to match your system.

## Location

Deskwork can automatically attempt to determine your approximate location using IP geolocation.

When successful, the coordinates are saved locally.

If automatic location detection later fails — for example because the computer is offline — Deskwork uses the last successfully saved location.

You can manually set the saved location:

```bash
deskwork --set-location LATITUDE LONGITUDE
```

Example:

```bash
deskwork --set-location 45.0 9.0
```

You can also temporarily run Deskwork using another location without changing the saved coordinates:

```bash
deskwork --location LATITUDE LONGITUDE
```

Solar calculations themselves are performed locally.

## Commands

### Start Deskwork

```bash
deskwork
```

### Show status

```bash
deskwork --status
```

Displays the current location, calculated solar events, current phase and next wallpaper change.

### Reset

```bash
deskwork --reset
```

Forces the running Deskwork process to reload its state and recalculate location, date, time and solar events.

Useful after changing location, system time or configuration.

### Force a phase

Useful for testing wallpaper sets:

```bash
deskwork --phase 3
```

or:

```bash
deskwork --phase civil-dawn
```

Available phases:

```text
1  night
2  dawn
3  civil-dawn
4  morning
5  day
6  evening
7  sunset
8  dusk
```

### Help

```bash
deskwork --help
```

or:

```bash
deskwork -h
```

## Automatic startup

Deskwork includes a systemd user service.

Copy it:

```bash
mkdir -p ~/.config/systemd/user
cp deskwork.service ~/.config/systemd/user/
```

Reload systemd:

```bash
systemctl --user daemon-reload
```

Enable and start Deskwork:

```bash
systemctl --user enable --now deskwork.service
```

Check its status:

```bash
systemctl --user status deskwork.service
```

Once enabled, Deskwork runs independently of your terminal and starts automatically with your user session.

## Design philosophy

Deskwork is intentionally small.

It does not need:

- a GUI
- a tray icon
- a database containing sunrise and sunset times
- a background loop constantly checking the clock
- separate seasonal wallpaper packs

The same eight wallpapers can be used throughout the entire year.

Only their switching times change.

The goal is simply:

```text
location + date
       ↓
solar calculation
       ↓
current phase
       ↓
wallpaper
       ↓
sleep
```

## License

Deskwork is released under the MIT License.

See `LICENSE` for details.
