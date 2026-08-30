#include "mickey_alarm.h"

#include <inttypes.h>
#include <time.h>

#include <cJSON.h>
#include <driver/rtc_io.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "config.h"
#include "device_state.h"
#include "display.h"
#include "mcp_server.h"
#include "settings.h"

#define TAG "MickeyAlarm"

namespace {
constexpr const char* kNvsNs = "mickey_alarm";
constexpr int kMinValidYear = 2025;
constexpr int kMorningIdleHoldTicks = 3;
constexpr int kSleepIdleWaitMs = 3000;
constexpr int kSleepTaskStack = 4096;
constexpr UBaseType_t kSleepTaskPriority = 3;
}  // namespace

extern void OttoPrepareForSleep();
extern void OttoQueueMorningWake();

MickeyAlarm& MickeyAlarm::GetInstance() {
    static MickeyAlarm instance;
    return instance;
}

bool MickeyAlarm::IsClockSynced() const {
    time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    return (local.tm_year + 1900) >= kMinValidYear;
}

bool MickeyAlarm::ComputeNextEpoch(int hour, int minute, int64_t* next_epoch) const {
    if (next_epoch == nullptr) {
        return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }
    if (!IsClockSynced()) {
        return false;
    }

    time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    time_t target = mktime(&local);
    if (target <= now) {
        target += 24 * 60 * 60;
    }
    *next_epoch = static_cast<int64_t>(target);
    return true;
}

bool MickeyAlarm::SetAlarm(int hour, int minute, bool repeat) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }
    Settings settings(kNvsNs, true);
    settings.SetBool("enabled", true);
    settings.SetInt("hour", hour);
    settings.SetInt("minute", minute);
    settings.SetBool("repeat", repeat);
    ESP_LOGI(TAG, "Alarm set to %02d:%02d repeat=%d", hour, minute, repeat ? 1 : 0);
    return true;
}

MickeyAlarmState MickeyAlarm::GetAlarm() const {
    Settings settings(kNvsNs, false);
    MickeyAlarmState state;
    state.enabled = settings.GetBool("enabled", false);
    state.hour = settings.GetInt("hour", 7);
    state.minute = settings.GetInt("minute", 0);
    state.repeat = settings.GetBool("repeat", true);
    state.pending_morning = settings.GetBool("pending_morning", false);
    state.clock_synced = IsClockSynced();
    if (state.enabled) {
        int64_t next = 0;
        if (ComputeNextEpoch(state.hour, state.minute, &next)) {
            state.next_epoch = next;
        }
    }
    return state;
}

void MickeyAlarm::Cancel() {
    Settings settings(kNvsNs, true);
    settings.SetBool("enabled", false);
    settings.SetBool("pending_morning", false);
    ESP_LOGI(TAG, "Alarm cancelled");
}

void MickeyAlarm::ClearPendingMorning() {
    Settings settings(kNvsNs, true);
    settings.SetBool("pending_morning", false);
}

std::string MickeyAlarm::RequestDeepSleep(int hour, int minute, int seconds_from_now) {
    if (sleep_requested_) {
        return "{\"ok\":false,\"error\":\"sleep already requested\"}";
    }

    uint64_t sleep_us = 0;
    if (seconds_from_now > 0) {
        sleep_us = static_cast<uint64_t>(seconds_from_now) * 1000000ULL;
        if (hour >= 0 && minute >= 0) {
            if (!SetAlarm(hour, minute, GetAlarm().repeat)) {
                return "{\"ok\":false,\"error\":\"invalid hour/minute\"}";
            }
        }
    } else {
        int use_hour = hour;
        int use_minute = minute;
        if (use_hour < 0 || use_minute < 0) {
            auto stored = GetAlarm();
            if (!stored.enabled) {
                return "{\"ok\":false,\"error\":\"no wake time; set hour/minute or seconds\"}";
            }
            use_hour = stored.hour;
            use_minute = stored.minute;
        } else if (!SetAlarm(use_hour, use_minute, GetAlarm().repeat)) {
            return "{\"ok\":false,\"error\":\"invalid hour/minute\"}";
        }

        if (!IsClockSynced()) {
            return "{\"ok\":false,\"error\":\"clock not synced; wait for activation or pass seconds\"}";
        }
        int64_t next_epoch = 0;
        if (!ComputeNextEpoch(use_hour, use_minute, &next_epoch)) {
            return "{\"ok\":false,\"error\":\"invalid hour/minute\"}";
        }
        time_t now = time(nullptr);
        int64_t delta = next_epoch - static_cast<int64_t>(now);
        if (delta < 1) {
            delta = 1;
        }
        sleep_us = static_cast<uint64_t>(delta) * 1000000ULL;
    }

    {
        Settings settings(kNvsNs, true);
        settings.SetBool("pending_morning", true);
    }

    sleep_us_ = sleep_us;
    sleep_requested_ = true;
    ESP_LOGI(TAG, "Deep sleep requested, wake in %" PRIu64 " us", sleep_us);

    BaseType_t ok = xTaskCreate(SleepTask, "mickey_sleep", kSleepTaskStack, this, kSleepTaskPriority,
                                nullptr);
    if (ok != pdPASS) {
        sleep_requested_ = false;
        ClearPendingMorning();
        return "{\"ok\":false,\"error\":\"failed to start sleep task\"}";
    }
    return "{\"ok\":true,\"sleeping\":true}";
}

