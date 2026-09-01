#include "mickey_sensors.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "config.h"
#include "device_state.h"
#include "mickey_behavior.h"
#include "otto_controller.h"

#define TAG "MickeySensors"

namespace {

constexpr uint32_t kPollMs = 50;
constexpr uint32_t kTouchDebounceMs = 40;
constexpr uint32_t kNotifyMinGapMs = 500;
constexpr UBaseType_t kTaskPriority = 3;
constexpr uint32_t kTaskStack = 5120;
constexpr int kI2cHz = 100000;

constexpr uint8_t kMpuWhoAmI = 0x75;
constexpr uint8_t kMpuPwrMgmt1 = 0x6B;
constexpr uint8_t kMpuConfig = 0x1A;
constexpr uint8_t kMpuGyroConfig = 0x1B;
constexpr uint8_t kMpuAccelConfig = 0x1C;
constexpr uint8_t kMpuMotThr = 0x1F;
constexpr uint8_t kMpuMotDur = 0x20;
constexpr uint8_t kMpuIntPinCfg = 0x37;
constexpr uint8_t kMpuIntEnable = 0x38;
constexpr uint8_t kMpuAccelXoutH = 0x3B;
constexpr uint8_t kMpuWhoAmIExpect = 0x68;

constexpr float kAccelScale = 16384.0f;
constexpr float kGyroScale = 131.0f;
constexpr float kRadToDeg = 57.2957795f;

constexpr float kStillAccelBandG = 0.15f;
constexpr float kStillGyroDps = 40.0f;
constexpr float kFallTiltDeg = 55.0f;
constexpr float kFallAccelLowG = 0.25f;
constexpr float kFallGyroDps = 180.0f;
constexpr int64_t kFallHoldUs = 150000;
constexpr float kPickupAccelBandG = 0.28f;
constexpr float kPickupTiltDeg = 28.0f;
constexpr int64_t kPutdownHoldUs = 300000;
constexpr float kShakeGyroDps = 200.0f;
constexpr int64_t kFidgetImuGraceUs = 400000;
constexpr int64_t kFidgetFallHoldUs = 250000;

#if MICKEY_TOUCH_ACTIVE_HIGH
constexpr int kTouchActiveLevel = 1;
#else
constexpr int kTouchActiveLevel = 0;
#endif

const char* ImuEventName(MickeyImuEvent event) {
    switch (event) {
        case kMickeyImuMoving:
            return "moving";
        case kMickeyImuPickup:
            return "pickup";
        case kMickeyImuPutdown:
            return "putdown";
        case kMickeyImuFall:
            return "fall";
        case kMickeyImuShake:
            return "shake";
        case kMickeyImuStill:
        default:
            return "still";
    }
}

// Nano printf has no %f; emit two decimal places from integer milles.
void AppendFixed2(std::string& out, float value) {
    bool negative = value < 0.0f;
    if (negative) {
        value = -value;
    }
    int scaled = static_cast<int>(value * 100.0f + 0.5f);
    if (negative && scaled != 0) {
        out += '-';
    }
    out += std::to_string(scaled / 100);
    out += '.';
    int frac = scaled % 100;
    if (frac < 10) {
        out += '0';
    }
    out += std::to_string(frac);
}

int64_t NowUs() { return esp_timer_get_time(); }

class MickeySensors {
public:
    void Start() {
        if (task_handle_ != nullptr) {
            return;
        }

        snapshot_.imu.wired = true;
        snapshot_.imu.ok = false;
        snapshot_.imu.error = "i2c_nack";
        snapshot_.touch.wired = true;

        InitTouch();
        InitImu();

        xTaskCreate(TaskEntry, "mickey_sensor", kTaskStack, this, kTaskPriority, &task_handle_);
        ESP_LOGI(TAG, "Sensor task started (imu_ok=%d touch=%d)", imu_ok_, MICKEY_TOUCH_PIN);
    }

    MickeySensorSnapshot GetSnapshot() {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        return snapshot_;
    }

