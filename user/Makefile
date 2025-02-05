CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200112L

# Source files
SRC = init_csd_edge.c hmb_mmap.c

# Header files
HEADERS = hmb_mmap.h

# Output binary
TARGET = init_csd_edge

# Build rules
all: $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lrt

clean:
	rm -f $(TARGET)