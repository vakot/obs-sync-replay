#include "ui/capture-controls.hpp"
#include "plugin/plugin-log.hpp"

#include "control/plugin-capture-runtime.hpp"
#include "ui/capture-controls-state.hpp"
#include "ui/obs-controls-adapter.hpp"
#include "ui/plugin-owned-help-button.hpp"

#include <QtCore/QTimer>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QWidget>

#include <exception>

#include <obs-module.h>

namespace obs_sync_replay {

namespace {

void SetButtonClasses(QPushButton* button, const CaptureControlVisualState state) {
    if (!button) {
        return;
    }
    QString classes = button->property("obsSyncReplayBaseClass").toString();
    const QString state_class = CaptureControlUsesNativeActiveStyle(state)
                                    ? QStringLiteral("state-active")
                                    : state == CaptureControlVisualState::Failed ? QStringLiteral("text-danger") : QString();
    if (!state_class.isEmpty()) {
        if (!classes.isEmpty()) {
            classes += QLatin1Char(' ');
        }
        classes += state_class;
    }
    button->setProperty("class", classes);
    // OBS's setClasses helper forces the stylesheet engine to recalculate
    // selectors such as QPushButton.state-active after changing the class property.
    const QString stylesheet = button->styleSheet();
    button->setStyleSheet(QStringLiteral("/* */"));
    button->setStyleSheet(stylesheet);
}

} // namespace

CaptureControls::CaptureControls(PluginCaptureRuntime& runtime, QWidget* parent, ObsControlsAdapter* controls_adapter)
    : QObject(parent), runtime_(runtime), controls_adapter_(controls_adapter) {
    recording_button_ = new PluginOwnedHelpButton(parent);
    recording_button_->setObjectName(QStringLiteral("obsSyncReplayRecordingButton"));
    recording_button_->setProperty("obsSyncReplayPluginControl", true);
    replay_button_ = new PluginOwnedHelpButton(parent);
    replay_button_->setObjectName(QStringLiteral("obsSyncReplayReplayButton"));
    replay_button_->setProperty("obsSyncReplayPluginControl", true);
    save_replay_button_ = new QPushButton(parent);
    save_replay_button_->setObjectName(QStringLiteral("obsSyncReplaySaveButton"));
    save_replay_button_->setProperty("obsSyncReplayPluginControl", true);
    save_replay_button_->setVisible(false);

    status_ = new QLabel(parent);
    status_->setObjectName(QStringLiteral("obsSyncReplayStatus"));
    status_->setVisible(false);

    QObject::connect(recording_button_, &QPushButton::clicked, this, [this] { InvokeRecording(); });
    QObject::connect(replay_button_, &QPushButton::clicked, this, [this] { InvokeReplayToggle(); });
    QObject::connect(save_replay_button_, &QPushButton::clicked, this, [this] { InvokeReplaySave(); });

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(250);
    QObject::connect(refresh_timer_, &QTimer::timeout, this, [this] {
        (void)runtime_.RefreshSceneTopology();
        (void)runtime_.RefreshReplayConfiguration();
        if (controls_adapter_) {
            (void)controls_adapter_->Reconcile(*this);
        }
        runtime_.PollReplaySave();
        ApplyPresentationSafely();
    });
    refresh_timer_->start();
    QTimer::singleShot(0, this, [this] { ApplyPresentationSafely(); });
}

CaptureControls::~CaptureControls() {
    if (refresh_timer_) {
        refresh_timer_->stop();
    }
    delete recording_button_;
    delete replay_button_;
    delete save_replay_button_;
    delete status_;
}

QPushButton* CaptureControls::recording_button() const noexcept {
    return recording_button_;
}

QPushButton* CaptureControls::replay_button() const noexcept {
    return replay_button_;
}

QPushButton* CaptureControls::save_replay_button() const noexcept {
    return save_replay_button_;
}

void CaptureControls::RefreshNow() {
    ApplyPresentationSafely();
}

void CaptureControls::DisableControls() {
    disabled_ = true;
    if (refresh_timer_) {
        refresh_timer_->stop();
    }
    recording_button_->setEnabled(false);
    replay_button_->setEnabled(false);
    save_replay_button_->setEnabled(false);
    save_replay_button_->setVisible(false);
}

void CaptureControls::InvokeRecording() {
    Report("recording-toggle", runtime_.ToggleRecording());
}

void CaptureControls::InvokeReplayToggle() {
    Report("replay-toggle", runtime_.ToggleReplay());
}

void CaptureControls::InvokeReplaySave() {
    Report("replay-save", runtime_.SaveReplay());
}

void CaptureControls::Report(const char* action, const ControlCommandResult& result) {
    const bool failed = result.status == ControlCommandStatus::Failed ||
                        result.status == ControlCommandStatus::InvalidState;
    if (QString::fromUtf8(action) == QStringLiteral("recording-toggle")) {
        recording_failed_ = failed;
    }
    if (QString::fromUtf8(action) == QStringLiteral("replay-toggle")) {
        replay_failed_ = failed;
    }
    OBS_SYNC_REPLAY_LOG(failed ? LOG_ERROR : LOG_INFO, "ui", "action=%s status=%s reason=%s", action,
                        ControlCommandStatusName(result.status), result.reason.c_str());
    ApplyPresentationSafely();
}

void CaptureControls::ApplyPresentationSafely() {
    try {
        ApplyPresentation();
    } catch (const std::exception& error) {
        disabled_ = true;
        refresh_timer_->stop();
        recording_button_->setEnabled(false);
        replay_button_->setEnabled(false);
        save_replay_button_->setEnabled(false);
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "ui", "presentation update failed with exception=%s", error.what());
    } catch (...) {
        disabled_ = true;
        refresh_timer_->stop();
        recording_button_->setEnabled(false);
        replay_button_->setEnabled(false);
        save_replay_button_->setEnabled(false);
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "ui", "presentation update failed with unknown exception");
    }
}

