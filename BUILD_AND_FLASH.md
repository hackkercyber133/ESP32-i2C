# ESP32 NEXUS — Build APK & Flash Firmware

## 1. APK

### Local
```bash
flutter pub get
flutter create --platforms=android --org com.esp32nexus --project-name esp32_nexus .
flutter build apk --release
```

APK akan berada di:
`build/app/outputs/flutter-apk/app-release.apk`

### GitHub Actions
Upload project ini ke GitHub, push ke branch `main`/`master`, lalu buka tab **Actions**.
Workflow **Build ESP32 NEXUS** menghasilkan artifact:
- `ESP32-NEXUS-APK`
- `ESP32-NEXUS-Firmware`

## 2. Firmware ESP32-C3

PlatformIO:
```bash
pio run -e esp32-c3-supermini
pio run -e esp32-c3-supermini -t upload
```

Source Arduino tetap tersedia di:
`firmware/firmware.ino`

## 3. Flash dengan aplikasi ESP32 Flash

Gunakan file hasil CI:
`ESP32-NEXUS-firmware.bin`

Jika aplikasi flash meminta alamat flash, firmware PlatformIO biasanya berada di offset:
`0x10000`

Untuk paket full-flash, gunakan bootloader + partitions + firmware sesuai offset yang ditampilkan oleh tool flash Anda. Jangan menebak offset tool yang berbeda.

## 4. Setelah firmware hidup

Default AP:
- SSID: `ESP32Cooler`
- Password: `bandar01`
- IP: `192.168.4.1`

Firmware menyediakan:
- `/api/info`
- `/api/status`
- `/api/set?voltage=5`
- `/api/led?mode=running`
- `/api/safe`
- `/scanwifi`
- `/setwifi`

Kontrol utama aplikasi menggunakan MQTT/BLE yang sudah sesuai dengan firmware:
- MQTT broker: `broker.emqx.io:1883`
- command: `cooler/<DEVICE_ID>/command`
- status: `cooler/<DEVICE_ID>/status`
- BLE service: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
- BLE characteristic: `beb5483e-36e1-4688-b7f5-ea07361b26a8`

## 5. Mode voltage hardware

Firmware mempertahankan mapping hardware yang sudah ada:
- 5V = semua selector floating
- 9V = GPIO6 LOW
- 12V = GPIO7 LOW
- 15V = GPIO5 LOW

GPIO 2, 8, dan 9 tidak digunakan karena alasan strapping/LED/BOOT.


## CH224A I2C hardware revision

The firmware in this archive is configured for the CH224A I2C trigger board:
SDA=GPIO5, SCL=GPIO6, PG=GPIO7. The old direct selector-pad GPIO logic has been removed.

## PD status in Flutter

The Flutter app now waits for firmware status instead of assuming a voltage change succeeded.
It displays CH224A readiness and PD negotiation status (`PD NEGOTIATED`, `PD WAITING`, or `REQUEST FAILED`).
No extra Flutter package is required.
