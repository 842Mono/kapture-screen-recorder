#!/bin/bash

# 1. Compile
echo "Compiling..."
gcc -o kapture-screen-recorder main.c $(pkg-config --cflags --libs gtk4 gstreamer-1.0)

# 2. Create Structure
echo "Creating directory structure..."
rm -rf kapture-deb
mkdir -p kapture-deb/DEBIAN
mkdir -p kapture-deb/usr/bin
mkdir -p kapture-deb/usr/share/applications
mkdir -p kapture-deb/usr/share/icons/hicolor/scalable/apps

# 3. Control File
echo "Creating control file..."
cat > kapture-deb/DEBIAN/control <<EOF
Package: kapture-screen-recorder
Version: 1.0
Section: video
Priority: optional
Architecture: amd64
Depends: libgtk-4-1, libgstreamer1.0-0, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-plugins-ugly, gstreamer1.0-libav, gstreamer1.0-pipewire
Maintainer: Mina William Michael Morcos <842mono@gmail.com>
Description: A simple Wayland screen recorder
 Records screen and audio using GStreamer and PipeWire.
 Supports MKV, MP4, WebM, and AVI formats.
EOF

# 4. Desktop Entry
echo "Creating desktop entry..."
cat > kapture-deb/usr/share/applications/kapture-screen-recorder.desktop <<EOF
[Desktop Entry]
Name=Kapture Screen Recorder
Comment=Record your Wayland screen
Exec=/usr/bin/kapture-screen-recorder
Icon=io.github.842mono.kapture
Terminal=false
Type=Application
Categories=AudioVideo;Recorder;
EOF

# 5. Copy Files
echo "Copying files..."
cp kapture-screen-recorder kapture-deb/usr/bin/
chmod 755 kapture-deb/usr/bin/kapture-screen-recorder

# Copy the icon to the standard system icon path so the desktop entry finds it
cp kapture-icon.svg kapture-deb/usr/share/icons/hicolor/scalable/apps/io.github.842mono.kapture.svg

# 6. Build
echo "Building .deb..."
dpkg-deb --build kapture-deb kapture-screen-recorder_1.0_amd64.deb

echo "Done! You can install it with: sudo apt install ./kapture-screen-recorder_1.0_amd64.deb"