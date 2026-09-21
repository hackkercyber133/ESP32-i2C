# ESP32 NEXUS Gaming Controller 2026

Project terintegrasi Flutter + ESP32-C3 Super Mini untuk mengontrol 5V / 9V / 12V / 15V dan RGB LED.

## App
- UI gaming neon modern
- hamburger menu kanan atas
- developer / Telegram / group / tujuan aplikasi
- dark / light mode
- custom accent color
- data & otomasi
- grafik durasi harian + grafik rata-rata voltase harian
- jadwal otomatis
- MQTT + BLE
- WiFi setup AP ESP32
- history lokal

## Firmware
Firmware berasal dari proyek ESP32-C3 yang sudah diperbaiki, dengan selector:
GPIO6=9V, GPIO7=12V, GPIO5=15V; default 5V floating.

RGB strip menggunakan GPIO4.

## Build
Lihat `BUILD_AND_FLASH.md`.
