CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
CPPFLAGS ?= -Iinclude

BUILD_DIR := build
TARGET := $(BUILD_DIR)/neuromorphic_core_demo
SRCS := src/main.c \
	src/core.c \
	src/core/config.c \
	src/core/ack.c \
	src/core/compute.c \
	src/core/encoder.c \
	src/core/input.c \
	src/core/mapping.c \
	src/core/output.c \
	src/core/utils.c \
	src/router.c
OBJS := $(SRCS:src/%.c=$(BUILD_DIR)/%.o)
PUBLIC_HEADERS := include/nmc/core.h include/nmc/router.h
PRIVATE_HEADERS := include/nmc/internal/core.h

.PHONY: all clean run

all: $(TARGET)

run: $(TARGET)
	./$(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

$(BUILD_DIR)/%.o: src/%.c $(PUBLIC_HEADERS) $(PRIVATE_HEADERS) | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
