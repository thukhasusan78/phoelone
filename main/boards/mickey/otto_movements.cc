#include "otto_movements.h"

#include <algorithm>
#include <cmath>

#include "freertos/idf_additions.h"
#include "oscillator.h"

namespace {
float EaseOutCubic(float t) {
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}
}  // namespace

Otto::Otto() {
    is_otto_resting_ = false;
    stop_requested_.store(false);
    for (int i = 0; i < SERVO_COUNT; i++) {
        servo_pins_[i] = -1;
        servo_trim_[i] = 0;
    }
}

Otto::~Otto() {
    DetachServos();
}

unsigned long IRAM_ATTR millis() {
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

void Otto::Init(int left_leg, int right_leg, int left_foot, int right_foot) {
    servo_pins_[LEFT_LEG] = left_leg;
    servo_pins_[RIGHT_LEG] = right_leg;
    servo_pins_[LEFT_FOOT] = left_foot;
    servo_pins_[RIGHT_FOOT] = right_foot;

    AttachServos();
    is_otto_resting_ = false;
    stop_requested_.store(false);
}

void Otto::RequestStop() {
    stop_requested_.store(true);
}

void Otto::ClearStop() {
    stop_requested_.store(false);
}

bool Otto::IsStopRequested() const {
    return stop_requested_.load();
}

void Otto::AttachServos() {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].Attach(servo_pins_[i]);
        }
    }
}

void Otto::DetachServos() {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].StopPwm();
        }
    }
}

void Otto::SetTrims(int left_leg, int right_leg, int left_foot, int right_foot) {
    servo_trim_[LEFT_LEG] = left_leg;
    servo_trim_[RIGHT_LEG] = right_leg;
    servo_trim_[LEFT_FOOT] = left_foot;
    servo_trim_[RIGHT_FOOT] = right_foot;

    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].SetTrim(servo_trim_[i]);
        }
    }
}

void Otto::MoveServos(int time, int servo_target[]) {
    if (GetRestState() == true) {
        SetRestState(false);
    }

    if (time <= 10) {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (servo_pins_[i] != -1) {
                servo_[i].SetPosition(servo_target[i]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(time));
        return;
    }

    int start[SERVO_COUNT];
    for (int i = 0; i < SERVO_COUNT; i++) {
        start[i] = servo_[i].GetPosition();
    }

    int steps = std::max(1, time / 10);
    for (int step = 1; step <= steps; step++) {
        if (stop_requested_.load()) {
            return;
        }
        float t = static_cast<float>(step) / static_cast<float>(steps);
        float eased_t = EaseOutCubic(t);
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (servo_pins_[i] != -1) {
                float interpolated = start[i] + (servo_target[i] - start[i]) * eased_t;
                servo_[i].SetPosition(static_cast<int>(std::round(interpolated)));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].SetPosition(servo_target[i]);
        }
    }
}

void Otto::MoveSingle(int position, int servo_number) {
    if (position > 180)
        position = 90;
    if (position < 0)
        position = 90;

    if (GetRestState() == true) {
        SetRestState(false);
    }

    if (servo_number >= 0 && servo_number < SERVO_COUNT && servo_pins_[servo_number] != -1) {
        servo_[servo_number].SetPosition(position);
    }
}

void Otto::OscillateServos(int amplitude[SERVO_COUNT], int offset[SERVO_COUNT], int period,
                           double phase_diff[SERVO_COUNT], float cycle = 1) {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].SetO(offset[i]);
            servo_[i].SetA(amplitude[i]);
            servo_[i].SetT(period);
            servo_[i].SetPh(phase_diff[i]);
        }
    }

    double ref = millis();
    double end_time = period * cycle + ref;

    while (millis() < end_time) {
        if (stop_requested_.load()) {
            return;
        }
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (servo_pins_[i] != -1) {
                servo_[i].Refresh();
            }
        }
        vTaskDelay(5);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

void Otto::Execute(int amplitude[SERVO_COUNT], int offset[SERVO_COUNT], int period,
                   double phase_diff[SERVO_COUNT], float steps = 1.0) {
    if (GetRestState() == true) {
        SetRestState(false);
    }

    int cycles = (int)steps;

    if (cycles >= 1)
        for (int i = 0; i < cycles; i++) {
            if (stop_requested_.load()) {
                return;
            }
            OscillateServos(amplitude, offset, period, phase_diff);
        }

    if (stop_requested_.load()) {
        return;
    }
    OscillateServos(amplitude, offset, period, phase_diff, (float)steps - cycles);
    vTaskDelay(pdMS_TO_TICKS(10));
}

