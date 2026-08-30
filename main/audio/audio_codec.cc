#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", true);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ < 0 || output_volume_ > 100) {
        ESP_LOGW(TAG, "Output volume value (%d) is out of range, setting to 100", output_volume_);
        output_volume_ = 100;
    }

    // Previous firmware defaulted to 70. Software-volume codecs square that
    // value, so 70% is only ~49% amplitude and setup prompts are too quiet.
    // Raise the stored factory default to the HAL maximum (100) once.
    if (!settings.GetBool("max_vol", false)) {
        if (output_volume_ == 70) {
            output_volume_ = 100;
            settings.SetInt("output_volume", output_volume_);
        }
        settings.SetBool("max_vol", true);
    }

    ESP_LOGI(TAG, "Audio codec started, output volume=%d", output_volume_);
}

void AudioCodec::SetOutputVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);

    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