void MickeyAlarm::SleepTask(void* arg) {
    auto* self = static_cast<MickeyAlarm*>(arg);
    self->EnterDeepSleepOnTask();
    self->sleep_requested_ = false;
    vTaskDelete(nullptr);
}

void MickeyAlarm::EnterDeepSleepOnTask() {
    auto& app = Application::GetInstance();

    app.Schedule([]() {
        auto& application = Application::GetInstance();
        DeviceState state = application.GetDeviceState();
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
            state == kDeviceStateConnecting) {
            application.SetDeviceState(kDeviceStateIdle);
        }
        application.ResetProtocol();
    });

    app.GetAudioService().EnableWakeWordDetection(false);

    const int step_ms = 100;
    int waited = 0;
    while (waited < kSleepIdleWaitMs) {
        if (app.CanEnterSleepMode()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
    }
    if (!app.CanEnterSleepMode()) {
        ESP_LOGW(TAG, "Entering deep sleep without a fully idle audio path");
    }

    OttoPrepareForSleep();

    auto& board = Board::GetInstance();
    Display* display = board.GetDisplay();
    if (display != nullptr) {
        display->SetEmotion("sleepy");
        display->SetPowerSaveMode(true);
    }
    Backlight* backlight = board.GetBacklight();
    if (backlight != nullptr) {
        backlight->SetBrightness(0);
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    ArmAndSleep(sleep_us_);
}

void MickeyAlarm::ArmAndSleep(uint64_t sleep_us) {
    ESP_LOGI(TAG, "Arming RTC timer for %" PRIu64 " us", sleep_us);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(sleep_us));

    if (rtc_gpio_is_valid_gpio(BOOT_BUTTON_GPIO)) {
        esp_err_t ext0 = esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0);
        if (ext0 == ESP_OK) {
            rtc_gpio_pullup_en(BOOT_BUTTON_GPIO);
            rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO);
            esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
            ESP_LOGI(TAG, "EXT0 wake enabled on GPIO %d", static_cast<int>(BOOT_BUTTON_GPIO));
        } else {
            ESP_LOGW(TAG, "EXT0 wake not available: %s", esp_err_to_name(ext0));
        }
    }

    ESP_LOGI(TAG, "Entering deep sleep");
    esp_deep_sleep_start();
}

bool MickeyAlarm::WokeFromTimer() const {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    uint32_t causes = esp_sleep_get_wakeup_causes();
    return (causes & (1UL << ESP_SLEEP_WAKEUP_TIMER)) != 0;
#else
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
#endif
}

bool MickeyAlarm::WokeFromExt0() const {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    uint32_t causes = esp_sleep_get_wakeup_causes();
    return (causes & (1UL << ESP_SLEEP_WAKEUP_EXT0)) != 0;
#else
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
#endif
}

