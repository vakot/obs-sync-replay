#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace obs_sync_replay {

class StockEncoderTimelineProbe final {
public:
    StockEncoderTimelineProbe(std::string scene_a_name, std::string scene_b_name);
    ~StockEncoderTimelineProbe();

    StockEncoderTimelineProbe(const StockEncoderTimelineProbe&) = delete;
    StockEncoderTimelineProbe& operator=(const StockEncoderTimelineProbe&) = delete;

    bool Start();
    void Stop();

private:
    struct State;

    void Run();

    std::string scene_a_name_;
    std::string scene_b_name_;
    std::unique_ptr<State> state_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
};

} // namespace obs_sync_replay
