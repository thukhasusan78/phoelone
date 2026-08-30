# Custom Board Guide

This file is the English translation of the former Chinese `docs/custom-board_zh.md`.

**Complete specification:** [custom-board.md](./custom-board.md)

A XiaoZhi firmware build selects exactly one board. Board identity is a coupled chain:

`config.json` → `scripts/build.py` → `main/Kconfig.projbuild` → `main/CMakeLists.txt` → board source and `config.h`.

Never change an existing board's GPIO map to support different hardware. Add a uniquely named board or release variant; board identity affects OTA compatibility. Export exactly one board factory with `DECLARE_BOARD(...)`.

Mickey currently uses the unique `mickey` board profile (`python scripts/build.py mickey --name mickey --language en-US`). Do not put Mickey GPIO changes into core modules. Board sources live under `main/boards/mickey/` with OTA `board.type` `mickey`.
