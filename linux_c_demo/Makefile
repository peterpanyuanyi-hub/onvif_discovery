CC = gcc
CFLAGS = -Wall -Wextra
TARGET = onvif_discover

all: $(TARGET)

$(TARGET): onvif_discover.c
	$(CC) $(CFLAGS) -o $(TARGET) onvif_discover.c

clean:
	rm -f $(TARGET)
