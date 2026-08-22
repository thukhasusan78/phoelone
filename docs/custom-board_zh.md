# Custom Board Guide

This file is the English translation of the former Chinese `docs/custom-board_zh.md`.

**Complete specification:** [custom-board.md](./custom-board.md)

A XiaoZhi firmware build selects exactly one board. Board identity is a coupled chain:

`config.json` → `scripts/build.py` → `main/Kconfig.projbuild` → `main/CMakeLists.txt` → board source and `config.h`.

Never change an existing board's GPIO map to support different hardware. Add a uniquely named board or release variant; board identity affects OTA compatibility. Export exactly one board factory with `DECLARE_BOARD(...)`.

Phoe Lone currently uses the stock `otto-robot` profile (`python scripts/build.py otto-robot --name otto-robot`). Do not put Phoe Lone GPIO changes into core modules. A future unique board would live under `main/boards/phoe-lone/` with its own `BOARD_TYPE`.
