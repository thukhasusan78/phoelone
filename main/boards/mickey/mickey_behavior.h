#ifndef MICKEY_BEHAVIOR_H
#define MICKEY_BEHAVIOR_H

void InitializeMickeyBehavior();
void MickeyBehaviorPause();
void MickeyBehaviorResume();
void MickeyBehaviorNotifyExternalEmotion();
void MickeyBehaviorOnPetBegin();
void MickeyBehaviorOnPetEnd();
void MickeyBehaviorOnImuEvent(int event);

#endif  // MICKEY_BEHAVIOR_H
