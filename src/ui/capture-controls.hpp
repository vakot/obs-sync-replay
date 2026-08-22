#pragma once

#include "control/capture-control.hpp"

#include <QtCore/QObject>

class QLabel;
class QPushButton;
class QTimer;
class QWidget;

namespace obs_sync_replay {

class PluginCaptureRuntime;

class CaptureControls final : public QObject {
  public:
    explicit CaptureControls(PluginCaptureRuntime& runtime, QWidget* parent);
    ~CaptureControls() override;

    CaptureControls(const CaptureControls&) = delete;
    CaptureControls& operator=(const CaptureControls&) = delete;

    QPushButton* recording_button() const noexcept;
    QPushButton* replay_button() const noexcept;
    QPushButton* save_replay_button() const noexcept;

    void RefreshNow();
    void DisableControls();

  private:
    void InvokeRecording();
    void InvokeReplayToggle();
    void InvokeReplaySave();
    void Report(const char* action, const ControlCommandResult& result);
    void ApplyPresentation();
    void ApplyPresentationSafely();

    PluginCaptureRuntime& runtime_;
    QPushButton* recording_button_ = nullptr;
    QPushButton* replay_button_ = nullptr;
    QPushButton* save_replay_button_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    bool disabled_ = false;
    bool recording_failed_ = false;
    bool replay_failed_ = false;
};

} // namespace obs_sync_replay
