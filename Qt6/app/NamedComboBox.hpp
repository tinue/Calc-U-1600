#pragma once
#include <QComboBox>

// ── NamedComboBox: long labels in the list, a short name once chosen ─────
//
// A combo whose items show their full text (a preset's description) in the
// open list, while the closed combo shows the current item's short name
// (kShortNameRole; the item text when unset) -- so a chosen "Towers of
// Hanoi, up to seven bricks" reads "Hanoi" instead of being cut off.
class NamedComboBox : public QComboBox {
public:
    static constexpr int kShortNameRole = Qt::UserRole + 1;

    using QComboBox::QComboBox;

    // An item: `label` in the list, `shortName` once chosen, `data` as
    // Qt::UserRole, `toolTip` on hover.
    void addNamedItem(const QString& label, const QString& shortName, const QVariant& data, const QString& toolTip);

    // Sized for the short names, not the list's long labels.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString shortName(int index) const;
};