void CaptureControls::ApplyPresentation() {
    if (disabled_) {
        return;
    }

    const CaptureInfrastructureState capture_state = runtime_.capture_state();
    const RecordingConsumerState recording_state = runtime_.recording_state();
    const ReplayConsumerState replay_state = runtime_.replay_state();
    const bool replay_available = runtime_.replay_available();
    const CaptureControlsLabels labels = ResolveCaptureControlsLabels();
    const CaptureControlsPresentation presentation = MakeCaptureControlsPresentation(
        capture_state, recording_state, replay_state, replay_available, recording_failed_, replay_failed_, labels);
    const auto to_qstring = [](const std::string& value) {
        return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    };
    recording_button_->setText(to_qstring(presentation.recording.text));
    recording_button_->SetPluginOwnedHelpTooltip(to_qstring(presentation.recording.plugin_owned_help_tooltip));
    recording_button_->setEnabled(presentation.recording.enabled);
    recording_button_->setVisible(presentation.recording.visible);
    SetButtonClasses(recording_button_, presentation.recording.state);

    replay_button_->setText(to_qstring(presentation.replay.text));
    replay_button_->SetPluginOwnedHelpTooltip(to_qstring(presentation.replay.plugin_owned_help_tooltip));
    replay_button_->setEnabled(presentation.replay.enabled);
    replay_button_->setVisible(presentation.replay.visible);
    SetButtonClasses(replay_button_, presentation.replay.state);

    save_replay_button_->setEnabled(presentation.save_replay_enabled);
    save_replay_button_->setVisible(presentation.save_replay_visible);
    save_replay_button_->setText(QString());
    save_replay_button_->setToolTip(to_qstring(presentation.save_replay_text));
    save_replay_button_->setAccessibleName(to_qstring(presentation.save_replay_text));
    SetButtonClasses(save_replay_button_, CaptureControlVisualState::Inactive);
}

} // namespace obs_sync_replay
