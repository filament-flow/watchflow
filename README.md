# WatchFlow – Live Drucker-Monitoring für FilamentFlow

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

WatchFlow ist ein ESP32-basiertes Drucker-Monitoring-Gerät für [FilamentFlow](https://filament-flow.com). Es überwacht Bambu Lab Drucker via MQTT und Moonraker/Klipper Drucker via HTTP und sendet Live-Daten an die FilamentFlow Web-App.

## Features

- 📡 **Bambu Lab** – LAN-Modus via MQTT (X1C, P1S, P1P, A1, H2C, H2S, alle Modelle mit Developer Mode)
- 🔧 **Moonraker/Klipper** – HTTP-Polling (Creality, Sovol, Snapmaker, Custom Builds)
- 📊 Live-Daten: Status, Fortschritt, Temperaturen, AMS-Belegung, HMS-Fehler
- ⚡ Bis zu 3 Drucker gleichzeitig
- 🔧 Setup via Browser (AP-Mode, keine App nötig)
- 🔑 Hardware-Lizenz für Pro-Features (20s Intervall, AMS, HMS, Temperaturen)

## Hardware

| Bauteil | Empfehlung |
|---|---|
| Mikrocontroller | ESP32 Dev Module (WROOM-32E) oder WEMOS D1 Mini |
| Stromversorgung | 5V via Micro-USB (min. 1A Netzteil) |

## Pinbelegung

| Funktion | GPIO |
|---|---|
| Setup-Button | GPIO 4 (beim Boot halten für Setup-Modus) |

## Flashen

### Voraussetzungen
- Arduino IDE 2.x
- ESP32 Board Package: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`

### Bibliotheken (Arduino Library Manager)
- ArduinoJson v6.x
- PubSubClient

### Schritte
1. `WatchFlow_v2_2_1.ino` in Arduino IDE öffnen
2. Board: **ESP32 Dev Module**
3. Upload-Speed: 460800
4. Flashen → fertig

## Setup

1. ESP32 einschalten → Setup-Button (GPIO 4) beim Boot gedrückt halten
2. Mit WLAN `WatchFlow-Setup` verbinden (Passwort: `watchflow`)
3. Browser öffnen: `http://192.168.4.1`
4. WLAN, FilamentFlow API-Key und Drucker konfigurieren
5. Speichern → ESP32 startet neu

## FilamentFlow API-Key

Den API-Key findest du in der FilamentFlow App unter **Einstellungen → WatchFlow API**.

## Hardware-Lizenz (Pro-Features)

Ein Hardware-Lizenz-Key (WF-XXXX oder Bundle BN-XXXX) schaltet folgende Features frei:

| Feature | Ohne Key | Mit Key |
|---|---|---|
| Druckerstatus | ✅ | ✅ |
| Fortschritt in % | ✅ | ✅ |
| Temperaturen | ❌ | ✅ |
| AMS-Daten | ❌ | ✅ |
| HMS-Fehler | ❌ | ✅ |
| Druckjob-Name | ❌ | ✅ |
| Poll-Intervall | 60s | 20s |

Keys sind erhältlich auf [filament-flow.com](https://filament-flow.com) oder als Teil eines WatchFlow-Kits.

## Selbst bauen

Die Firmware ist Open Source (GPL v3) – du kannst sie frei nutzen, modifizieren und weitergeben. Für den vollen Funktionsumfang benötigst du einen Hardware-Lizenz-Key.

Community-Builds, Gehäuse und Halterungen sind herzlich willkommen!

## Verwandte Projekte

- [FilamentFlow](https://filament-flow.com) – Die Web-App
- [SpoolFlow](https://github.com/filament-flow/spoolflow) – NFC Spool-Tracker

---

*MW Service 3D | filament-flow.com | @filament_flow_com*
