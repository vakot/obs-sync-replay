#pragma once

#include "ui/plugin-owned-help-indicator.hpp"

#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtWidgets/QPushButton>

class QEvent;
class QEnterEvent;
class QMouseEvent;
class QPaintEvent;

namespace obs_sync_replay {

class PluginOwnedHelpButton final : public QPushButton {
  public:
    explicit PluginOwnedHelpButton(QWidget* parent = nullptr);

    void SetPluginOwnedHelpTooltip(const QString& tooltip);
    QString plugin_owned_help_tooltip() const;
    QRect plugin_owned_help_indicator_rect() const;
    QRect plugin_owned_help_hit_rect() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void ShowHelpTooltip(const QPoint& position);
    PluginOwnedHelpIndicatorGeometry Geometry() const;
    QRect IndicatorRect() const;
    QRect HitRect() const;

    QString help_tooltip_;
    bool help_press_active_ = false;
};

} // namespace obs_sync_replay
