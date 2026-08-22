# Build and Configuration Guide

This file is the English translation of the former Chinese README for Movecall Moji2.0.

This document provides instructions on how to configure and build the firmware for **Movecall Moji2.0 (Xiaozhi AI Edition)**.

## Prerequisites

- **ESP-IDF Version**: v5.5
- **Target Chip**: ESP32-C5

## Hardware Information

This project is based on the following open-source hardware:

- **OSHWHub Link**: [https://oshwhub.com/movecall/moji2](https://oshwhub.com/movecall/moji2)

## Build Steps

### 1. Set the Build Target

Initialize the project to target the ESP32-C5 chip:

```bash
idf.py set-target esp32c5
```

### 2. Configure the Board Type

Open the graphical configuration menu:

```bash
idf.py menuconfig
```

Navigate to:

> **Xiaozhi Assistant** -> **Board Type** -> **Movecall Moji2.0 Xiaozhi AI derivative**

After selecting, press **S** to save (then Enter to confirm) and press **Q** to exit.

### 3. Build the Project

```bash
idf.py build
```

## Useful Commands

Clean build files (recommended if you encounter errors):

```bash
idf.py fullclean
```

Flash firmware to the device:

```bash
idf.py flash
```

Monitor serial output:

```bash
idf.py monitor
```
