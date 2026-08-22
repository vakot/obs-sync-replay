#include "ui/obs-controls-adapter.hpp"

#include "ui/capture-controls.hpp"

#include <QtWidgets/QDockWidget>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStyle>
#include <QtWidgets/QWidget>

#include <obs-module.h>

namespace obs_sync_replay {

ObsControlsAdapter::ObsControlsAdapter(QWidget* frontend_window) : frontend_window_(frontend_window) {}

ObsControlsAdapter::~ObsControlsAdapter() {
    Restore();
}

bool ObsControlsAdapter::Locate() {
    if (located_) {
        return controls_dock_ && controls_parent_ && native_recording_button_ && native_replay_button_ &&
               native_save_replay_button_;
    }
    located_ = true;
    if (!frontend_window_) {
        Fail("frontend-main-window-missing");
        return false;
    }

    controls_dock_ = frontend_window_->findChild<QDockWidget*>(QStringLiteral("controlsDock"));
    if (!controls_dock_ || !controls_dock_->widget()) {
        Fail("controls-dock-not-found");
        return false;
    }
    controls_parent_ = controls_dock_->widget();
    native_recording_button_ = controls_parent_->findChild<QPushButton*>(QStringLiteral("recordButton"));
    native_replay_button_ = controls_parent_->findChild<QPushButton*>(QStringLiteral("replayBufferButton"));
    native_save_replay_button_ = controls_parent_->findChild<QPushButton*>(QStringLiteral("saveReplayButton"));
    if (!native_recording_button_ || !native_replay_button_ || !native_save_replay_button_) {
        Fail("native-recording-or-replay-control-not-found");
        return false;
    }
    return true;
}

bool ObsControlsAdapter::Install(CaptureControls& controls) {
    if (installed_) {
        return true;
    }
    if (!Locate()) {
        return false;
    }
    if (!Replace(native_recording_button_, controls.recording_button()) ||
        !Replace(native_replay_button_, controls.replay_button()) ||
        !Replace(native_save_replay_button_, controls.save_replay_button())) {
        Restore();
        Fail("native-control-layout-replacement-failed");
        return false;
    }
    CopyNativeAppearance(native_recording_button_, controls.recording_button());
    CopyNativeAppearance(native_replay_button_, controls.replay_button());
    CopyNativeAppearance(native_save_replay_button_, controls.save_replay_button());
    installed_ = true;
    blog(LOG_INFO, "[plugin-ui] native controls replaced dock=controlsDock record=recordButton replay=replayBufferButton "
                   "save=saveReplayButton");
    return true;
}

void ObsControlsAdapter::Restore() noexcept {
    const size_t restored_count = replacements_.size();
    for (auto iterator = replacements_.rbegin(); iterator != replacements_.rend(); ++iterator) {
        Replacement& replacement = *iterator;
        if (replacement.layout && replacement.replacement) {
            replacement.layout->removeWidget(replacement.replacement);
        }
        if (replacement.layout && replacement.native) {
            replacement.layout->insertWidget(replacement.index, replacement.native, replacement.alignment);
            replacement.native->setVisible(replacement.visible);
            replacement.native->setEnabled(replacement.enabled);
        }
    }
    replacements_.clear();
    installed_ = false;
    if (restored_count > 0) {
        blog(LOG_INFO, "[plugin-ui] native controls restored count=%zu", restored_count);
    }
}

QWidget* ObsControlsAdapter::controls_parent() const noexcept {
    return controls_parent_;
}

bool ObsControlsAdapter::installed() const noexcept {
    return installed_;
}

bool ObsControlsAdapter::Replace(QPushButton* native, QPushButton* replacement) {
    if (!native || !replacement || !controls_parent_) {
        blog(LOG_WARNING, "[plugin-ui] replace failed stage=arguments");
        return false;
    }
    QBoxLayout* layout = FindContainingLayout(controls_parent_->layout(), native);
    if (!layout) {
        blog(LOG_WARNING, "[plugin-ui] replace failed stage=containing-layout native=%s parent=%s",
             native->objectName().toUtf8().constData(), native->parentWidget() ? native->parentWidget()->objectName().toUtf8().constData() : "none");
        return false;
    }
    const int index = layout->indexOf(native);
    if (index < 0) {
        blog(LOG_WARNING, "[plugin-ui] replace failed stage=layout-index native=%s layout=%s",
             native->objectName().toUtf8().constData(), layout->objectName().toUtf8().constData());
        return false;
    }
    QLayoutItem* item = layout->itemAt(index);
    if (!item) {
        blog(LOG_WARNING, "[plugin-ui] replace failed stage=layout-item native=%s layout=%s index=%d",
             native->objectName().toUtf8().constData(), layout->objectName().toUtf8().constData(), index);
        return false;
    }
    Replacement record;
    record.layout = layout;
    record.native = native;
    record.replacement = replacement;
    record.index = index;
    record.alignment = item->alignment();
    record.visible = native->isVisible();
    record.enabled = native->isEnabled();

    native->setVisible(false);
    native->setEnabled(false);
    layout->removeWidget(native);
    layout->insertWidget(index, replacement, record.alignment);
    replacement->setVisible(true);
    replacements_.push_back(record);
    return true;
}

QBoxLayout* ObsControlsAdapter::FindContainingLayout(QLayout* root, QWidget* target) {
    if (!root || !target) {
        return nullptr;
    }
    for (int index = 0; index < root->count(); ++index) {
        QLayoutItem* item = root->itemAt(index);
        if (!item) {
            continue;
        }
        if (item->widget() == target) {
            return qobject_cast<QBoxLayout*>(root);
        }
        if (QWidget* child_widget = item->widget()) {
            if (QBoxLayout* result = FindContainingLayout(child_widget->layout(), target)) {
                return result;
            }
        }
        if (QLayout* child = item->layout()) {
            if (QBoxLayout* result = FindContainingLayout(child, target)) {
                return result;
            }
        }
    }
    return nullptr;
}

void ObsControlsAdapter::CopyNativeAppearance(QPushButton* native, QPushButton* replacement) {
    if (!native || !replacement) {
        return;
    }
    replacement->setFont(native->font());
    replacement->setIcon(native->icon());
    replacement->setIconSize(native->iconSize());
    replacement->setSizePolicy(native->sizePolicy());
    replacement->setMinimumSize(native->minimumSize());
    replacement->setMaximumSize(native->maximumSize());
    replacement->setContentsMargins(native->contentsMargins());
    replacement->setToolTip(native->toolTip());
    replacement->setAccessibleName(native->accessibleName());
    replacement->setStyleSheet(native->styleSheet());
    replacement->setProperty("obsSyncReplayBaseClass", native->property("class"));
    if (native->objectName() == QStringLiteral("saveReplayButton")) {
        const int native_height = native->sizeHint().height();
        if (native_height > 0) {
            replacement->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            replacement->setFixedSize(native_height, native_height);
        }
        replacement->setText(QString());
    }
    replacement->style()->unpolish(replacement);
    replacement->style()->polish(replacement);
}

void ObsControlsAdapter::Fail(const char* reason) const {
    blog(LOG_WARNING, "[plugin-ui] native-control-replacement-unavailable reason=%s", reason);
}

} // namespace obs_sync_replay
