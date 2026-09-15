#include "AboutDialog.hpp"
#include "Version.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("About Calc-U-1600"));

    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel(tr("Calc-U-1600"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 4);
    title->setFont(titleFont);
    layout->addWidget(title);

    layout->addWidget(new QLabel(tr("Version %1 (%2)").arg(QStringLiteral(APP_VERSION), QStringLiteral(APP_BUILD_ID)), this));
    layout->addWidget(new QLabel(tr("A Sharp PC-1500/1500A/1600 pocket-computer emulator."), this));

    auto* licenseLabel = new QLabel(
        tr("See LICENSE and THIRD-PARTY-NOTICES.md, shipped alongside this app, for licensing details."), this);
    licenseLabel->setWordWrap(true);
    layout->addWidget(licenseLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}
