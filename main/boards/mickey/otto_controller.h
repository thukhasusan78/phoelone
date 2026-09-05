#ifndef OTTO_CONTROLLER_H
#define OTTO_CONTROLLER_H

#include "config.h"

enum OttoFidgetMotion {
    kOttoFidgetNone = 0,
    kOttoFidgetSwing,
    kOttoFidgetTiptoeSwing,
    kOttoFidgetShakeLeg,
    kOttoFidgetUpDown,
    kOttoFidgetBend,
    kOttoFidgetJitter,
    kOttoFidgetWalk,
    kOttoFidgetSit,
};

void InitializeOttoController(const HardwareConfig& hw_config);
void OttoPrepareForSleep();
void OttoQueueMorningWake();
bool OttoTryQueueFidget(OttoFidgetMotion motion, int steps, int speed, int direction, int amount);
void OttoCancelFidget();
bool OttoIsBusy();
bool OttoIsFidgeting();
void OttoStopAndHome();
void OttoQueueHome();
bool OttoMotionInhibited();

#endif  // OTTO_CONTROLLER_H