    std::string ImuJson() {
        MickeyImuSnapshot imu;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            imu = snapshot_.imu;
        }
        if (!imu.ok) {
            std::string json = "{\"wired\":true,\"ok\":false,\"error\":\"";
            json += imu.error ? imu.error : "i2c_nack";
            json += "\"}";
            return json;
        }
        std::string json = "{\"wired\":true,\"sensor\":\"MPU6050\",\"ax\":";
        AppendFixed2(json, imu.ax);
        json += ",\"ay\":";
        AppendFixed2(json, imu.ay);
        json += ",\"az\":";
        AppendFixed2(json, imu.az);
        json += ",\"gx\":";
        AppendFixed2(json, imu.gx);
        json += ",\"gy\":";
        AppendFixed2(json, imu.gy);
        json += ",\"gz\":";
        AppendFixed2(json, imu.gz);
        json += ",\"pitch\":";
        AppendFixed2(json, imu.pitch);
        json += ",\"roll\":";
        AppendFixed2(json, imu.roll);
        json += ",\"temp_c\":";
        AppendFixed2(json, imu.temp_c);
        json += ",\"event\":\"";
        json += ImuEventName(imu.event);
        json += "\"}";
        return json;
    }

    std::string TouchJson() {
        MickeyTouchSnapshot touch;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            touch = snapshot_.touch;
        }
        std::string json = "{\"wired\":true,\"touched\":";
        json += touch.touched ? "true" : "false";
        json += ",\"count\":";
        json += std::to_string(touch.count);
        json += ",\"ms_held\":";
        json += std::to_string(touch.ms_held);
        json += "}";
        return json;
    }

    void EmitEvent(const char* event) {
        MickeyImuSnapshot sample;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            sample = snapshot_.imu;
        }
        EmitNotify(event, sample);
    }

    void ConfigureTouchPin(gpio_int_type_t intr_type) {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << MICKEY_TOUCH_PIN;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = MICKEY_TOUCH_ACTIVE_HIGH ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
        io.pull_down_en = MICKEY_TOUCH_ACTIVE_HIGH ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        io.intr_type = intr_type;
        gpio_config(&io);
    }

    void PrepareForSleep() {
        sleep_requested_.store(true, std::memory_order_release);
        gpio_isr_handler_remove(MICKEY_TOUCH_PIN);
        gpio_isr_handler_remove(MICKEY_IMU_INT);
        ConfigureTouchPin(GPIO_INTR_DISABLE);
        gpio_set_intr_type(MICKEY_IMU_INT, GPIO_INTR_DISABLE);

        if (task_handle_ != nullptr) {
            xTaskNotifyGive(task_handle_);
            const int timeout_ms = 500;
            int waited = 0;
            while (!sleep_idle_.load(std::memory_order_acquire) && waited < timeout_ms) {
                vTaskDelay(pdMS_TO_TICKS(10));
                waited += 10;
            }
            if (waited >= timeout_ms) {
                ESP_LOGW(TAG, "Timed out waiting for sensor task to idle");
            }
            vTaskSuspend(task_handle_);
        }
        ESP_LOGI(TAG, "Sensor task suspended; I2C idle for sleep");
    }

