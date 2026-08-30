# Mickey AI Robot - Agent Context & Instructions

## 1. Project Core Objective
You are assisting Thu Kha Su San in developing "Mickey," an ESP32-S3 desktop AI robot based on the open-source XiaoZhi client. 
**CRITICAL GOAL:** We are NOT using the official XiaoZhi cloud server. We are building a **Custom Local Backend Server**. The ESP32 client code should remain as unmodified as possible, only changing what is strictly necessary to point to our custom backend.

## 2. Hardware & Pinout Status (DO NOT MODIFY PINS)
The current hardware is wired exactly according to the original `otto-robot` configuration in the repository:
- **Servos:** Right Leg 39, Right foot 38, Left leg 17, Left foot 18
- **Display:** SCL 9, SDA 10, RES 11, DC 46, CS GND, BLK 3
- **Amplifier (I2S):** LRC 16, BCLK 15, DIN 7
- **Microphone (I2S):** SD 6, SCK 5, WS 4
**ACTION:** DO NOT modify any GPIO pins in `config.h`. The original codebase is already perfectly matched to this hardware and should be kept as-is.

## 3. Custom Backend Development Workflow
When asked to work on the Backend:
1. Read `docs/websocket.md` and `docs/mqtt-udp.md` to understand the communication protocols.
2. Build a custom server (e.g., using Python/FastAPI) that mimics the expected WebSocket/MQTT behavior.
3. Guide the user to update ONLY the Server IP and Wi-Fi credentials in the ESP32 client (via `sdkconfig` or `menuconfig`) to connect to this new custom server.

CRITICAL RULE: The Custom Backend Server will NOT be developed locally in this repository. It will be hosted remotely on a Digital Ocean VPS. DO NOT generate any backend scripts (e.g., Python, FastAPI) inside this ESP32 workspace. Focus EXCLUSIVELY on the ESP-IDF client-side C++ code and leave the backend development to a separate SSH session.

## 4. Future Hardware Expansion
When the user wants to add new modules (e.g., MPU6050, Light Sensor, Touch Sensor):
1. Implement the driver code properly within the ESP-IDF framework.
2. Register the sensor data as **MCP (Model Context Protocol)** tools so the Custom Backend can access them.
3. Ensure additions do not block the main audio/display loops.

## 5. Strict Rules for Agent
- **DO NOT use PlatformIO.** Use the official ESP-IDF v6.0.x workflow.
- Build command: `python scripts/build.py mickey --name mickey --language en-US`
- When modifying code, explain *why* and *which file* is being changed.