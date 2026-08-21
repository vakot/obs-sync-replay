#include "ui/plugin-control-dock.hpp"

#include "control/plugin-capture-runtime.hpp"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtCore/QTimer>
#include <QtWidgets/QVBoxLayout>

#include <obs-module.h>

#include <string>

namespace obs_sync_replay {

namespace {

const char* RecordingStateText(const RecordingConsumerState state) {
    switch (state) {
    case RecordingConsumerState::Off:
        return "Inactive";
    case RecordingConsumerState::Starting:
        return "Starting";
    case RecordingConsumerState::Running:
        return "Running";
    case RecordingConsumerState::Stopping:
        return "Stopping";
    }
    return "Unknown";
}

const char* ReplayStateText(const ReplayConsumerState state) {
    switch (state) {
    case ReplayConsumerState::Off:
        return "Inactive";
    case ReplayConsumerState::Running:
        return "Running";
    case ReplayConsumerState::Saving:
        return "Saving";
    case ReplayConsumerState::Stopping:
        return "Stopping";
    }
    return "Unknown";
}

} // namespace

PluginControlDock::PluginControlDock(PluginCaptureRuntime& runtime, QWidget* parent)
    : QWidget(parent), runtime_(runtime) {
    setObjectName(QStringLiteral("obsSyncReplayControlDock"));
    setMinimumWidth(260);

    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(QStringLiteral("Synchronized Capture"), this);
    title->setStyleSheet(QStringLiteral("font-weight: bold;"));
    layout->addWidget(title);

    auto* recording_row = new QHBoxLayout();
    recording_state_ = new QLabel(this);
    recording_button_ = new QPushButton(this);
    recording_row->addWidget(recording_state_);
    recording_row->addWidget(recording_button_);
    layout->addLayout(recording_row);

    auto* replay_row = new QHBoxLayout();
    replay_state_ = new QLabel(this);
    replay_button_ = new QPushButton(this);
    replay_row->addWidget(replay_state_);
    replay_row->addWidget(replay_button_);
    layout->addLayout(replay_row);

    save_button_ = new QPushButton(QStringLiteral("Save Replay"), this);
    layout->addWidget(save_button_);
    status_ = new QLabel(QStringLiteral("Ready; capture is off"), this);
    status_->setWordWrap(true);
    layout->addWidget(status_);

    connect(recording_button_, &QPushButton::clicked, this, [this] { InvokeRecording(); });
    connect(replay_button_, &QPushButton::clicked, this, [this] { InvokeReplayToggle(); });
    connect(save_button_, &QPushButton::clicked, this, [this] { InvokeReplaySave(); });

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(250);
    connect(refresh_timer_, &QTimer::timeout, this, [this] {
        runtime_.PollReplaySave();
        Refresh();
    });
    refresh_timer_->start();
    Refresh();
}

PluginControlDock::~PluginControlDock() = default;

void PluginControlDock::DisableControls() {
    disabled_ = true;
    if (refresh_timer_) {
        refresh_timer_->stop();
    }
    recording_button_->setEnabled(false);
    replay_button_->setEnabled(false);
    save_button_->setEnabled(false);
    status_->setText(QStringLiteral("Plugin shutting down"));
}

void PluginControlDock::Refresh() {
    if (disabled_) {
        return;
    }

    const RecordingConsumerState recording = runtime_.recording_state();
    const ReplayConsumerState replay = runtime_.replay_state();
    const bool infrastructure_failed = runtime_.capture_state() == CaptureInfrastructureState::Failed;
    recording_state_->setText(QStringLiteral("Recording: ") + QString::fromUtf8(
        recording_failed_ || infrastructure_failed ? "Failed" : RecordingStateText(recording)));
    replay_state_->setText(QStringLiteral("Replay: ") + QString::fromUtf8(
        replay_failed_ || infrastructure_failed ? "Failed" : ReplayStateText(replay)));

    const bool recording_transition = recording == RecordingConsumerState::Starting ||
                                      recording == RecordingConsumerState::Stopping;
    recording_button_->setText(recording == RecordingConsumerState::Running ? QStringLiteral("Stop Recording")
                                                                              : QStringLiteral("Start Recording"));
    recording_button_->setEnabled(!recording_transition &&
                                  (recording == RecordingConsumerState::Off ||
                                   recording == RecordingConsumerState::Running));

    const bool replay_transition = replay == ReplayConsumerState::Saving || replay == ReplayConsumerState::Stopping;
    replay_button_->setText(replay == ReplayConsumerState::Running ? QStringLiteral("Stop Replay Buffer")
                                                                     : replay_transition ? QStringLiteral("Saving...")
                                                                                          : QStringLiteral("Start Replay Buffer"));
    replay_button_->setEnabled(!replay_transition &&
                               (replay == ReplayConsumerState::Off || replay == ReplayConsumerState::Running));
    save_button_->setEnabled(replay == ReplayConsumerState::Running);
}

void PluginControlDock::InvokeRecording() {
    const ControlCommandResult result = runtime_.ToggleRecording();
    Report("recording-toggle", result);
}

void PluginControlDock::InvokeReplayToggle() {
    const ControlCommandResult result = runtime_.ToggleReplay();
    Report("replay-toggle", result);
}

void PluginControlDock::InvokeReplaySave() {
    const ControlCommandResult result = runtime_.SaveReplay();
    Report("replay-save", result);
}

void PluginControlDock::Report(const char* action, const ControlCommandResult& result) {
    const bool failed = result.status == ControlCommandStatus::Failed ||
                        result.status == ControlCommandStatus::InvalidState;
    if (std::string(action) == "recording-toggle") {
        recording_failed_ = failed;
    }
    if (std::string(action) == "replay-toggle") {
        replay_failed_ = failed;
    }
    status_->setText(QString::fromUtf8(action) + QStringLiteral(": ") + QString::fromUtf8(result.reason.c_str()));
    blog(failed ? LOG_ERROR : LOG_INFO, "[plugin-ui] action=%s status=%s reason=%s", action,
         ControlCommandStatusName(result.status), result.reason.c_str());
    Refresh();
}

} // namespace obs_sync_replay
