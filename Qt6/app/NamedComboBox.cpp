#include "NamedComboBox.hpp"

#include <QAbstractItemView>

#include <algorithm>
#include <QStyleOptionComboBox>
#include <QStylePainter>

void NamedComboBox::addNamedItem(const QString& label, const QString& shortName, const QVariant& data,
                                 const QString& toolTip) {
    addItem(label, data);
    const int index = count() - 1;
    setItemData(index, shortName, kShortNameRole);
    if (!toolTip.isEmpty()) setItemData(index, toolTip, Qt::ToolTipRole);
    // The open list is as wide as its longest label, not as the combo.
    const int labelWidth = fontMetrics().horizontalAdvance(label) + 48;
    view()->setMinimumWidth(std::max(view()->minimumWidth(), labelWidth));
}

QString NamedComboBox::shortName(int index) const {
    const QString name = itemData(index, kShortNameRole).toString();
    return name.isEmpty() ? itemText(index) : name;
}

QSize NamedComboBox::sizeHint() const {
    QStyleOptionComboBox opt;
    initStyleOption(&opt);
    int width = 0;
    for (int i = 0; i < count(); ++i) width = std::max(width, fontMetrics().horizontalAdvance(shortName(i)));
    const QSize text(width, fontMetrics().height());
    return style()->sizeFromContents(QStyle::CT_ComboBox, &opt, text, this);
}

QSize NamedComboBox::minimumSizeHint() const { return sizeHint(); }

void NamedComboBox::paintEvent(QPaintEvent*) {
    QStylePainter painter(this);
    QStyleOptionComboBox opt;
    initStyleOption(&opt);
    if (currentIndex() >= 0) opt.currentText = shortName(currentIndex());
    painter.drawComplexControl(QStyle::CC_ComboBox, opt);
    painter.drawControl(QStyle::CE_ComboBoxLabel, opt);
}
