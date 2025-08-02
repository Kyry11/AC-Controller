# AC-Controller

ESP32-based controller for Fujitsu air conditioners with web interface and Home Assistant integration.

## Features

- Control your Fujitsu air conditioner via WiFi
- Web interface for easy control
- OTA (Over-The-Air) firmware updates
- OTA SPIFFS filesystem updates
- Home Assistant integration
- IR remote control emulation

## Documentation

- [OTA SPIFFS Upload](README_OTA_SPIFFS.md) - How to upload static files to the SPIFFS filesystem via OTA

## Tools

- `upload_spiffs_ota.js` - Node.js script for uploading SPIFFS filesystem via OTA
- `build_and_upload_spiffs.sh` - Shell script to build and upload SPIFFS filesystem in one command

## Development

This project is built using PlatformIO. To build and upload the firmware:

```bash
pio run -t upload
```

To build and upload the SPIFFS filesystem:

```bash
# Build SPIFFS image
pio run -t buildfs

# Upload SPIFFS image via serial
pio run -t uploadfs

# Upload SPIFFS image via OTA
./build_and_upload_spiffs.sh --ip YOUR_ESP32_IP
```

## Home Assistant Integration

This project includes a custom Home Assistant component for easy integration. See the `ha_custom_component` directory for details.
