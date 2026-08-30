#ifndef MICKEY_ALARM_H
#define MICKEY_ALARM_H

#include <cstdint>
#include <string>

#include <esp_timer.h>

struct MickeyAlarmState {
    bool enabled = false;
    int hour = 7;
    int minute = 0;
    bool repeat = true;
    bool pending_morning = false;
    bool clock_synced = false;
    int64_t next_epoch = 0;
};

class MickeyAlarm {
public:
    static MickeyAlarm& GetInstance();

    bool SetAlarm(int hour, int minute, bool repeat = true);
    MickeyAlarmState GetAlarm() const;
    void Cancel();

    // Returns a JSON status string. Teardown runs on a worker task.
    std::string RequestDeepSleep(int hour, int minute, int seconds_from_now);

    void StartMorningWatcher();
    void RegisterMcpTools();

    bool IsClockSynced() const;
    bool ComputeNextEpoch(int hour, int minute, int64_t* next_epoch) const;

private:
    MickeyAlarm() = default;
    MickeyAlarm(const MickeyAlarm&) = delete;
    MickeyAlarm& operator=(const MickeyAlarm&) = delete;

    void EnterDeepSleepOnTask();
    void ArmAndSleep(uint64_t sleep_us);
    void OnMorningTick();
    void StopMorningWatcher();
    void RunMorningRoutine();
    void ClearPendingMorning();
    bool WokeFromTimer() const;
    bool WokeFromExt0() const;

    static void SleepTask(void* arg);
    static void MorningTimerCallback(void* arg);

    bool sleep_requested_ = false;
    bool mcp_registered_ = false;
    uint64_t sleep_us_ = 0;
    int idle_hold_ticks_ = 0;
    esp_timer_handle_t morning_timer_ = nullptr;
};

#endif  // MICKEY_ALARM_H
