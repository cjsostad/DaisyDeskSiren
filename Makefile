# DubSauce — Seed3 Desktop dub siren (bench v0.4.1 — lightweight FX bus)
TARGET = DubSauceSiren

DEBUG = 0
OPT = -O3

# Program runs from SRAM, loaded by the external-DFU bootloader
APP_TYPE = BOOT_SRAM

# Only our firmware compiles. The je-201 RE-201 model under src/je201/ is SHELVED
# (left on disk for a later optimization pass) and intentionally NOT built here.
CPP_SOURCES = src/main.cpp

C_INCLUDES += -Isrc

# libDaisy v9.0.0 + DaisySP live inside this project folder
LIBDAISY_DIR = libDaisy
DAISYSP_DIR  = DaisySP

USE_FATFS = 1

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core

# External-DFU bootloader (reprogram over the kit's USB-C). Requires libDaisy v9.0.0.
BOOT_BIN = $(SYSTEM_FILES_DIR)/dsy_bootloader_v6_4-extdfu-2000ms.bin

include $(SYSTEM_FILES_DIR)/Makefile
