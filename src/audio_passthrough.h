#pragma once

#include <atomic>
#include <thread>

class AudioPassthrough {
public:
    AudioPassthrough() = default;
    ~AudioPassthrough();

    AudioPassthrough(const AudioPassthrough&) = delete;
    AudioPassthrough& operator=(const AudioPassthrough&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool running() const noexcept;

private:
    void audio_loop(std::stop_token stop_token);

    std::jthread thread_;
    std::atomic<bool> running_{false};
};
