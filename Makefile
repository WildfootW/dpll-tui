# Makefile for dpllctl
# SPDX-License-Identifier: MIT

CC       = gcc
CFLAGS   = -g -O2 -Wall -Wextra -Werror -std=gnu11 -D_GNU_SOURCE
CFLAGS  += -fPIE -fcf-protection=full
LDFLAGS  = -pie -Wl,-z,relro,-z,now,-z,noexecstack -lynl -lncurses

SRC_DIR = src
HDR_DIR = hdr
OBJ_DIR = obj

HEADERS = $(HDR_DIR)/dpll_utils.h $(HDR_DIR)/log.h
OBJECTS = $(OBJ_DIR)/dpllctl.o $(OBJ_DIR)/dpll_utils.o
TARGET  = dpllctl

all: $(TARGET)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS)
	@echo "Built $@ successfully"

$(OBJ_DIR)/dpllctl.o: $(SRC_DIR)/dpllctl.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -I. -c $< -o $@

$(OBJ_DIR)/dpll_utils.o: $(SRC_DIR)/dpll_utils.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -I. -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)
	rm -f $(TARGET)
	@echo "Cleaned build artifacts"

.PHONY: all clean
