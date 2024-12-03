# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2

# Directories
SRC_DIR = .
CACHE_DIR = $(SRC_DIR)/cache
HEALTH_CHECK_DIR = $(SRC_DIR)/health_check
LOAD_BALANCER_DIR = $(SRC_DIR)/load_balancer

# Source files
SOURCES = $(CACHE_DIR)/cache.c \
          $(HEALTH_CHECK_DIR)/health_check.c \
          $(LOAD_BALANCER_DIR)/load_balancer.c \
          $(SRC_DIR)/asynch_reverse_proxy.c

# Header files
HEADERS = $(CACHE_DIR)/cache.h \
          $(HEALTH_CHECK_DIR)/health_check.h \
          $(LOAD_BALANCER_DIR)/load_balancer.h

# Object files
OBJECTS = $(SOURCES:.c=.o)

# Output executable
TARGET = reverse_proxy

# Build rules
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
