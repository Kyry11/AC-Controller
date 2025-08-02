#!/bin/bash

# Build and upload SPIFFS filesystem for AC-Controller
# This script builds the SPIFFS image and uploads it via OTA

# Exit on error
set -e

# Print usage information
function print_usage {
  echo "Usage: $0 [options]"
  echo ""
  echo "Options:"
  echo "  -i, --ip IP_ADDRESS    Specify the IP address of the ESP32 (default: 192.168.1.1)"
  echo "  -p, --port PORT        Specify the port of the ESP32 web server (default: 80)"
  echo "  -c, --copy             Copy static files to data directory before building"
  echo "  -b, --build-only       Build SPIFFS image only, don't upload"
  echo "  -h, --help             Show this help message"
  echo ""
  echo "Example:"
  echo "  $0 --ip 192.168.1.100 --port 8080 --copy"
}

# Default values
IP_ADDRESS="192.168.11.144"
PORT=80
COPY_FILES=false
BUILD_ONLY=false

# Parse command line arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    -i|--ip)
      IP_ADDRESS="$2"
      shift 2
      ;;
    -p|--port)
      PORT="$2"
      shift 2
      ;;
    -c|--copy)
      COPY_FILES=true
      shift
      ;;
    -b|--build-only)
      BUILD_ONLY=true
      shift
      ;;
    -h|--help)
      print_usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      print_usage
      exit 1
      ;;
  esac
done

# Check if the data directory exists
if [ ! -d "data" ]; then
  echo "Error: 'data' directory not found"
  echo "Please create a 'data' directory and add your static files to it"
  exit 1
fi

# Copy static files if requested
if [ "$COPY_FILES" = true ]; then
  # Check if the copy script exists
  if [ ! -f "copy_static_files.js" ]; then
    echo "Error: copy_static_files.js script not found"
    exit 1
  fi

  # Make sure the copy script is executable
  chmod +x copy_static_files.js

  # Copy static files
  echo "Copying static files to data directory..."
  ./copy_static_files.js
fi

# Check if there are files in the data directory
if [ -z "$(ls -A data)" ]; then
  echo "Warning: 'data' directory is empty"
  echo "No files will be included in the SPIFFS image"
  read -p "Do you want to continue? (y/n) " -n 1 -r
  echo
  if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    exit 1
  fi
fi

# Build SPIFFS image
echo "Building SPIFFS image..."
pio run --target buildfs

# Check if the SPIFFS binary exists
SPIFFS_BIN=".pio/build/esp32dev/spiffs.bin"
if [ ! -f "$SPIFFS_BIN" ]; then
  echo "Error: SPIFFS binary not found at $SPIFFS_BIN"
  echo "Build failed"
  exit 1
fi

# Check if Node.js is installed
if ! command -v node &> /dev/null; then
  echo "Error: Node.js is not installed"
  echo "Please install Node.js to use the OTA upload script"
  exit 1
fi

# Check if the upload script exists
if [ ! -f "upload_spiffs_ota.js" ]; then
  echo "Error: upload_spiffs_ota.js script not found"
  exit 1
fi

# Make sure the upload script is executable
chmod +x upload_spiffs_ota.js

# Upload SPIFFS image via OTA if not build-only
if [ "$BUILD_ONLY" = false ]; then
  # Upload SPIFFS image via OTA
  echo "Uploading SPIFFS image to $IP_ADDRESS:$PORT..."
  ./upload_spiffs_ota.js <<EOF
$IP_ADDRESS
EOF
  echo "Upload completed successfully!"
else
  echo "SPIFFS image built successfully!"
  echo "To upload the image, run: ./upload_spiffs_ota.js"
fi

echo "Done!"