void Otto::Execute2(int amplitude[SERVO_COUNT], int center_angle[SERVO_COUNT], int period,
                    double phase_diff[SERVO_COUNT], float steps = 1.0) {
    if (GetRestState() == true) {
        SetRestState(false);
    }

    int offset[SERVO_COUNT];
    for (int i = 0; i < SERVO_COUNT; i++) {
        offset[i] = center_angle[i] - 90;
    }

    int cycles = (int)steps;

    if (cycles >= 1)
        for (int i = 0; i < cycles; i++) {
            if (stop_requested_.load()) {
                return;
            }
            OscillateServos(amplitude, offset, period, phase_diff);
        }

    if (stop_requested_.load()) {
        return;
    }
    OscillateServos(amplitude, offset, period, phase_diff, (float)steps - cycles);
    vTaskDelay(pdMS_TO_TICKS(10));
}

void Otto::Home() {
    // Always reapply the rest pose so PWM keeps holding after a cooperative stop.
    stop_requested_.store(false);

    int homes[SERVO_COUNT] = {90, 90, 90, 90};
    int max_delta = 0;
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            max_delta = std::max(max_delta, std::abs(homes[i] - servo_[i].GetPosition()));
        }
    }

    int home_time = std::clamp(500 + max_delta * 9, 500, 1700);
    MoveServos(home_time, homes);
    is_otto_resting_ = true;

    vTaskDelay(pdMS_TO_TICKS(200));
}

bool Otto::GetRestState() {
    return is_otto_resting_;
}

void Otto::SetRestState(bool state) {
    is_otto_resting_ = state;
}

void Otto::Jump(float steps, int period) {
    int up[SERVO_COUNT] = {90, 90, 150, 30};
    MoveServos(period, up);
    int down[SERVO_COUNT] = {90, 90, 90, 90};
    MoveServos(period, down);
}

void Otto::Walk(float steps, int period, int dir, int amplitude) {
    if (amplitude <= 0) {
        amplitude = 30;
    }
    int A[SERVO_COUNT] = {amplitude, amplitude, amplitude, amplitude};
    int O[SERVO_COUNT] = {0, 0, 5, -5};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(dir * -90), DEG2RAD(dir * -90)};

    Execute(A, O, period, phase_diff, steps);
}

void Otto::Turn(float steps, int period, int dir) {
    int A[SERVO_COUNT] = {30, 30, 30, 30};
    int O[SERVO_COUNT] = {0, 0, 5, -5};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(-90), DEG2RAD(-90)};

    if (dir == LEFT) {
        A[0] = 30;
        A[1] = 0;
    } else {
        A[0] = 0;
        A[1] = 30;
    }

    Execute(A, O, period, phase_diff, steps);
}

void Otto::Bend(int steps, int period, int dir) {
    int bend1[SERVO_COUNT] = {90, 90, 62, 35};
    int bend2[SERVO_COUNT] = {90, 90, 62, 105};
    int homes[SERVO_COUNT] = {90, 90, 90, 90};

    if (dir == -1) {
        bend1[2] = 180 - 35;
        bend1[3] = 180 - 60;
        bend2[2] = 180 - 105;
        bend2[3] = 180 - 60;
    }

    int T2 = 800;

    for (int i = 0; i < steps; i++) {
        MoveServos(T2 / 2, bend1);
        MoveServos(T2 / 2, bend2);
        vTaskDelay(pdMS_TO_TICKS(period * 0.8));
        MoveServos(500, homes);
    }
}

void Otto::ShakeLeg(int steps, int period, int dir) {
    int numberLegMoves = 2;

    int shake_leg1[SERVO_COUNT] = {90, 90, 58, 35};
    int shake_leg2[SERVO_COUNT] = {90, 90, 58, 120};
    int shake_leg3[SERVO_COUNT] = {90, 90, 58, 60};
    int homes[SERVO_COUNT] = {90, 90, 90, 90};

    if (dir == LEFT) {
        shake_leg1[2] = 180 - 35;
        shake_leg1[3] = 180 - 58;
        shake_leg2[2] = 180 - 120;
        shake_leg2[3] = 180 - 58;
        shake_leg3[2] = 180 - 60;
        shake_leg3[3] = 180 - 58;
    }

    int T2 = 1000;
    period = period - T2;
    period = std::max(period, 200 * numberLegMoves);

    for (int j = 0; j < steps; j++) {
        MoveServos(T2 / 2, shake_leg1);
        MoveServos(T2 / 2, shake_leg2);

        for (int i = 0; i < numberLegMoves; i++) {
            MoveServos(period / (2 * numberLegMoves), shake_leg3);
            MoveServos(period / (2 * numberLegMoves), shake_leg2);
        }
        MoveServos(500, homes);
    }

    vTaskDelay(pdMS_TO_TICKS(period));
}

