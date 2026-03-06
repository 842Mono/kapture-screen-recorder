CC=gcc
PKG_CONFIG_CFLAGS=$(shell pkg-config --cflags gtk4 gstreamer-1.0)
PKG_CONFIG_LIBS=$(shell pkg-config --libs gtk4 gstreamer-1.0)

CFLAGS=-Wall $(PKG_CONFIG_CFLAGS)
LDFLAGS=$(PKG_CONFIG_LIBS)

TARGET=kapture-screen-recorder
SRC=main.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)