void MickeyAlarm::StartMorningWatcher() {
    if (WokeFromExt0() && !WokeFromTimer()) {
        ClearPendingMorning();
        ESP_LOGI(TAG, "Woke from boot button; skipping morning routine");
        return;
    }
    if (!WokeFromTimer()) {
        return;
    }

    Settings settings(kNvsNs, false);
    if (!settings.GetBool("pending_morning", false)) {
        ESP_LOGI(TAG, "Timer wake without pending morning flag");
        return;
    }

    idle_hold_ticks_ = 0;
    esp_timer_create_args_t args = {};
    args.callback = MorningTimerCallback;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "mickey_morning";
    ESP_ERROR_CHECK(esp_timer_create(&args, &morning_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(morning_timer_, 1000000));
    ESP_LOGI(TAG, "Morning watcher started (timer wakeup)");
}

void MickeyAlarm::MorningTimerCallback(void* arg) {
    static_cast<MickeyAlarm*>(arg)->OnMorningTick();
}

void MickeyAlarm::OnMorningTick() {
    Settings settings(kNvsNs, false);
    if (!settings.GetBool("pending_morning", false)) {
        StopMorningWatcher();
        return;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        idle_hold_ticks_ = 0;
        return;
    }
    idle_hold_ticks_++;
    if (idle_hold_ticks_ < kMorningIdleHoldTicks) {
        return;
    }
    StopMorningWatcher();
    Application::GetInstance().Schedule([]() { MickeyAlarm::GetInstance().RunMorningRoutine(); });
}

void MickeyAlarm::StopMorningWatcher() {
    if (morning_timer_ != nullptr) {
        // Do not delete from the timer callback; only stop here.
        esp_timer_stop(morning_timer_);
    }
}

void MickeyAlarm::RunMorningRoutine() {
    if (morning_timer_ != nullptr) {
        esp_timer_stop(morning_timer_);
        esp_timer_delete(morning_timer_);
        morning_timer_ = nullptr;
    }
    ESP_LOGI(TAG, "Running morning routine");
    auto& board = Board::GetInstance();
    Display* display = board.GetDisplay();
    if (display != nullptr) {
        display->SetPowerSaveMode(false);
        display->SetEmotion("happy");
    }
    Backlight* backlight = board.GetBacklight();
    if (backlight != nullptr) {
        backlight->RestoreBrightness();
    }

    Application::GetInstance().PlaySound(Lang::Sounds::OGG_VIBRATION);
    OttoQueueMorningWake();

    Settings settings(kNvsNs, true);
    settings.SetBool("pending_morning", false);
    if (!settings.GetBool("repeat", true)) {
        settings.SetBool("enabled", false);
    }
}

void MickeyAlarm::RegisterMcpTools() {
    if (mcp_registered_) {
        return;
    }
    mcp_registered_ = true;
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.mickey.alarm.set",
        "Set the local morning alarm (hour 0-23, minute 0-59, 24-hour local time from OTA "
        "server_time). repeat keeps the alarm after it fires. sleep_now enters Deep Sleep "
        "immediately until that time. Requires a synced clock unless you only store the time.",
        PropertyList({Property("hour", kPropertyTypeInteger, 0, 23),
                      Property("minute", kPropertyTypeInteger, 0, 59),
                      Property("repeat", kPropertyTypeBoolean, true),
                      Property("sleep_now", kPropertyTypeBoolean, false)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int hour = properties["hour"].value<int>();
            int minute = properties["minute"].value<int>();
            bool repeat = properties["repeat"].value<bool>();
            bool sleep_now = properties["sleep_now"].value<bool>();
            if (!SetAlarm(hour, minute, repeat)) {
                return "{\"ok\":false,\"error\":\"invalid hour/minute\"}";
            }
            if (sleep_now) {
                return RequestDeepSleep(hour, minute, 0);
            }
            auto state = GetAlarm();
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "ok", true);
            cJSON_AddBoolToObject(json, "enabled", state.enabled);
            cJSON_AddNumberToObject(json, "hour", state.hour);
            cJSON_AddNumberToObject(json, "minute", state.minute);
            cJSON_AddBoolToObject(json, "repeat", state.repeat);
            cJSON_AddBoolToObject(json, "clock_synced", state.clock_synced);
            cJSON_AddNumberToObject(json, "next_epoch", static_cast<double>(state.next_epoch));
            char* printed = cJSON_PrintUnformatted(json);
            std::string result(printed);
            cJSON_free(printed);
            cJSON_Delete(json);
            return result;
        });

    mcp_server.AddTool(
        "self.mickey.alarm.get",
        "Return the stored morning alarm: enabled, hour, minute, repeat, next_epoch, clock_synced.",
        PropertyList(), [this](const PropertyList&) -> ReturnValue {
            auto state = GetAlarm();
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "enabled", state.enabled);
            cJSON_AddNumberToObject(json, "hour", state.hour);
            cJSON_AddNumberToObject(json, "minute", state.minute);
            cJSON_AddBoolToObject(json, "repeat", state.repeat);
            cJSON_AddBoolToObject(json, "pending_morning", state.pending_morning);
            cJSON_AddBoolToObject(json, "clock_synced", state.clock_synced);
            cJSON_AddNumberToObject(json, "next_epoch", static_cast<double>(state.next_epoch));
            char* printed = cJSON_PrintUnformatted(json);
            std::string result(printed);
            cJSON_free(printed);
            cJSON_Delete(json);
            return result;
        });

    mcp_server.AddTool(
        "self.mickey.alarm.cancel",
        "Clear the stored morning alarm. Does not wake a chip that is already in Deep Sleep.",
        PropertyList(), [this](const PropertyList&) -> ReturnValue {
            Cancel();
            return "{\"ok\":true,\"enabled\":false}";
        });

    mcp_server.AddTool(
        "self.mickey.sleep.now",
        "Enter Deep Sleep until the next alarm. Pass hour and minute to override the stored "
        "alarm, or seconds (1-86400) for a bench-test timer that does not need a synced clock.",
        PropertyList({Property("hour", kPropertyTypeInteger, -1, -1, 23),
                      Property("minute", kPropertyTypeInteger, -1, -1, 59),
                      Property("seconds", kPropertyTypeInteger, 0, 0, 86400)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int hour = properties["hour"].value<int>();
            int minute = properties["minute"].value<int>();
            int seconds = properties["seconds"].value<int>();
            return RequestDeepSleep(hour, minute, seconds);
        });

    ESP_LOGI(TAG, "MCP alarm tools registered");
}
