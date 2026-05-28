/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "backends/vad_backend.hpp"
#include "vad_service.h"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "ASSERTION FAILED: " << message << std::endl;
        std::exit(1);
    }
}

class RecordingCallback : public vad::IVadCallback {
public:
    void onResult(const vad::DetectionResult& result) override {
        result_count++;
        last_result = result;
    }

    void onSpeechStart(int64_t timestamp_ms) override {
        speech_start_count++;
        last_speech_start_ms = timestamp_ms;
    }

    void onSpeechEnd(int64_t timestamp_ms, int duration_ms) override {
        speech_end_count++;
        last_speech_end_ms = timestamp_ms;
        last_speech_duration_ms = duration_ms;
    }

    void onError(const vad::ErrorInfo& error) override {
        error_count++;
        last_error = error;
    }

    int result_count = 0;
    int speech_start_count = 0;
    int speech_end_count = 0;
    int error_count = 0;
    int64_t last_speech_start_ms = -1;
    int64_t last_speech_end_ms = -1;
    int last_speech_duration_ms = 0;
    vad::DetectionResult last_result;
    vad::ErrorInfo last_error = vad::ErrorInfo::ok();
};

class FakeVadBackend : public vad::IVadBackend {
public:
    vad::ErrorInfo initialize(const vad::VadConfig& config) override {
        current_config_ = config;
        initialized_ = true;
        return vad::ErrorInfo::ok();
    }

    void shutdown() override {
        initialized_ = false;
        streaming_.store(false);
    }

    bool isInitialized() const override { return initialized_; }
    vad::BackendType getType() const override { return vad::BackendType::CUSTOM; }
    std::string getName() const override { return "fake-vad"; }
    void reset() override {
        state_ = vad::VadState::SILENCE;
        frame_count_ = 0;
        speech_start_ms_ = -1;
    }

    vad::ErrorInfo detect(const vad::AudioChunk& audio, vad::DetectionResult& result) override {
        if (!initialized_) {
            return vad::ErrorInfo::error(vad::ErrorCode::NOT_INITIALIZED, "Fake backend not initialized");
        }
        if (audio.isEmpty()) {
            return vad::ErrorInfo::error(vad::ErrorCode::INVALID_PARAMETER, "Audio frame is empty");
        }

        const int frame_duration_ms =
            (current_config_.window_size * 1000) / current_config_.sample_rate;
        const int64_t timestamp_ms = frame_count_ * frame_duration_ms;
        const bool voiced = audio.data[0] > 0.0f;

        result.timestamp_ms = timestamp_ms;
        result.probability = voiced ? 0.9f : 0.1f;
        result.smoothed_probability = result.probability;

        if (state_ == vad::VadState::SILENCE && voiced) {
            state_ = vad::VadState::SPEECH_START;
            speech_start_ms_ = timestamp_ms;
            result.speech_start_ms = speech_start_ms_;
        } else if ((state_ == vad::VadState::SPEECH_START || state_ == vad::VadState::SPEECH) && voiced) {
            state_ = vad::VadState::SPEECH;
        } else if ((state_ == vad::VadState::SPEECH_START || state_ == vad::VadState::SPEECH) && !voiced) {
            state_ = vad::VadState::SPEECH_END;
            result.speech_end_ms = timestamp_ms;
            result.speech_duration_ms = static_cast<int>(timestamp_ms - speech_start_ms_);
        } else {
            state_ = vad::VadState::SILENCE;
            speech_start_ms_ = -1;
        }

        result.state = state_;
        result.is_speech = state_ == vad::VadState::SPEECH_START || state_ == vad::VadState::SPEECH;
        frame_count_++;
        return vad::ErrorInfo::ok();
    }

private:
    vad::VadConfig current_config_;
    bool initialized_ = false;
    vad::VadState state_ = vad::VadState::SILENCE;
    int64_t frame_count_ = 0;
    int64_t speech_start_ms_ = -1;
};

void verify_config_contract() {
    const auto presets = SpacemiT::VadConfig::AvailablePresets();
    require(std::find(presets.begin(), presets.end(), "silero") != presets.end(),
            "silero preset must be advertised");
    require(std::find(presets.begin(), presets.end(), "energy") != presets.end(),
            "energy preset must be advertised");

    const auto silero = SpacemiT::VadConfig::Preset("silero");
    require(silero.backend == SpacemiT::VadBackendType::SILERO,
            "silero preset must select SILERO backend");
    require(silero.sample_rate == 16000, "silero preset must keep 16 kHz default sample rate");
    require(silero.window_size == 512, "silero preset must keep 512-sample default window");
    require(!silero.model_dir.empty(), "silero preset must provide a model directory");

    const auto tuned = silero.withTriggerThreshold(0.7f)
                            .withStopThreshold(0.4f)
                            .withSmoothingWindow(3)
                            .withSampleRate(8000)
                            .withWindowSize(256);
    require(silero.trigger_threshold == 0.5f, "builder must not mutate original trigger threshold");
    require(silero.stop_threshold == 0.35f, "builder must not mutate original stop threshold");
    require(tuned.trigger_threshold == 0.7f, "builder must set trigger threshold on returned config");
    require(tuned.stop_threshold == 0.4f, "builder must set stop threshold on returned config");
    require(tuned.smoothing_window == 3, "builder must set smoothing window on returned config");
    require(tuned.sample_rate == 8000, "builder must set sample rate on returned config");
    require(tuned.window_size == 256, "builder must set window size on returned config");
}

