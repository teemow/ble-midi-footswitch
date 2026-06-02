This directory is a vendored, version-pinned copy of NimBLE-Arduino 1.4.3.

Why vendored: lathoub/BLE-MIDI 2.2's ESP32 NimBLE backend uses the NimBLE 1.x
API. PlatformIO cannot constrain BLE-MIDI's unversioned transitive NimBLE
dependency, so it would otherwise pull NimBLE 2.x alongside a pinned 1.4.x and
fail to link (duplicate symbols). A private lib here satisfies that dependency
and prevents the registry copy from being installed.

Source: https://github.com/h2zero/NimBLE-Arduino/tree/1.4.3
Do not bump to 2.x without migrating the BLE-MIDI backend to the NimBLE 2.x API.
