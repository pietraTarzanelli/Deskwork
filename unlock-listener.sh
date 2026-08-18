#!/bin/bash

# Listen for KDE Plasma screen unlock events.
# When the session is unlocked, force Deskwork to recalculate
# the current location, time, solar phase and wallpaper.

dbus-monitor --session \
"type='signal',interface='org.freedesktop.ScreenSaver',member='ActiveChanged'" |
while read -r line; do
    if [[ "$line" == *"boolean false"* ]]; then
        "$HOME/.local/bin/deskwork/deskwork" --reset
    fi
done
