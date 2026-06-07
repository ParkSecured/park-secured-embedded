# ParkSecured Embedded

ESP32 firmware for the ParkSecured physical gate.

## Live configuration

```text
Cloud API:     https://park-secured-cloud-r62j.onrender.com/api
Web dashboard: https://park-secure-vrxr.onrender.com/
```

The firmware receives a Bluetooth access code from the mobile app, validates it through the cloud API over HTTPS, then updates the hardware status in the cloud.

## Cloud endpoints used

```text
POST /api/gate/validate-bluetooth
POST /api/hardware/update-status
```

## Required local configuration

Update these values in `src/main.cpp` before flashing a real board:

```cpp
#define WIFI_SSID "..."
#define WIFI_PASS "..."
#define GATE_API_KEY "..."
```

The repository keeps `GATE_API_KEY` as a placeholder. Do not commit the real key.

## Build

```powershell
py -m platformio run
```

The project uses `board_build.partitions = huge_app.csv` because HTTPS increases the firmware size beyond the default ESP32 partition.
