# Makefile for the C-based Wayland Recorder

TARGET = kapture-screen-recorder

# Use pkg-config to find compiler and linker flags for our dependencies
CFLAGS := $(shell pkg-config --cflags gtk4 gstreamer-1.0 gio-2.0)
LIBS := $(shell pkg-config --libs gtk4 gstreamer-1.0 gio-2.0)

all: $(TARGET)

$(TARGET): main.c
	$(CC) -o $(TARGET) main.c $(CFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)