#include "mickey_alarm.h"

#include <inttypes.h>
#include <time.h>

#include <driver/rtc_io.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "config.h"
#include "device_state.h"
#include "display.h"
#include "mcp_server.h"
#include "mickey_sensors.h"
#include "otto_controller.h"
#include "settings.h"

#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_wifi.h>

#define TAG "MickeyAlarm"

namespace {
constexpr const char* kNvsNs = "mickey_alarm";
constexpr int kMinValidYear = 2025;
constexpr int kMorningIdleHoldTicks = 3;
constexpr int kSleepIdleWaitMs = 3000;
constexpr int kSleepTaskStack = 4096;
constexpr UBaseType_t kSleepTaskPriority = 3;
constexpr const char* kWakeSrcKey = "wake_src";
constexpr int kWakeSrcNone = 0;
constexpr int kWakeSrcTimer = 1;
constexpr int kWakeSrcButton = 2;
constexpr int kWakeSrcPet = 3;

bool TouchPinActive() {
    int level = gpio_get_level(MICKEY_TOUCH_PIN);
#if MICKEY_TOUCH_ACTIVE_HIGH
    return level == 1;
#else
    return level == 0;
#endif
}

bool BootButtonHeld() { return gpio_get_level(BOOT_BUTTON_GPIO) == 0; }

void PersistWakeSrc(int src) {
    Settings settings(kNvsNs, true);
    settings.SetInt(kWakeSrcKey, src);
}

int ConsumeWakeSrc() {
    Settings settings(kNvsNs, true);
    int src = settings.GetInt(kWakeSrcKey, kWakeSrcNone);
    if (src != kWakeSrcNone) {
        settings.SetInt(kWakeSrcKey, kWakeSrcNone);
    }
    return src;
}

enum class SleepWakeResult { kTimer, kButton, kPet };

gpio_int_type_t TouchWakeIntr() {
#if MICKEY_TOUCH_ACTIVE_HIGH
    return GPIO_INTR_HIGH_LEVEL;
#else
    return GPIO_INTR_LOW_LEVEL;
#endif
}

esp_err_t EnableGpioLightSleepWake() {
    esp_err_t err = gpio_wakeup_enable(MICKEY_TOUCH_PIN, TouchWakeIntr());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Touch GPIO wakeup enable failed: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_wakeup_enable(BOOT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot GPIO wakeup enable failed: %s", esp_err_to_name(err));
        return err;
    }
    return esp_sleep_enable_gpio_wakeup();
}

// GPIO 47 is not an RTC pad, so a level wake starts the CPU immediately.
// Stay awake only if the touch stays asserted for the pet-wake hold.
bool ConfirmTouchSleepHold() {
    const int64_t need_us = static_cast<int64_t>(MICKEY_TOUCH_SLEEP_HOLD_MS) * 1000;
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < need_us) {
        if (BootButtonHeld()) {
            return false;
        }
        if (!TouchPinActive()) {
            ESP_LOGI(TAG, "Sleep touch released before %d ms hold", MICKEY_TOUCH_SLEEP_HOLD_MS);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return TouchPinActive();
}

SleepWakeResult LightSleepUntilWake(uint64_t sleep_us) {
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(sleep_us);
    ESP_LOGI(TAG, "GPIO %d is not an RTC pad; using light sleep so a %d ms pet can wake Mickey",
             static_cast<int>(MICKEY_TOUCH_PIN), MICKEY_TOUCH_SLEEP_HOLD_MS);

    esp_err_t wifi_err = esp_wifi_stop();
    if (wifi_err != ESP_OK && wifi_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(wifi_err));
    }

    while (true) {
        int64_t remaining = deadline_us - esp_timer_get_time();
        if (remaining <= 0) {
            return SleepWakeResult::kTimer;
        }

        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(remaining)));
        esp_err_t gpio_wake = EnableGpioLightSleepWake();
        if (gpio_wake != ESP_OK) {
            ESP_LOGW(TAG, "GPIO light-sleep wake unavailable (%s); timer only",
                     esp_err_to_name(gpio_wake));
        }

        ESP_LOGI(TAG, "Light sleep, remaining %" PRId64 " us", remaining);
        esp_err_t sleep_err = esp_light_sleep_start();
        if (sleep_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_light_sleep_start: %s", esp_err_to_name(sleep_err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        uint32_t causes = esp_sleep_get_wakeup_causes();
        bool timer = (causes & (1UL << ESP_SLEEP_WAKEUP_TIMER)) != 0;
        bool gpio = (causes & (1UL << ESP_SLEEP_WAKEUP_GPIO)) != 0;
#else
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        bool timer = cause == ESP_SLEEP_WAKEUP_TIMER;
        bool gpio = cause == ESP_SLEEP_WAKEUP_GPIO;
#endif
        if (timer || (deadline_us - esp_timer_get_time()) <= 0) {
            return SleepWakeResult::kTimer;
        }
        if (gpio || BootButtonHeld() || TouchPinActive()) {
            if (BootButtonHeld()) {
                return SleepWakeResult::kButton;
            }
            if (TouchPinActive() && ConfirmTouchSleepHold()) {
                ESP_LOGI(TAG, "Pet-to-wake hold confirmed");
                return SleepWakeResult::kPet;
            }
            if (BootButtonHeld()) {
                return SleepWakeResult::kButton;
            }
        }
    }
}
}  // namespace

MickeyAlarm& MickeyAlarm::GetInstance() {
    static MickeyAlarm instance;
    return instance;
}

bool MickeyAlarm::IsClockSynced() const {
    time_t now = time(nullptr);
    struct tm local{};
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
    struct tm local{};
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
            return "{\"ok\":false,\"error\":\"clock not synced; wait for activation or pass "
                   "seconds\"}";
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

    BaseType_t ok =
        xTaskCreate(SleepTask, "mickey_sleep", kSleepTaskStack, this, kSleepTaskPriority, nullptr);
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

    // Tell the companion dashboard we are going to sleep before the socket dies.
    MickeySensorsEmitEvent("sleep");
    vTaskDelay(pdMS_TO_TICKS(400));

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
    MickeySensorsPrepareForSleep();

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
    ESP_LOGI(TAG, "Arming sleep for %" PRIu64 " us", sleep_us);

    SleepWakeResult result = LightSleepUntilWake(sleep_us);
    switch (result) {
        case SleepWakeResult::kPet:
            PersistWakeSrc(kWakeSrcPet);
            break;
        case SleepWakeResult::kButton:
            PersistWakeSrc(kWakeSrcButton);
            break;
        case SleepWakeResult::kTimer:
        default:
            PersistWakeSrc(kWakeSrcTimer);
            break;
    }
    ESP_LOGI(TAG, "Leaving sleep, restart wake_src=%d", static_cast<int>(result));
    esp_restart();
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
    int wake_src = ConsumeWakeSrc();
    if (wake_src == kWakeSrcPet) {
        ClearPendingMorning();
        ESP_LOGI(TAG, "Woke from a %d ms pet hold; skipping morning routine",
                 MICKEY_TOUCH_SLEEP_HOLD_MS);
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->SetPowerSaveMode(false);
            display->SetEmotion("happy");
        }
        auto backlight = Board::GetInstance().GetBacklight();
        if (backlight != nullptr) {
            backlight->RestoreBrightness();
        }
        return;
    }
    if (wake_src == kWakeSrcButton || (WokeFromExt0() && !WokeFromTimer())) {
        ClearPendingMorning();
        ESP_LOGI(TAG, "Woke from boot button; skipping morning routine");
        return;
    }
    if (wake_src != kWakeSrcTimer && !WokeFromTimer()) {
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
