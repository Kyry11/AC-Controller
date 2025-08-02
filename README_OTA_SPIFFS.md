# OTA SPIFFS Upload for AC-Controller

This document explains how to upload the SPIFFS filesystem to your ESP32 AC-Controller device using Over-The-Air (OTA) updates.

## Prerequisites

- Node.js installed on your computer
- PlatformIO installed (for building the SPIFFS image)
- ESP32 device with AC-Controller firmware already installed
- ESP32 device connected to your local network

## Building the SPIFFS Image

Before uploading the SPIFFS filesystem, you need to build the SPIFFS image:

### Step 1: Prepare Static Files

You have two options for preparing your static files:

#### Option 1: Manually Copy Files

1. Create a `data/` directory in the project root if it doesn't exist
2. Copy all your static files (HTML, CSS, JavaScript, images, etc.) to the `data/` directory

#### Option 2: Use the Copy Script

Use the provided script to automatically copy static files from the `src/static/` directory to the `data/` directory:

```bash
./copy_static_files.js
```

This script will:
- Create the `data/` directory if it doesn't exist
- Copy all files from `src/static/` to `data/`
- Skip `.h` and `.cpp` files

### Step 2: Build the SPIFFS Image

Run the following command to build the SPIFFS image:

```bash
pio run --target buildfs
```

This will create a SPIFFS binary file at `.pio/build/esp32dev/spiffs.bin`.

### Step 3 (Optional): All-in-One Script

Alternatively, you can use the all-in-one script to copy files, build the image, and upload it:

```bash
# Copy files, build and upload
./build_and_upload_spiffs.sh --copy --ip YOUR_ESP32_IP

# Just copy files and build (don't upload)
./build_and_upload_spiffs.sh --copy --build-only
```

## Uploading SPIFFS via OTA

There are two ways to upload the SPIFFS filesystem via OTA:

### Method 1: Using the Web Interface

1. Open a web browser and navigate to your ESP32's IP address (e.g., `http://192.168.1.100`)
2. Click on the "Upload new firmware" link at the bottom of the page
3. In the OTA update page, select "Filesystem" from the dropdown menu
4. Click "Choose File" and select the SPIFFS binary file (`.pio/build/esp32dev/spiffs.bin`)
5. Click "Update" to start the upload process
6. Wait for the upload to complete and the device to restart

### Method 2: Using the Command-Line Script

1. Make sure you have built the SPIFFS image as described above
2. Run the `upload_spiffs_ota.js` script:

```bash
./upload_spiffs_ota.js
```

3. Enter the IP address of your ESP32 device when prompted (or press Enter to use the default)
4. Enter the port number when prompted (or press Enter to use the default port 80)
5. The script will calculate the MD5 hash of the SPIFFS binary and upload it to the device
6. Wait for the upload to complete and the device to restart

## Troubleshooting

### Upload Issues

- If the upload fails, make sure your ESP32 device is connected to the network and accessible
- Check that the IP address and port are correct
- Ensure that the SPIFFS binary file exists at the expected location
- If using the web interface, make sure your browser supports the required JavaScript features
- Try restarting the ESP32 device and attempting the upload again

### Static Files Issues

- If your static files are not showing up after upload, check that they were properly included in the SPIFFS image
- Run `./copy_static_files.js` to ensure all files are copied to the `data/` directory
- Make sure your HTML files have the correct paths to CSS and JavaScript files
- Check the ESP32's serial output for any file system errors

## Notes

- The SPIFFS upload process will replace all files on the SPIFFS filesystem
- The device will automatically restart after the upload is complete
- The upload process may take a few minutes depending on the size of the SPIFFS image
- The maximum SPIFFS size is determined by the partition scheme in `platformio.ini`
