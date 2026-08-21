#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace obs_sync_replay {

class ThreeStreamCapturePoc final {
public:
    ThreeStreamCapturePoc(std::string scene_a_name, std::string scene_b_name);
    ~ThreeStreamCapturePoc();

    ThreeStreamCapturePoc(const ThreeStreamCapturePoc &) = delete;
    ThreeStreamCapturePoc &operator=(const ThreeStreamCapturePoc &) = delete;

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