void verify_backend_streaming_contract() {
    FakeVadBackend backend;
    vad::VadConfig config;
    config.sample_rate = 16000;
    config.window_size = 512;

    RecordingCallback callback;
    backend.setCallback(&callback);
    require(backend.initialize(config).isOk(), "fake backend initialization must succeed");

    float voiced_frame[1] = {1.0f};
    float silent_frame[1] = {0.0f};
    vad::AudioChunk voiced = vad::AudioChunk::fromFloat(voiced_frame, 1, config.sample_rate);
    vad::AudioChunk silent = vad::AudioChunk::fromFloat(silent_frame, 1, config.sample_rate);

    auto err = backend.feedAudio(voiced);
    require(err.code == vad::ErrorCode::NOT_STARTED, "feed before start must report NOT_STARTED");
    require(callback.result_count == 0, "feed before start must not emit results");

    require(backend.startStream().isOk(), "first startStream must succeed");
    require(backend.isStreamActive(), "backend must report active stream after start");
    err = backend.startStream();
    require(err.code == vad::ErrorCode::ALREADY_STARTED, "second startStream must report ALREADY_STARTED");

    require(backend.feedAudio(voiced).isOk(), "voiced frame must be accepted");
    require(callback.result_count == 1, "voiced frame must emit one result");
    require(callback.speech_start_count == 1, "voiced frame must emit speech start");
    require(callback.last_result.state == vad::VadState::SPEECH_START,
            "first voiced frame must enter SPEECH_START");

    require(backend.feedAudio(voiced).isOk(), "second voiced frame must be accepted");
    require(callback.result_count == 2, "second voiced frame must emit one result");
    require(callback.speech_start_count == 1, "speech start must not repeat while in speech");
    require(callback.last_result.state == vad::VadState::SPEECH,
            "second voiced frame must enter SPEECH");

    require(backend.feedAudio(silent).isOk(), "silent frame after speech must be accepted");
    require(callback.result_count == 3, "silent frame must emit one result");
    require(callback.speech_end_count == 1, "silent frame after speech must emit speech end");
    require(callback.last_result.state == vad::VadState::SPEECH_END,
            "silent frame after speech must enter SPEECH_END");
    require(callback.last_speech_duration_ms > 0, "speech end must report positive duration");

    require(backend.stopStream().isOk(), "first stopStream must succeed");
    require(!backend.isStreamActive(), "backend must report inactive stream after stop");
    err = backend.stopStream();
    require(err.code == vad::ErrorCode::NOT_STARTED, "second stopStream must report NOT_STARTED");
}

void verify_invalid_input_error_path() {
    bool threw = false;
    try {
        (void)SpacemiT::VadConfig::Preset("does-not-exist");
    } catch (const std::invalid_argument& exc) {
        threw = std::string(exc.what()).find("Unknown VAD preset") != std::string::npos;
    }
    require(threw, "unknown preset must throw a useful invalid_argument");

    FakeVadBackend backend;
    vad::VadConfig config;
    RecordingCallback callback;
    backend.setCallback(&callback);
    require(backend.initialize(config).isOk(), "fake backend initialization must succeed");
    require(backend.startStream().isOk(), "stream must start before invalid frame test");

    const vad::AudioChunk empty = vad::AudioChunk::fromFloat(nullptr, 0, config.sample_rate);
    const auto err = backend.feedAudio(empty);
    require(err.code == vad::ErrorCode::INVALID_PARAMETER,
            "empty audio frame must report INVALID_PARAMETER");
    require(callback.result_count == 0, "invalid frame must not emit successful result");
    require(callback.speech_start_count == 0, "invalid frame must not emit speech start");
    require(callback.speech_end_count == 0, "invalid frame must not emit speech end");
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected one test mode argument");
    const std::string mode = argv[1];

    if (mode == "--config-and-backend-contract") {
        verify_config_contract();
        verify_backend_streaming_contract();
    } else if (mode == "--invalid-input-error-path") {
        verify_invalid_input_error_path();
    } else {
        std::cerr << "Unknown mode: " << mode << std::endl;
        return 2;
    }

    std::cout << "PASS " << mode << std::endl;
    return 0;
}
