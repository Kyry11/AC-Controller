#!/usr/bin/env node

/**
 * Script to copy static files to the data directory for SPIFFS
 *
 * This script copies all static files from src/static to the data directory
 * for inclusion in the SPIFFS filesystem.
 */

const fs = require('fs');
const path = require('path');

// Configuration
const SOURCE_DIR = 'src/static';
const TARGET_DIR = 'data';

// Create data directory if it doesn't exist
if (!fs.existsSync(TARGET_DIR)) {
  console.log(`Creating ${TARGET_DIR} directory...`);
  fs.mkdirSync(TARGET_DIR, { recursive: true });
}

// Function to copy a file
function copyFile(source, target) {
  const targetDir = path.dirname(target);

  // Create target directory if it doesn't exist
  if (!fs.existsSync(targetDir)) {
    fs.mkdirSync(targetDir, { recursive: true });
  }

  // Copy the file
  fs.copyFileSync(source, target);
  console.log(`Copied: ${source} -> ${target}`);
}

// Function to recursively copy files from source to target
function copyFiles(sourceDir, targetDir) {
  // Read the source directory
  const entries = fs.readdirSync(sourceDir, { withFileTypes: true });

  // Process each entry
  for (const entry of entries) {
    const sourcePath = path.join(sourceDir, entry.name);
    const targetPath = path.join(targetDir, entry.name);

    if (entry.isDirectory()) {
      // Recursively copy subdirectories
      copyFiles(sourcePath, targetPath);
    } else {
      // Skip .h and .cpp files
      if (entry.name.endsWith('.h') || entry.name.endsWith('.cpp') || entry.name.endsWith('.sh')) {
        console.log(`Skipping: ${sourcePath}`);
        continue;
      }

      // Copy the file
      copyFile(sourcePath, targetPath);
    }
  }
}

// Check if source directory exists
if (!fs.existsSync(SOURCE_DIR)) {
  console.error(`Error: Source directory '${SOURCE_DIR}' not found`);
  process.exit(1);
}

// Start copying files
console.log(`Copying static files from ${SOURCE_DIR} to ${TARGET_DIR}...`);
copyFiles(SOURCE_DIR, TARGET_DIR);

console.log('Done!');
