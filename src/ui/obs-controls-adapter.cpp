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

#include <algorithm>

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
    native_recording_button_ = FindNativeButtons(QStringLiteral("recordButton")).value(0, nullptr);
    native_replay_button_ = FindNativeButtons(QStringLiteral("replayBufferButton")).value(0, nullptr);
    native_save_replay_button_ = FindNativeButtons(QStringLiteral("saveReplayButton")).value(0, nullptr);
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

bool ObsControlsAdapter::Reconcile(CaptureControls& controls) {
    if (!installed_) {
        return false;
    }
    if (!controls_parent_ || !controls.recording_button() || !controls.replay_button() ||
        !controls.save_replay_button()) {
        Fail("native-control-reconciliation-arguments");
        return false;
    }

    // OBS may reinsert its named controls when replay configuration is applied. Keep
    // the plugin controls in the recorded slots and detach every stock control with
    // the exact native object name; arbitrary dock children are never touched.
    ReconcileNativeControl(QStringLiteral("recordButton"), native_recording_button_, controls.recording_button());
    ReconcileNativeControl(QStringLiteral("replayBufferButton"), native_replay_button_, controls.replay_button());
    ReconcileNativeControl(QStringLiteral("saveReplayButton"), native_save_replay_button_, controls.save_replay_button());
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
    for (auto iterator = extra_native_controls_.rbegin(); iterator != extra_native_controls_.rend(); ++iterator) {
        NativeRestoration& restoration = *iterator;
        if (restoration.layout && restoration.native && restoration.layout->indexOf(restoration.native) < 0) {
            restoration.layout->insertWidget(restoration.index, restoration.native, restoration.alignment);
            restoration.native->setVisible(restoration.visible);
            restoration.native->setEnabled(restoration.enabled);
        }
    }
    replacements_.clear();
    extra_native_controls_.clear();
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

bool ObsControlsAdapter::ReconcileNativeControl(const QString& object_name, QPushButton* primary_native,
                                                QPushButton* replacement) {
    bool reconciled = false;
    for (QPushButton* native : FindNativeButtons(object_name)) {
        if (!native || native == replacement) {
            continue;
        }

        QBoxLayout* layout = FindContainingLayout(controls_parent_->layout(), native);
        if (native == primary_native || IsTrackedExtraNative(native)) {
            if (layout) {
                layout->removeWidget(native);
            }
            native->setVisible(false);
            native->setEnabled(false);
            if (native == primary_native) {
                CopyNativeAppearance(native, replacement);
            }
            reconciled = true;
            continue;
        }

        if (!layout) {
            native->setVisible(false);
            native->setEnabled(false);
            reconciled = true;
            continue;
        }

        const int index = layout->indexOf(native);
        if (index < 0) {
            continue;
        }
        QLayoutItem* item = layout->itemAt(index);
        NativeRestoration restoration;
        restoration.layout = layout;
        restoration.native = native;
        restoration.index = index;
        restoration.alignment = item ? item->alignment() : Qt::Alignment{};
        restoration.visible = native->isVisible();
        restoration.enabled = native->isEnabled();
        layout->removeWidget(native);
        native->setVisible(false);
        native->setEnabled(false);
        extra_native_controls_.push_back(restoration);
        blog(LOG_WARNING, "[plugin-ui] native control reconciled object=%s action=detached-reintroduced-widget",
             object_name.toUtf8().constData());
        reconciled = true;
    }
    return reconciled;
}

QList<QPushButton*> ObsControlsAdapter::FindNativeButtons(const QString& object_name) const {
    QList<QPushButton*> buttons;
    if (!controls_parent_) {
        return buttons;
    }
    const QList<QPushButton*> candidates =
        controls_parent_->findChildren<QPushButton*>(object_name, Qt::FindChildrenRecursively);
    for (QPushButton* candidate : candidates) {
        if (candidate && !candidate->property("obsSyncReplayPluginControl").toBool()) {
            buttons.push_back(candidate);
        }
    }
    std::stable_sort(buttons.begin(), buttons.end(), [this](QPushButton* left, QPushButton* right) {
        const bool left_in_layout = FindContainingLayout(controls_parent_->layout(), left) != nullptr;
        const bool right_in_layout = FindContainingLayout(controls_parent_->layout(), right) != nullptr;
        return left_in_layout && !right_in_layout;
    });
    return buttons;
}

bool ObsControlsAdapter::IsTrackedExtraNative(QPushButton* native) const {
    return std::any_of(extra_native_controls_.begin(), extra_native_controls_.end(),
                       [native](const NativeRestoration& restoration) { return restoration.native == native; });
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
    replacement->setObjectName(native->objectName());
    replacement->setProperty("obsSyncReplayPluginControl", true);
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