void Otto::Sit() {
    int target[SERVO_COUNT] = {120, 60, 0, 180};
    MoveServos(600, target);
}

void Otto::UpDown(float steps, int period, int height) {
    int A[SERVO_COUNT] = {0, 0, height, height};
    int O[SERVO_COUNT] = {0, 0, height, -height};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(-90), DEG2RAD(90)};

    Execute(A, O, period, phase_diff, steps);
}

void Otto::Swing(float steps, int period, int height) {
    int A[SERVO_COUNT] = {0, 0, height, height};
    int O[SERVO_COUNT] = {0, 0, height / 2, -height / 2};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(0), DEG2RAD(0)};

    Execute(A, O, period, phase_diff, steps);
}

void Otto::TiptoeSwing(float steps, int period, int height) {
    int A[SERVO_COUNT] = {0, 0, height, height};
    int O[SERVO_COUNT] = {0, 0, height, -height};
    double phase_diff[SERVO_COUNT] = {0, 0, 0, 0};

    Execute(A, O, period, phase_diff, steps);
}

void Otto::Jitter(float steps, int period, int height) {
    height = std::min(25, height);
    int A[SERVO_COUNT] = {height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, 0, 0};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(-90), DEG2RAD(90), 0, 0};

    Execute(A, O, period, phase_diff, steps);
}

void Otto::AscendingTurn(float steps, int period, int height) {
    height = std::min(13, height);
    int A[SERVO_COUNT] = {height, height, height, height};
    int O[SERVO_COUNT] = {0, 0, height + 4, -height + 4};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(-90), DEG2RAD(90), DEG2RAD(-90), DEG2RAD(90)};

    Execute(A, O, period, phase_diff, steps);
}

void Otto::Moonwalker(float steps, int period, int height, int dir) {
    int A[SERVO_COUNT] = {0, 0, height, height};
    int O[SERVO_COUNT] = {0, 0, height / 2 + 2, -height / 2 - 2};
    int phi = -dir * 90;
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(phi), DEG2RAD(-60 * dir + phi)};

    Execute(A, O, period, phase_diff, steps);
}

void Otto::Crusaito(float steps, int period, int height, int dir) {
    int A[SERVO_COUNT] = {25, 25, height, height};
    int O[SERVO_COUNT] = {0, 0, height / 2 + 4, -height / 2 - 4};
    double phase_diff[SERVO_COUNT] = {90, 90, DEG2RAD(0), DEG2RAD(-60 * dir)};

    Execute(A, O, period, phase_diff, steps);
}

void Otto::Flapping(float steps, int period, int height, int dir) {
    int A[SERVO_COUNT] = {12, 12, height, height};
    int O[SERVO_COUNT] = {0, 0, height - 10, -height + 10};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(0), DEG2RAD(180), DEG2RAD(-90 * dir),
                                      DEG2RAD(90 * dir)};

    Execute(A, O, period, phase_diff, steps);
}

void Otto::WhirlwindLeg(float steps, int period, int amplitude) {
    int target[SERVO_COUNT] = {90, 90, 180, 90};
    MoveServos(100, target);
    target[RIGHT_FOOT] = 160;
    MoveServos(500, target);
    vTaskDelay(pdMS_TO_TICKS(1000));

    int C[SERVO_COUNT] = {90, 90, 180, 160};
    int A[SERVO_COUNT] = {amplitude, 0, 0, 0};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(20), 0, 0, 0};
    Execute2(A, C, period, phase_diff, steps);
}

void Otto::Showcase() {
    if (GetRestState() == true) {
        SetRestState(false);
    }

    Walk(3, 1000, FORWARD);
    vTaskDelay(pdMS_TO_TICKS(500));

    Moonwalker(3, 900, 25, LEFT);
    vTaskDelay(pdMS_TO_TICKS(500));

    Swing(3, 1000, 30);
    vTaskDelay(pdMS_TO_TICKS(500));

    Walk(3, 1000, BACKWARD);
}

void Otto::EnableServoLimit(int diff_limit) {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].SetLimiter(diff_limit);
        }
    }
}

void Otto::DisableServoLimit() {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].DisableLimiter();
        }
    }
}