private:
    static void TaskEntry(void* arg) { static_cast<MickeySensors*>(arg)->Run(); }

    static void TouchIsr(void* arg) {
        auto* self = static_cast<MickeySensors*>(arg);
        self->touch_irq_.store(true, std::memory_order_relaxed);
        self->NotifyFromIsr();
    }

    static void ImuIsr(void* arg) {
        auto* self = static_cast<MickeySensors*>(arg);
        self->imu_irq_.store(true, std::memory_order_relaxed);
        self->NotifyFromIsr();
    }

    void NotifyFromIsr() {
        BaseType_t woken = pdFALSE;
        if (task_handle_ != nullptr) {
            vTaskNotifyGiveFromISR(task_handle_, &woken);
        }
        portYIELD_FROM_ISR(woken);
    }

    void InstallIsrService() {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(err));
        }
    }

    void InitTouch() {
        ConfigureTouchPin(GPIO_INTR_ANYEDGE);

        InstallIsrService();
        esp_err_t err = gpio_isr_handler_add(MICKEY_TOUCH_PIN, TouchIsr, this);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Touch ISR add failed: %s", esp_err_to_name(err));
        }
        touch_raw_ = gpio_get_level(MICKEY_TOUCH_PIN) == kTouchActiveLevel;
        ESP_LOGI(TAG, "TTP223 on GPIO %d, level=%d", static_cast<int>(MICKEY_TOUCH_PIN),
                 gpio_get_level(MICKEY_TOUCH_PIN));
    }

    bool ProbeMpu(uint8_t addr) {
        if (i2c_master_probe(i2c_bus_, addr, pdMS_TO_TICKS(50)) != ESP_OK) {
            return false;
        }
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = addr;
        dev_cfg.scl_speed_hz = kI2cHz;
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &i2c_dev_) != ESP_OK) {
            return false;
        }
        uint8_t who = 0;
        if (ReadReg(kMpuWhoAmI, who) != ESP_OK) {
            i2c_master_bus_rm_device(i2c_dev_);
            i2c_dev_ = nullptr;
            return false;
        }
        ESP_LOGI(TAG, "MPU6050 WHO_AM_I=0x%02x at 0x%02x", who, addr);
        if (who != kMpuWhoAmIExpect && who != 0x69) {
            ESP_LOGW(TAG, "Unexpected WHO_AM_I, continuing");
        }
        return true;
    }

    void InitImu() {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = MICKEY_IMU_SDA,
            .scl_io_num = MICKEY_IMU_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        esp_err_t err = i2c_new_master_bus(&bus_cfg, &i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C bus failed: %s", esp_err_to_name(err));
            SetImuError("i2c_nack");
            return;
        }

        if (!ProbeMpu(0x68) && !ProbeMpu(0x69)) {
            ESP_LOGE(TAG, "MPU6050 not found on 0x68/0x69");
            SetImuError("i2c_nack");
            return;
        }

        if (WriteReg(kMpuPwrMgmt1, 0x00) != ESP_OK || WriteReg(kMpuConfig, 0x03) != ESP_OK ||
            WriteReg(kMpuGyroConfig, 0x00) != ESP_OK || WriteReg(kMpuAccelConfig, 0x00) != ESP_OK) {
            ESP_LOGE(TAG, "MPU6050 wake/config failed");
            SetImuError("i2c_nack");
            return;
        }

        // Active-low MOT INT; poll still runs if the INT line is unconnected.
        WriteReg(kMpuMotThr, 40);
        WriteReg(kMpuMotDur, 1);
        WriteReg(kMpuIntPinCfg, 0x90);
        WriteReg(kMpuIntEnable, 0x40);

        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << MICKEY_IMU_INT;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_NEGEDGE;
        gpio_config(&io);
        InstallIsrService();
        err = gpio_isr_handler_add(MICKEY_IMU_INT, ImuIsr, this);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "IMU INT ISR add failed: %s", esp_err_to_name(err));
        }

        imu_ok_ = true;
        SetImuError(nullptr);
        ESP_LOGI(TAG, "MPU6050 ready SDA=%d SCL=%d INT=%d", static_cast<int>(MICKEY_IMU_SDA),
                 static_cast<int>(MICKEY_IMU_SCL), static_cast<int>(MICKEY_IMU_INT));
    }

    void SetImuError(const char* error) {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.imu.wired = true;
        snapshot_.imu.ok = error == nullptr;
        snapshot_.imu.error = error;
    }

    esp_err_t WriteReg(uint8_t reg, uint8_t value) {
        if (i2c_dev_ == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        std::lock_guard<std::mutex> lock(i2c_mutex_);
        uint8_t buf[2] = {reg, value};
        return i2c_master_transmit(i2c_dev_, buf, 2, 50);
    }

    esp_err_t ReadReg(uint8_t reg, uint8_t& value) { return ReadRegs(reg, &value, 1); }

    esp_err_t ReadRegs(uint8_t reg, uint8_t* buf, size_t len) {
        if (i2c_dev_ == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        std::lock_guard<std::mutex> lock(i2c_mutex_);
        return i2c_master_transmit_receive(i2c_dev_, &reg, 1, buf, len, 50);
    }

    bool ReadImuSample(MickeyImuSnapshot& sample) {
        uint8_t raw[14];
        if (ReadRegs(kMpuAccelXoutH, raw, sizeof(raw)) != ESP_OK) {
            consecutive_i2c_fail_++;
            if (consecutive_i2c_fail_ >= 5) {
                imu_ok_ = false;
                SetImuError("i2c_nack");
            }
            return false;
        }
        consecutive_i2c_fail_ = 0;

        auto be16 = [](const uint8_t* p) -> int16_t {
            return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
        };
        int16_t ax_raw = be16(raw + 0);
        int16_t ay_raw = be16(raw + 2);
        int16_t az_raw = be16(raw + 4);
        int16_t temp_raw = be16(raw + 6);
        int16_t gx_raw = be16(raw + 8);
        int16_t gy_raw = be16(raw + 10);
        int16_t gz_raw = be16(raw + 12);

        sample.wired = true;
        sample.ok = true;
        sample.error = nullptr;
        sample.ax = ax_raw / kAccelScale;
        sample.ay = ay_raw / kAccelScale;
        sample.az = az_raw / kAccelScale;
        sample.gx = gx_raw / kGyroScale;
        sample.gy = gy_raw / kGyroScale;
        sample.gz = gz_raw / kGyroScale;
        sample.temp_c = temp_raw / 340.0f + 36.53f;

        float ax = sample.ax;
        float ay = sample.ay;
        float az = sample.az;
        sample.pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * kRadToDeg;
        sample.roll = atan2f(ay, az) * kRadToDeg;
        return true;
    }

    bool FidgetSelfMotionMasked(int64_t now) {
        if (OttoIsFidgeting()) {
            fidget_mask_until_us_ = now + kFidgetImuGraceUs;
        }
        return fidget_mask_until_us_ != 0 && now < fidget_mask_until_us_;
    }

    MickeyImuEvent Classify(const MickeyImuSnapshot& sample, int64_t now) {
        float mag = sqrtf(sample.ax * sample.ax + sample.ay * sample.ay + sample.az * sample.az);
        float tilt = sqrtf(sample.pitch * sample.pitch + sample.roll * sample.roll);
        float gyro = sqrtf(sample.gx * sample.gx + sample.gy * sample.gy + sample.gz * sample.gz);
        float accel_err = fabsf(mag - 1.0f);
        bool masked = FidgetSelfMotionMasked(now);

        // During an intentional fidget, ignore tilt/bounce fall and pickup. Keep
        // true freefall (near-zero |a|) so a real drop still homes the servos.
        bool fall_now = masked ? (mag < kFallAccelLowG)
                               : ((tilt > kFallTiltDeg) ||
                                  (fabsf(sample.az) < kFallAccelLowG && gyro > kFallGyroDps) ||
                                  (mag < kFallAccelLowG && gyro > 80.0f));
        int64_t fall_hold = masked ? kFidgetFallHoldUs : kFallHoldUs;
        if (fall_now) {
            if (fall_since_us_ == 0) {
                fall_since_us_ = now;
            }
            if (now - fall_since_us_ >= fall_hold) {
                putdown_since_us_ = 0;
                return kMickeyImuFall;
            }
        } else {
            fall_since_us_ = 0;
        }

        if (masked) {
            return kMickeyImuStill;
        }

        if (gyro > kShakeGyroDps && tilt < kFallTiltDeg) {
            putdown_since_us_ = 0;
            return kMickeyImuShake;
        }

        bool lifted = accel_err > kPickupAccelBandG || tilt > kPickupTiltDeg;
        if (lifted && !fall_now) {
            putdown_since_us_ = 0;
            return kMickeyImuPickup;
        }

        bool planted = accel_err < kStillAccelBandG && gyro < kStillGyroDps && tilt < 20.0f;
        if (planted) {
            if (last_event_ == kMickeyImuPickup || last_event_ == kMickeyImuFall ||
                last_event_ == kMickeyImuMoving) {
                if (putdown_since_us_ == 0) {
                    putdown_since_us_ = now;
                }
                if (now - putdown_since_us_ >= kPutdownHoldUs) {
                    putdown_since_us_ = 0;
                    return kMickeyImuPutdown;
                }
                return last_event_ == kMickeyImuFall ? kMickeyImuFall : kMickeyImuMoving;
            }
            putdown_since_us_ = 0;
            return kMickeyImuStill;
        }

        putdown_since_us_ = 0;
        return kMickeyImuMoving;
    }

    void PublishImu(const MickeyImuSnapshot& sample) {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.imu = sample;
    }

    void HandleImuEvent(MickeyImuEvent event, const MickeyImuSnapshot& sample) {
        if (event == last_event_) {
            return;
        }
        MickeyImuEvent previous = last_event_;
        last_event_ = event;

        if (event == kMickeyImuFall) {
            OttoStopAndHome();
            MickeyBehaviorOnImuEvent(kMickeyImuFall);
            EmitNotify("fall", sample);
            return;
        }
        if (event == kMickeyImuPickup) {
            MickeyBehaviorOnImuEvent(kMickeyImuPickup);
            EmitNotify("pickup", sample);
            return;
        }
        if (event == kMickeyImuPutdown &&
            (previous == kMickeyImuPickup || previous == kMickeyImuFall ||
             previous == kMickeyImuMoving)) {
            MickeyBehaviorOnImuEvent(kMickeyImuPutdown);
            EmitNotify("putdown", sample);
            return;
        }
        if (event == kMickeyImuShake) {
            MickeyBehaviorOnImuEvent(kMickeyImuShake);
        }
    }

    void SampleTouch(int64_t now) {
        bool irq = touch_irq_.exchange(false, std::memory_order_relaxed);
        (void)irq;
        bool raw = gpio_get_level(MICKEY_TOUCH_PIN) == kTouchActiveLevel;
        if (raw != touch_raw_) {
            touch_raw_ = raw;
            debounce_until_us_ = now + static_cast<int64_t>(kTouchDebounceMs) * 1000;
        }
        if (now < debounce_until_us_) {
            return;
        }

        bool pressed = touch_raw_;
        if (pressed && !touch_stable_) {
            touch_stable_ = true;
            pet_confirmed_ = false;
            touch_down_us_ = now;
            uint32_t count = 0;
            {
                std::lock_guard<std::mutex> lock(snapshot_mutex_);
                snapshot_.touch.touched = true;
                snapshot_.touch.count += 1;
                snapshot_.touch.ms_held = 0;
                count = snapshot_.touch.count;
            }
            ESP_LOGI(TAG, "Touch down count=%u", count);
        } else if (!pressed && touch_stable_) {
            touch_stable_ = false;
            uint32_t held_ms = 0;
            if (touch_down_us_ != 0) {
                held_ms = static_cast<uint32_t>((now - touch_down_us_) / 1000);
            }
            {
                std::lock_guard<std::mutex> lock(snapshot_mutex_);
                snapshot_.touch.touched = false;
                snapshot_.touch.ms_held = held_ms;
            }
            if (pet_confirmed_) {
                pet_confirmed_ = false;
                MickeyBehaviorOnPetEnd();
            } else {
                ESP_LOGD(TAG, "Touch ignored as tap (%u ms)", held_ms);
            }
        } else if (pressed && touch_stable_ && touch_down_us_ != 0) {
            uint32_t held_ms = static_cast<uint32_t>((now - touch_down_us_) / 1000);
            {
                std::lock_guard<std::mutex> lock(snapshot_mutex_);
                snapshot_.touch.touched = true;
                snapshot_.touch.ms_held = held_ms;
            }
            if (!pet_confirmed_ && held_ms >= MICKEY_PET_CONFIRM_MS) {
                pet_confirmed_ = true;
                ESP_LOGI(TAG, "Pet confirmed after %u ms", held_ms);
                MickeyBehaviorOnPetBegin();
                EmitNotify("pet", GetSnapshot().imu);
            }
        }
    }

    void EmitNotify(const char* event, const MickeyImuSnapshot& imu) {
        int64_t now = NowUs();
        bool is_priority = strcmp(event, "fall") == 0 || strcmp(event, "sleep") == 0;
        if (!is_priority) {
            auto state = Application::GetInstance().GetDeviceState();
            if (state == kDeviceStateSpeaking) {
                return;
            }
            if (now - last_notify_us_ < static_cast<int64_t>(kNotifyMinGapMs) * 1000) {
                return;
            }
        } else if (now - last_notify_us_ < 200000) {
            return;
        }
        last_notify_us_ = now;

        std::string payload = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/phoe_lone.event\",";
        payload += "\"params\":{\"event\":\"";
        payload += event;
        payload += "\",\"ts_ms\":";
        payload += std::to_string(now / 1000);
        payload += ",\"imu\":{\"pitch\":";
        AppendFixed2(payload, imu.pitch);
        payload += ",\"az\":";
        AppendFixed2(payload, imu.az);
        payload += "}}}";
        Application::GetInstance().SendMcpMessage(payload);
    }

    void Run() {
        while (true) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPollMs));
            if (sleep_requested_.load(std::memory_order_acquire)) {
                sleep_idle_.store(true, std::memory_order_release);
                while (sleep_requested_.load(std::memory_order_acquire)) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                continue;
            }

            int64_t now = NowUs();
            imu_irq_.store(false, std::memory_order_relaxed);

            if (imu_ok_) {
                MickeyImuSnapshot sample;
                if (ReadImuSample(sample)) {
                    sample.event = Classify(sample, now);
                    PublishImu(sample);
                    HandleImuEvent(sample.event, sample);
                }
            }

            SampleTouch(now);
        }
    }

    TaskHandle_t task_handle_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t i2c_dev_ = nullptr;
    bool imu_ok_ = false;
    int consecutive_i2c_fail_ = 0;

    std::mutex i2c_mutex_;
    std::mutex snapshot_mutex_;
    MickeySensorSnapshot snapshot_{};

    std::atomic<bool> touch_irq_{false};
    std::atomic<bool> imu_irq_{false};
    std::atomic<bool> sleep_requested_{false};
    std::atomic<bool> sleep_idle_{false};
    bool touch_raw_ = false;
    bool touch_stable_ = false;
    bool pet_confirmed_ = false;
    int64_t debounce_until_us_ = 0;
    int64_t touch_down_us_ = 0;
    int64_t last_notify_us_ = 0;
    int64_t fall_since_us_ = 0;
    int64_t putdown_since_us_ = 0;
    int64_t fidget_mask_until_us_ = 0;
    MickeyImuEvent last_event_ = kMickeyImuStill;
};

