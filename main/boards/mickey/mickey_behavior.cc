#include "mickey_behavior.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

#include "application.h"
#include "board.h"
#include "config.h"
#include "device_state.h"
#include "display.h"
#include "mickey_sensors.h"
#include "otto_controller.h"

#define TAG "MickeyBehavior"

namespace {

constexpr uint32_t kTickMs = 100;
constexpr int kFaceMinGapMs = 8000;
constexpr int kFaceMaxGapMs = 20000;
constexpr int kPostChatMinGapMs = 10000;
constexpr int kPostChatMaxGapMs = 20000;
constexpr int kBodyMinGapMs = 20000;
constexpr int kBodyMaxGapMs = 45000;
constexpr int64_t kBodyIdleMs = 60000;
constexpr int64_t kExternalEmotionYieldUs = 30LL * 1000 * 1000;
constexpr int kPetJitterSpeed = 2000;
constexpr int kPetJitterAmount = 4;
constexpr int kEmotionJitterSpeed = 600;
constexpr int kEmotionJitterAmount = 6;
constexpr int kMicroSwaySpeed = 3000;
constexpr int kMicroSwayAmount = 8;
constexpr int kNightHourStart = 22;
constexpr int kMorningHour = 7;
constexpr int kNightDimStep = 20;
constexpr uint8_t kNightDimFloor = 15;
constexpr UBaseType_t kTaskPriority = 3;
constexpr uint32_t kTaskStack = 4096;

struct FidgetClip {
    const char* id;
    int weight;
    const char* emotion;
    OttoFidgetMotion motion;
    int steps;
    int speed;
    int direction;
    int amount;
    int duration_ms;
    int min_idle_ms;
};

// Face-only clips may run immediately. Body clips wait for 60 s of inactivity,
// then slow-sway or take occasional reduced-amplitude forward steps.
constexpr FidgetClip kClips[] = {
    {"blink", 35, "winking", kOttoFidgetNone, 0, 0, 0, 0, 800, 0},
    {"loving", 25, "loving", kOttoFidgetNone, 0, 0, 0, 0, 1200, 0},
    {"sleepy", 15, "sleepy", kOttoFidgetNone, 0, 0, 0, 0, 1500, 60000},
    {"sway", 25, "happy", kOttoFidgetSwing, 2, 2800, 0, 20, 6100, 60000},
    {"slowStep", 10, "happy", kOttoFidgetWalk, 2, 3200, 1, 18, 6900, 60000},
};

int RandomRange(int min_inclusive, int max_inclusive) {
    if (max_inclusive <= min_inclusive) {
        return min_inclusive;
    }
    uint32_t span = static_cast<uint32_t>(max_inclusive - min_inclusive + 1);
    return min_inclusive + static_cast<int>(esp_random() % span);
}

int64_t NowUs() { return esp_timer_get_time(); }

bool EmotionEquals(const char* a, const char* b) {
    return a != nullptr && b != nullptr && strcmp(a, b) == 0;
}

class MickeyBehavior {
public:
    void Start() {
        if (task_handle_ != nullptr) {
            return;
        }

        auto& app = Application::GetInstance();
        app.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
            OnStateChanged(old_state, new_state);
        });
        app.RegisterExternalEmotionCallback(
            [this](const char* emotion) { NotifyExternalEmotion(emotion); });

        ScheduleNextGap(0);
        xTaskCreate(TaskEntry, "mickey_fidget", kTaskStack, this, kTaskPriority, &task_handle_);
        ESP_LOGI(TAG, "Idle director started (body motion after 60 s inactivity)");
    }

    void Pause() {
        paused_.store(true);
        InvalidateClip();
        OttoCancelFidget();
        NotifyTask();
    }

    void Resume() {
        paused_.store(false);
        ResetUserIdle();
        RestoreNightDimIfNeeded(false);
        ScheduleNextGap(0);
        NotifyTask();
    }

    void NotifyExternalEmotion(const char* emotion) {
        yield_until_us_.store(NowUs() + kExternalEmotionYieldUs);
        {
            std::lock_guard<std::mutex> lock(llm_mutex_);
            last_llm_emotion_ = (emotion != nullptr) ? emotion : "";
        }
        InvalidateClip();
        OttoCancelFidget();
        PlayEmotionGesture(emotion);
        NotifyTask();
    }

    void OnPetBegin() {
        ResetUserIdle();
        InvalidateClip();
        OttoCancelFidget();
        pet_active_.store(true);
        pet_afterglow_until_us_.store(0);

        ScheduleEmotion("happy");

        auto state = Application::GetInstance().GetDeviceState();
        if (state == kDeviceStateIdle && !OttoIsBusy()) {
            if (!OttoTryQueueFidget(kOttoFidgetJitter, 1, kPetJitterSpeed, 0, kPetJitterAmount)) {
                ESP_LOGD(TAG, "Pet jitter skipped; otto busy");
            }
        }

        ESP_LOGI(TAG, "Pet begin state=%d", static_cast<int>(state));
        NotifyTask();
    }

    void OnPetEnd() {
        pet_active_.store(false);
        pet_afterglow_until_us_.store(NowUs() +
                                      static_cast<int64_t>(MICKEY_PET_AFTERGLOW_MS) * 1000);
        ESP_LOGI(TAG, "Pet release; holding happy for %d ms", MICKEY_PET_AFTERGLOW_MS);
        NotifyTask();
    }

    void OnImuEvent(int event) {
        switch (event) {
            case kMickeyImuFall:
                paused_.store(true);
                InvalidateClip();
                OttoCancelFidget();
                break;
            case kMickeyImuPickup:
                ResetUserIdle();
                paused_.store(true);
                InvalidateClip();
                OttoCancelFidget();
                ScheduleEmotion("surprised");
                break;
            case kMickeyImuPutdown:
                paused_.store(false);
                ResetUserIdle();
                ScheduleNextGap(0);
                if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    ScheduleEmotion("staticstate");
                }
                break;
            case kMickeyImuShake:
                ResetUserIdle();
                break;
            default:
                break;
        }
        NotifyTask();
    }

