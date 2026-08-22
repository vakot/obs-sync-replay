#include "ui/plugin-owned-help-button.hpp"

#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyle>

#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace obs_sync_replay;

namespace {

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void Click(QAbstractButton& button, const QPoint& position) {
    const QPointF local_position(position);
    const QPointF global_position(button.mapToGlobal(position));
    QMouseEvent press(QEvent::MouseButtonPress, local_position, global_position, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, local_position, global_position, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&button, &press);
    QApplication::sendEvent(&button, &release);
}

void TestHelpAreaDoesNotInvokeMainAction() {
    PluginOwnedHelpButton button;
    button.resize(240, 40);
    button.SetPluginOwnedHelpTooltip(QStringLiteral("Localized ownership help"));
    int clicked = 0;
    QObject::connect(&button, &QAbstractButton::clicked, [&clicked] { ++clicked; });

    Click(button, QPoint(24, 20));
    Require(clicked == 1, "clicking the main button area must preserve toggle semantics");

    const QRect hit_rect = button.plugin_owned_help_hit_rect();
    const int side = std::max(1, button.style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, &button));
    const int expected_right_margin = std::max(
        {(button.height() - side) / 2, button.style()->pixelMetric(QStyle::PM_ButtonMargin, nullptr, &button),
         button.contentsMargins().right()});
    const QRect indicator = button.plugin_owned_help_indicator_rect();
    Require(button.width() - indicator.right() - 1 == expected_right_margin,
            "help indicator must use square-button padding for its right inset");
    Require(hit_rect.right() >= indicator.right(), "help hit area must include the indicator");
    Click(button, hit_rect.center());
    Require(clicked == 1, "clicking the help indicator must not invoke the main action");
    Require(button.accessibleDescription() == QStringLiteral("Localized ownership help"),
            "help text must be exposed through accessibility metadata");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    TestHelpAreaDoesNotInvokeMainAction();
    return EXIT_SUCCESS;
}
