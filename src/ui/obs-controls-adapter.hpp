#pragma once

#include <QtCore/QPointer>
#include <QtCore/Qt>

#include <vector>

class QDockWidget;
class QBoxLayout;
class QLayout;
class QPushButton;
class QWidget;

namespace obs_sync_replay {

class CaptureControls;

class ObsControlsAdapter final {
  public:
    explicit ObsControlsAdapter(QWidget* frontend_window);
    ~ObsControlsAdapter();

    ObsControlsAdapter(const ObsControlsAdapter&) = delete;
    ObsControlsAdapter& operator=(const ObsControlsAdapter&) = delete;

    bool Locate();
    bool Install(CaptureControls& controls);
    void Restore() noexcept;

    QWidget* controls_parent() const noexcept;
    bool installed() const noexcept;

  private:
    struct Replacement final {
        QPointer<QBoxLayout> layout;
        QPointer<QPushButton> native;
        QPointer<QPushButton> replacement;
        int index = -1;
        Qt::Alignment alignment{};
        bool visible = false;
        bool enabled = false;
    };

    bool Replace(QPushButton* native, QPushButton* replacement);
    static QBoxLayout* FindContainingLayout(QLayout* root, QWidget* target);
    static void CopyNativeAppearance(QPushButton* native, QPushButton* replacement);
    void Fail(const char* reason) const;

    QWidget* frontend_window_ = nullptr;
    QPointer<QDockWidget> controls_dock_;
    QPointer<QWidget> controls_parent_;
    QPointer<QPushButton> native_recording_button_;
    QPointer<QPushButton> native_replay_button_;
    QPointer<QPushButton> native_save_replay_button_;
    std::vector<Replacement> replacements_;
    bool located_ = false;
    bool installed_ = false;
};

} // namespace obs_sync_replay