private:
    static void TaskEntry(void* arg) { static_cast<MickeyBehavior*>(arg)->Run(); }

    void OnStateChanged(DeviceState old_state, DeviceState new_state) {
        if (new_state == kDeviceStateListening) {
            InvalidateClip();
            OttoCancelFidget();
            NotifyTask();
            return;
        }

        if (new_state == kDeviceStateSpeaking) {
            InvalidateClip();
            OttoCancelFidget();
            MaybeQueueMicroSway();
            NotifyTask();
            return;
        }

        if (old_state == kDeviceStateIdle && new_state != kDeviceStateIdle) {
            InvalidateClip();
            OttoCancelFidget();
            pet_active_.store(false);
            pet_afterglow_until_us_.store(0);
        } else if (new_state == kDeviceStateIdle) {
            OttoCancelFidget();
            ResetUserIdle();
            if (old_state == kDeviceStateListening || old_state == kDeviceStateSpeaking) {
                SchedulePostChatGap();
            } else {
                ScheduleNextGap(0);
            }
            ApplyIdleRestFace();
        }
        NotifyTask();
    }

    void NotifyTask() {
        if (task_handle_ != nullptr) {
            xTaskNotifyGive(task_handle_);
        }
    }

    void InvalidateClip() {
        clip_generation_.fetch_add(1);
        revert_emotion_.store(false);
    }

    void ResetUserIdle() { user_idle_since_us_.store(NowUs()); }

    void ScheduleNextGap(int64_t idle_ms) {
        int min_gap = idle_ms >= kBodyIdleMs ? kBodyMinGapMs : kFaceMinGapMs;
        int max_gap = idle_ms >= kBodyIdleMs ? kBodyMaxGapMs : kFaceMaxGapMs;
        next_clip_us_.store(NowUs() + static_cast<int64_t>(RandomRange(min_gap, max_gap)) * 1000);
    }

    void SchedulePostChatGap() {
        next_clip_us_.store(
            NowUs() + static_cast<int64_t>(RandomRange(kPostChatMinGapMs, kPostChatMaxGapMs)) *
                          1000);
    }

    void ScheduleEmotion(const char* emotion) {
        if (emotion == nullptr || emotion[0] == '\0') {
            return;
        }
        Application::GetInstance().Schedule([emotion_copy = std::string(emotion)]() {
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetEmotion(emotion_copy.c_str());
            }
        });
    }

    std::string CopyLastLlmEmotion() {
        std::lock_guard<std::mutex> lock(llm_mutex_);
        return last_llm_emotion_;
    }

    bool GestureBlocked() const {
        auto state = Application::GetInstance().GetDeviceState();
        if (state != kDeviceStateIdle) {
            return true;
        }
        if (OttoIsBusy() || OttoMotionInhibited()) {
            return true;
        }
        if (paused_.load() || PetBlockingIdle()) {
            return true;
        }
        return false;
    }

    void PlayEmotionGesture(const char* emotion) {
        if (emotion == nullptr || emotion[0] == '\0') {
            return;
        }
        if (GestureBlocked()) {
            ESP_LOGD(TAG, "Emotion gesture skipped for '%s'", emotion);
            return;
        }

        if (EmotionEquals(emotion, "happy") || EmotionEquals(emotion, "laughing") ||
            EmotionEquals(emotion, "loving")) {
            if (!OttoTryQueueFidget(kOttoFidgetJitter, 1, kEmotionJitterSpeed, 0,
                                    kEmotionJitterAmount)) {
                ESP_LOGD(TAG, "Emotion jitter skipped; otto busy");
            } else {
                ESP_LOGI(TAG, "Emotion jitter for '%s'", emotion);
            }
            return;
        }
        if (EmotionEquals(emotion, "sad") || EmotionEquals(emotion, "sleepy")) {
            if (!OttoTryQueueFidget(kOttoFidgetSit, 1, 0, 0, 0)) {
                ESP_LOGD(TAG, "Emotion sit skipped; otto busy");
            } else {
                ESP_LOGI(TAG, "Emotion sit for '%s'", emotion);
            }
            return;
        }
        if (EmotionEquals(emotion, "surprised")) {
            OttoCancelFidget();
            ESP_LOGI(TAG, "Emotion freeze for surprised");
            return;
        }
        // angry and other faces: GIF only
    }

    void MaybeQueueMicroSway() {
        if (OttoIsBusy() || OttoMotionInhibited() || paused_.load() || PetBlockingIdle()) {
            return;
        }
        std::string last = CopyLastLlmEmotion();
        if (last == "sad" || last == "sleepy") {
            return;
        }
        if (!OttoTryQueueFidget(kOttoFidgetSwing, 1, kMicroSwaySpeed, 0, kMicroSwayAmount)) {
            ESP_LOGD(TAG, "Micro-sway skipped; otto busy");
            return;
        }
        ESP_LOGI(TAG, "Speaking micro-sway");
    }

    void ApplyIdleRestFace() {
        if (PetBlockingIdle()) {
            ScheduleEmotion("happy");
            return;
        }
        if (NowUs() < yield_until_us_.load()) {
            std::string last = CopyLastLlmEmotion();
            if (!last.empty()) {
                ScheduleEmotion(last.c_str());
                return;
            }
        }
        ScheduleEmotion("staticstate");
    }

    bool NightMode() const {
        if (!Application::GetInstance().HasServerTime()) {
            return false;
        }
        time_t now = time(nullptr);
        if (now < 0) {
            return false;
        }
        struct tm local = {};
        localtime_r(&now, &local);
        return local.tm_hour >= kNightHourStart || local.tm_hour < kMorningHour;
    }

    void UpdateNightDim() {
        bool night = NightMode();
        if (night && !night_dimmed_.load()) {
            night_dimmed_.store(true);
            Application::GetInstance().Schedule([]() {
                auto backlight = Board::GetInstance().GetBacklight();
                if (backlight == nullptr) {
                    return;
                }
                int next = static_cast<int>(backlight->brightness()) - kNightDimStep;
                if (next < static_cast<int>(kNightDimFloor)) {
                    next = kNightDimFloor;
                }
                backlight->SetBrightness(static_cast<uint8_t>(next), false);
                ESP_LOGI(TAG, "Night dim backlight=%d", next);
            });
        } else if (!night && night_dimmed_.load()) {
            RestoreNightDimIfNeeded(true);
        }
    }

    void RestoreNightDimIfNeeded(bool log) {
        if (!night_dimmed_.exchange(false)) {
            return;
        }
        Application::GetInstance().Schedule([log]() {
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight != nullptr) {
                backlight->RestoreBrightness();
            }
            if (log) {
                ESP_LOGI(TAG, "Restored backlight after night dim");
            }
        });
    }

    const FidgetClip* PickClip(int64_t idle_ms) const {
        if (NightMode()) {
            for (const auto& clip : kClips) {
                if (clip.emotion != nullptr && strcmp(clip.emotion, "sleepy") == 0 &&
                    clip.motion == kOttoFidgetNone) {
                    return &clip;
                }
            }
        }
        int total = 0;
        for (const auto& clip : kClips) {
            if (idle_ms >= clip.min_idle_ms) {
                total += clip.weight;
            }
        }
        if (total <= 0) {
            return nullptr;
        }
        int pick = RandomRange(1, total);
        for (const auto& clip : kClips) {
            if (idle_ms < clip.min_idle_ms) {
                continue;
            }
            pick -= clip.weight;
            if (pick <= 0) {
                return &clip;
            }
        }
        return &kClips[0];
    }

    void PlayClip(const FidgetClip& clip) {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle || OttoIsBusy()) {
            return;
        }

        if (clip.motion != kOttoFidgetNone && (OttoMotionInhibited() || NightMode())) {
            ESP_LOGD(TAG, "Fidget '%s' skipped; battery low or night", clip.id);
            return;
        }

        uint32_t generation = clip_generation_.load();
        if (clip.emotion != nullptr) {
            ScheduleEmotion(clip.emotion);
            revert_emotion_.store(true);
        } else {
            revert_emotion_.store(false);
        }

        if (clip.motion != kOttoFidgetNone) {
            if (!OttoTryQueueFidget(clip.motion, clip.steps, clip.speed, clip.direction,
                                    clip.amount)) {
                ESP_LOGD(TAG, "Fidget '%s' skipped; otto busy", clip.id);
            }
        }

        clip_until_us_.store(NowUs() + static_cast<int64_t>(clip.duration_ms) * 1000);
        clip_generation_at_play_.store(generation);
        ESP_LOGI(TAG, "Clip '%s' emotion=%s motion=%d", clip.id, clip.emotion ? clip.emotion : "-",
                 static_cast<int>(clip.motion));
    }

    void MaybeRevertEmotion() {
        if (!revert_emotion_.load()) {
            return;
        }
        if (NowUs() < clip_until_us_.load()) {
            return;
        }
        if (clip_generation_.load() != clip_generation_at_play_.load()) {
            revert_emotion_.store(false);
            return;
        }
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            revert_emotion_.store(false);
            return;
        }
        if (NightMode()) {
            revert_emotion_.store(false);
            return;
        }
        if (NowUs() < yield_until_us_.load()) {
            return;
        }
        revert_emotion_.store(false);
        ScheduleEmotion("staticstate");
    }

    bool PetBlockingIdle() const {
        if (pet_active_.load()) {
            return true;
        }
        int64_t until = pet_afterglow_until_us_.load();
        return until != 0 && NowUs() < until;
    }

    void MaybeFinishPetAfterglow() {
        if (pet_active_.load()) {
            return;
        }
        int64_t until = pet_afterglow_until_us_.load();
        if (until == 0 || NowUs() < until) {
            return;
        }
        pet_afterglow_until_us_.store(0);
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            return;
        }
        ScheduleEmotion("staticstate");
        OttoQueueHome();
        ResetUserIdle();
        ScheduleNextGap(0);
        ESP_LOGI(TAG, "Pet afterglow done; home + staticstate");
    }

    void MaybeResetOnUserMotion() {
        if (OttoIsBusy() && !OttoIsFidgeting()) {
            ResetUserIdle();
        }
    }

    void Run() {
        while (true) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTickMs));

            MaybeResetOnUserMotion();
            MaybeFinishPetAfterglow();
            UpdateNightDim();

            if (paused_.load()) {
                MaybeRevertEmotion();
                continue;
            }

            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateIdle) {
                MaybeRevertEmotion();
                continue;
            }

            MaybeRevertEmotion();

            if (PetBlockingIdle()) {
                continue;
            }

            int64_t now = NowUs();
            if (now < yield_until_us_.load()) {
                continue;
            }
            if (now < next_clip_us_.load()) {
                continue;
            }
            if (OttoIsBusy()) {
                continue;
            }

            int64_t idle_since = user_idle_since_us_.load();
            if (idle_since == 0) {
                user_idle_since_us_.store(now);
                idle_since = now;
            }
            int64_t idle_ms = (now - idle_since) / 1000;

            const FidgetClip* clip = PickClip(idle_ms);
            if (clip != nullptr) {
                PlayClip(*clip);
            }
            ScheduleNextGap(idle_ms);
        }
    }

    TaskHandle_t task_handle_ = nullptr;
    std::mutex llm_mutex_;
    std::string last_llm_emotion_;
    std::atomic<bool> paused_{false};
    std::atomic<bool> pet_active_{false};
    std::atomic<bool> revert_emotion_{false};
    std::atomic<bool> night_dimmed_{false};
    std::atomic<uint32_t> clip_generation_{0};
    std::atomic<uint32_t> clip_generation_at_play_{0};
    std::atomic<int64_t> user_idle_since_us_{0};
    std::atomic<int64_t> next_clip_us_{0};
    std::atomic<int64_t> clip_until_us_{0};
    std::atomic<int64_t> yield_until_us_{0};
    std::atomic<int64_t> pet_afterglow_until_us_{0};
};

MickeyBehavior* g_behavior = nullptr;

}  // namespace

void InitializeMickeyBehavior() {
    if (g_behavior == nullptr) {
        g_behavior = new MickeyBehavior();
    }
    g_behavior->Start();
}

void MickeyBehaviorPause() {
    if (g_behavior != nullptr) {
        g_behavior->Pause();
    }
}

void MickeyBehaviorResume() {
    if (g_behavior != nullptr) {
        g_behavior->Resume();
    }
}

void MickeyBehaviorNotifyExternalEmotion(const char* emotion) {
    if (g_behavior != nullptr) {
        g_behavior->NotifyExternalEmotion(emotion);
    }
}

void MickeyBehaviorOnPetBegin() {
    if (g_behavior != nullptr) {
        g_behavior->OnPetBegin();
    }
}

void MickeyBehaviorOnPetEnd() {
    if (g_behavior != nullptr) {
        g_behavior->OnPetEnd();
    }
}

void MickeyBehaviorOnImuEvent(int event) {
    if (g_behavior != nullptr) {
        g_behavior->OnImuEvent(event);
    }
}
