#include "ui/plugin-owned-help-button.hpp"

#include "ui/plugin-owned-help-indicator.hpp"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolTip>

#include <algorithm>

namespace obs_sync_replay {

namespace {

QRect ToQRect(const PluginOwnedHelpIndicatorGeometry& geometry, const bool hit_rect) {
    return hit_rect ? QRect(geometry.hit_x, geometry.hit_y, geometry.hit_width, geometry.hit_height)
                    : QRect(geometry.indicator_x, geometry.indicator_y, geometry.indicator_width,
                            geometry.indicator_height);
}

} // namespace

PluginOwnedHelpButton::PluginOwnedHelpButton(QWidget* parent) : QPushButton(parent) {
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
}

void PluginOwnedHelpButton::SetPluginOwnedHelpTooltip(const QString& tooltip) {
    help_tooltip_ = tooltip;
    setAccessibleDescription(help_tooltip_);
    if (help_tooltip_.isEmpty()) {
        QToolTip::hideText();
    }
    update();
}

QString PluginOwnedHelpButton::plugin_owned_help_tooltip() const {
    return help_tooltip_;
}

QRect PluginOwnedHelpButton::plugin_owned_help_indicator_rect() const {
    return IndicatorRect();
}

QRect PluginOwnedHelpButton::plugin_owned_help_hit_rect() const {
    return HitRect();
}

void PluginOwnedHelpButton::paintEvent(QPaintEvent* event) {
    QPushButton::paintEvent(event);
    if (help_tooltip_.isEmpty()) {
        return;
    }

    const QRect indicator = IndicatorRect();
    if (indicator.isEmpty()) {
        return;
    }

    QPainter painter(this);
    const QIcon help_icon = style()->standardIcon(QStyle::SP_MessageBoxQuestion);
    if (!help_icon.isNull()) {
        const QIcon::Mode mode = isEnabled() ? (underMouse() ? QIcon::Active : QIcon::Normal) : QIcon::Disabled;
        help_icon.paint(&painter, indicator, Qt::AlignCenter, mode, isDown() ? QIcon::On : QIcon::Off);
        return;
    }

    const QPalette::ColorGroup group = isEnabled() ? QPalette::Active : QPalette::Disabled;
    painter.setPen(palette().color(group, QPalette::ButtonText));
    QFont font = painter.font();
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(indicator, Qt::AlignCenter, QStringLiteral("?"));
}

void PluginOwnedHelpButton::enterEvent(QEnterEvent* event) {
    QPushButton::enterEvent(event);
    update();
}

void PluginOwnedHelpButton::leaveEvent(QEvent* event) {
    QToolTip::hideText();
    help_press_active_ = false;
    QPushButton::leaveEvent(event);
    update();
}

void PluginOwnedHelpButton::mouseMoveEvent(QMouseEvent* event) {
    QPushButton::mouseMoveEvent(event);
    if (HitRect().contains(event->position().toPoint())) {
        ShowHelpTooltip(event->position().toPoint());
    } else {
        QToolTip::hideText();
    }
    update();
}

void PluginOwnedHelpButton::mousePressEvent(QMouseEvent* event) {
    if (!help_tooltip_.isEmpty() && event->button() == Qt::LeftButton &&
        HitRect().contains(event->position().toPoint())) {
        help_press_active_ = true;
        event->accept();
        return;
    }
    QPushButton::mousePressEvent(event);
}

void PluginOwnedHelpButton::mouseReleaseEvent(QMouseEvent* event) {
    if (help_press_active_) {
        help_press_active_ = false;
        event->accept();
        return;
    }
    QPushButton::mouseReleaseEvent(event);
}

void PluginOwnedHelpButton::ShowHelpTooltip(const QPoint& position) {
    if (help_tooltip_.isEmpty()) {
        return;
    }
    QToolTip::showText(mapToGlobal(position), help_tooltip_, this, HitRect());
}

QRect PluginOwnedHelpButton::IndicatorRect() const {
    const int side = std::max(1, style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this));
    const int margin = std::max(1, style()->pixelMetric(QStyle::PM_ButtonMargin, nullptr, this));
    return ToQRect(MakePluginOwnedHelpIndicatorGeometry(width(), height(), side, side, margin, margin / 2), false);
}

QRect PluginOwnedHelpButton::HitRect() const {
    const int side = std::max(1, style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this));
    const int margin = std::max(1, style()->pixelMetric(QStyle::PM_ButtonMargin, nullptr, this));
    return ToQRect(MakePluginOwnedHelpIndicatorGeometry(width(), height(), side, side, margin, margin / 2), true);
}

} // namespace obs_sync_replay