MickeySensors* g_sensors = nullptr;

}  // namespace

void InitializeMickeySensors() {
#if OTTO_HARDWARE_VERSION == OTTO_VERSION_CAMERA
    ESP_LOGW(TAG, "Skipping sensors on camera SKU (GPIO 40-42/47 occupied)");
    return;
#else
    if (g_sensors == nullptr) {
        g_sensors = new MickeySensors();
    }
    g_sensors->Start();
#endif
}

void MickeySensorsPrepareForSleep() {
    if (g_sensors != nullptr) {
        g_sensors->PrepareForSleep();
    }
}

void MickeySensorsEmitEvent(const char* event) {
    if (g_sensors != nullptr && event != nullptr && event[0] != '\0') {
        g_sensors->EmitEvent(event);
    }
}

MickeySensorSnapshot MickeySensorsGetSnapshot() {
    if (g_sensors == nullptr) {
        MickeySensorSnapshot snap;
        snap.imu.wired = true;
        snap.imu.ok = false;
        snap.imu.error = "not_started";
        snap.touch.wired = true;
        return snap;
    }
    return g_sensors->GetSnapshot();
}

std::string MickeySensorsImuJson() {
    if (g_sensors == nullptr) {
        return "{\"wired\":true,\"ok\":false,\"error\":\"not_started\"}";
    }
    return g_sensors->ImuJson();
}

std::string MickeySensorsTouchJson() {
    if (g_sensors == nullptr) {
        return "{\"wired\":true,\"touched\":false,\"count\":0,\"ms_held\":0}";
    }
    return g_sensors->TouchJson();
}
