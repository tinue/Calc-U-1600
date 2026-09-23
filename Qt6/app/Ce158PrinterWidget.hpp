#pragma once
#include <QByteArray>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class MachineController;

// Docked CE-158 panel: what the interface printed on its Centronics port,
// plus the host path of its RS-232C port. A sibling of PlotterPaperWidget
// in MainWindow's debug row -- same header, button bar and pill buttons
// (ChromeColors / ChromeStyle) -- added while a CE-158 is attached.
//
// The printed bytes are kept raw for Save; the view shows them as text
// (LF = new line, CR dropped, other control bytes as <XX>).
class Ce158PrinterWidget : public QWidget {
    Q_OBJECT
public:
    explicit Ce158PrinterWidget(MachineController* controller, QWidget* parent = nullptr);

    // Called once per ~60 Hz frame tick while the CE-158 is attached:
    // drains the printer output and refreshes the serial-port line.
    void onFrameTick();

protected:
    void changeEvent(QEvent* event) override;

private:
    MachineController* m_controller; // not owned
    QLabel* m_header;
    QLabel* m_serialLabel;
    QPlainTextEdit* m_output;
    QWidget* m_buttonBar;
    QPushButton* m_saveButton;
    QPushButton* m_clearButton;
    QByteArray m_printed;
    QString m_serialPath;

    void applyChrome();
    void appendBytes(const QByteArray& bytes);
    void saveToFile();
    void clearOutput();
};
