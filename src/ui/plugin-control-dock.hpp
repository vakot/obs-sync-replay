#pragma once

#include "control/capture-control.hpp"

#include <QtWidgets/QWidget>

class QLabel;
class QPushButton;
class QTimer;

namespace obs_sync_replay {

class PluginCaptureRuntime;

class PluginControlDock final : public QWidget {
  public:
    explicit PluginControlDock(PluginCaptureRuntime& runtime, QWidget* parent = nullptr);
    ~PluginControlDock() override;

    PluginControlDock(const PluginControlDock&) = delete;
    PluginControlDock& operator=(const PluginControlDock&) = delete;

    void DisableControls();

  private:
    void Refresh();
    void InvokeRecording();
    void InvokeReplayToggle();
    void InvokeReplaySave();
    void Report(const char* action, const ControlCommandResult& result);

    PluginCaptureRuntime& runtime_;
    QLabel* recording_state_ = nullptr;
    QLabel* replay_state_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* recording_button_ = nullptr;
    QPushButton* replay_button_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    bool disabled_ = false;
    bool recording_failed_ = false;
    bool replay_failed_ = false;
};

} // namespace obs_sync_replay
