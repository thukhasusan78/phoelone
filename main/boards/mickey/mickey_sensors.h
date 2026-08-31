#ifndef MICKEY_SENSORS_H
#define MICKEY_SENSORS_H

#include <cstdint>
#include <string>

enum MickeyImuEvent {
    kMickeyImuStill = 0,
    kMickeyImuMoving,
    kMickeyImuPickup,
    kMickeyImuPutdown,
    kMickeyImuFall,
    kMickeyImuShake,
};

struct MickeyImuSnapshot {
    bool wired = true;
    bool ok = false;
    const char* error = "not_started";
    float ax = 0;
    float ay = 0;
    float az = 0;
    float gx = 0;
    float gy = 0;
    float gz = 0;
    float pitch = 0;
    float roll = 0;
    float temp_c = 0;
    MickeyImuEvent event = kMickeyImuStill;
};

struct MickeyTouchSnapshot {
    bool wired = true;
    bool touched = false;
    uint32_t count = 0;
    uint32_t ms_held = 0;
};

struct MickeySensorSnapshot {
    MickeyImuSnapshot imu;
    MickeyTouchSnapshot touch;
};

void InitializeMickeySensors();
void MickeySensorsPrepareForSleep();
void MickeySensorsEmitEvent(const char* event);
MickeySensorSnapshot MickeySensorsGetSnapshot();
std::string MickeySensorsImuJson();
std::string MickeySensorsTouchJson();

#endif  // MICKEY_SENSORS_H
