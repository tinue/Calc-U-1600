#pragma once
#include <QObject>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Connector/AlpsPlotterMechanism.hpp"
#include "Debug/Inspect/Inspector.hpp"
#include "Display/LcdScreenshot.hpp"
#include "Display/LcdText.hpp"
#include "KeyPaste.hpp"
#include "PC1500/PC1500Variant.hpp"
#include "Serial/LazySerialLink.hpp"
#include "Serial/PtySerialLink.hpp"
#include "Tape/TapeDeck.hpp"
#include "TraceTypes.hpp"
#include "Shared/MachineModel.hpp"
#include "Shared/AppSession.hpp"
#include "Shared/MachineSession.hpp"

class PC1500Machine;
class PC1600Machine;
class MemoryModuleManager;
class DebugController;
class FloppyDiskManager;

using shared::Model;
using shared::PC1600RomVersion;
using shared::CE1600PRomVersion;

inline QString modelSettingsKey(Model model) { return QString::fromLatin1(shared::modelKey(model)); }

using shared::DisplayFrame;

// The Qt side of shared::AppSession (Shared/AppSession.hpp and
// Shared/MachineSession.hpp have the behaviour and the docs): owns the
// app session, turns its Platform calls into signals, message boxes and
// the debugger hooks, and keeps the QString-typed calls the widgets use.
// The machine is session(), the rest app().
class MachineController : public QObject, private shared::AppSession::Platform {
    Q_OBJECT
public:
    explicit MachineController(QObject* parent = nullptr);
    ~MachineController() override;

    shared::AppSession& app() { return *m_app; }
    shared::MachineSession& session() { return m_app->machine(); }
    const shared::MachineSession& session() const { return m_app->machine(); }

    // ---- Forwarded unchanged (see MachineSession) ----
    void resetRomSelectionsToDefault() { session().resetRomSelectionsToDefault(); }
    void switchModel(Model model, bool keepPlotter = false) { session().switchModel(model, keepPlotter); }
    Model currentModel() const { return session().currentModel(); }
    void setPC1500RomRevision(PC1500RomRevision r) { session().setPC1500RomRevision(r); }
    PC1500RomRevision pc1500RomRevision() const { return session().pc1500RomRevision(); }
    void setPC1600RomVersion(PC1600RomVersion v) { session().setPC1600RomVersion(v); }
    PC1600RomVersion pc1600RomVersion() const { return session().pc1600RomVersion(); }
    void setCE1600PRomVersion(CE1600PRomVersion v) { session().setCE1600PRomVersion(v); }
    CE1600PRomVersion ce1600pRomVersion() const { return session().ce1600pRomVersion(); }
    void resetToPrompt(bool allReset) { session().resetToPrompt(allReset); }
    void seedClockFromHost() { session().seedClockFromHost(); }
    void powerCycleAround(const std::function<void()>& change) { session().powerCycleAround(change); }

    void pressKey(const std::string& name) { session().pressKey(name); }
    void releaseKey(const std::string& name) { session().releaseKey(name); }
    void setOnKeyPressed(bool pressed) { session().setOnKeyPressed(pressed); }
    void tapShiftedKey(const std::string& baseName) { session().tapShiftedKey(baseName); }
    void tapKey(const std::string& name) { session().tapKey(name); }
    void enqueueKey(const std::string& name) { session().enqueueKey(name); }
    void enqueueShiftedKey(const std::string& baseName) { session().enqueueShiftedKey(baseName); }
    void pasteText(const std::string& text) { session().pasteText(text); }
    void typeCommand(const std::string& line) { session().typeCommand(line); }
    bool pasteActive() const { return session().pasteActive(); }
    void cancelPaste() { session().cancelPaste(); }
    void interruptPaste() { session().interruptPaste(); }
    void typeLiveStep(const PasteStep& step) { session().typeLiveStep(step); }
    bool liveTypingActive() const { return session().liveTypingActive(); }

    GrayImage currentScreenImage() const { return session().currentScreenImage(); }
    void advance(std::uint64_t cyclesBudget) { session().advance(cyclesBudget); }
    std::size_t drainAudio(std::int16_t* out, std::size_t max) { return session().drainAudio(out, max); }
    void discardAudio() { session().discardAudio(); }
    DisplayFrame currentDisplay() const { return session().currentDisplay(); }
    bool lcdText(LcdText* out) const { return session().lcdText(out); }
    double clockHz() const { return session().clockHz(); }

    PC1500Machine* pc1500() const { return session().pc1500(); }
    PC1600Machine* pc1600() const { return session().pc1600(); }
    bool hasLiveMachine() const { return session().hasLiveMachine(); }
    bool isMachinePoweredOn() const { return session().isMachinePoweredOn(); }
    template <class F>
    void withMachine(F&& f) const { session().withMachine(std::forward<F>(f)); }
    template <class R, class F>
    R withMachine(R none, F&& f) const { return session().withMachine(std::move(none), std::forward<F>(f)); }

    PC1500Machine& resetBareForPresetPC1500(PC1500Variant variant) { return session().resetBareForPresetPC1500(variant); }
    PC1600Machine& resetBareForPresetPC1600(PC1600RomVersion version, CE1600PRomVersion ce1600pVersion) {
        return session().resetBareForPresetPC1600(version, ce1600pVersion);
    }
    void finishPresetLoad(Model model) { session().finishPresetLoad(model); }

    std::vector<std::string> inspectorView(inspect::View view) const { return session().inspectorView(view); }
    std::vector<inspect::MenuEntry> inspectorMenu(bool dumps) const { return session().inspectorMenu(dumps); }
    std::vector<std::uint8_t> debugSlotCardImage(int slot, int* bankCount) const {
        return session().debugSlotCardImage(slot, bankCount);
    }
    void endTrace() { session().endTrace(); }
    std::uint64_t traceBytes() const { return session().traceBytes(); }
    bool traceActive() const { return session().traceActive(); }

    void detachCE150() { session().detachCE150(); }
    bool ce150Attached() const { return session().ce150Attached(); }
    void detachCE1600P() { session().detachCE1600P(); }
    bool ce1600pAttached() const { return session().ce1600pAttached(); }
    void detachCE158() { session().detachCE158(); }
    bool ce158Attached() const { return session().ce158Attached(); }
    std::vector<std::uint8_t> drainCE158PrinterOutput() { return session().drainCE158PrinterOutput(); }
    void detachHostDrive() { session().detachHostDrive(); }
    bool hostDriveAttached() const { return session().hostDriveAttached(); }
    bool tapeInterfaceAttached() const { return session().tapeInterfaceAttached(); }
    void tapeRecord(const QString& path) { session().tapeRecord(path.toStdString()); }
    TapeDeck::Status tapeStatus() const { return session().tapeStatus(); }
    std::vector<AlpsPlotterMechanism::FlatPoint> ce150PlotPoints() const { return session().ce150PlotPoints(); }
    std::uint64_t ce150PlotRevision() const { return session().ce150PlotRevision(); }
    void clearCE150Paper() { session().clearCE150Paper(); }
    std::vector<AlpsPlotterMechanism::FlatPoint> ce1600pPlotPoints() const { return session().ce1600pPlotPoints(); }
    std::uint64_t ce1600pPlotRevision() const { return session().ce1600pPlotRevision(); }
    void clearCE1600PPaper() { session().clearCE1600PPaper(); }

    // ---- QString translations ----
    bool beginTrace(const QString& path) { return session().beginTrace(path.toStdString()); }
    bool attachCE150(QString* error = nullptr);
    bool attachCE1600P(QString* error = nullptr);
    bool swapCE1600PRom(CE1600PRomVersion version, QString* error = nullptr);
    bool attachCE158(QString* error = nullptr);
    bool attachHostDrive(const QString& dir, QString* error = nullptr);
    QString hostDriveDirectory() const { return QString::fromStdString(session().hostDriveDirectory()); }
    bool tapePlay(const QString& path, QString* error = nullptr);
    bool tapeEject(QString* error = nullptr);
    QString tapeTakeLastError() { return QString::fromStdString(session().tapeTakeLastError()); }
    void refreshSerialLinkDirectory() { session().refreshSerialLinkDirectory(); }
    QString serialLinkStatus() const { return QString::fromStdString(session().serialLinkStatus()); }
    QString ce158SerialLinkStatus() const { return QString::fromStdString(session().ce158SerialLinkStatus()); }

    // ---- Qt side ----
    // The debugger (DAP server + session), see debug/DebugController.
    DebugController* debugController() const { return m_debug.get(); }
    // Starts, stops or rebinds the DAP server after a Settings change.
    void refreshDebugServer();

signals:
    void modelChanged(Model model);
    // refreshSerialLinkDirectory() moved the PTY symlinks.
    void serialLinksMoved();
    // See shared::AppSession::Platform.
    void bayModuleChanged(int slot);
    void tapeListChanged();
    void bayErrorMessage(shared::Bay bay, const QString& text);
    void presetWasArmed();
    void peripheralAttachFailed(const QString& device, const QString& reason);

private:
    // shared::AppSession::Platform
    void romMissing(const std::string& error, const std::string& searchedIn) override;
    void notice(shared::MachineSession::Notice notice, const std::string& detail) override;
    void machineAboutToChange() override;
    void machineReplaced() override;
    void modelSwitched(Model model) override { emit modelChanged(model); }
    bool runDebuggerSlice(std::uint64_t cycles) override;
    void serialLinksRelinked() override { emit serialLinksMoved(); }
    void moduleChanged(int slot) override { emit bayModuleChanged(slot); }
    void tapesChanged() override { emit tapeListChanged(); }
    void bayError(shared::Bay bay, const std::string& text) override;
    void presetArmed() override { emit presetWasArmed(); }
    void presetLog(const std::string& line) override;
    void attachFailed(const std::string& device, const std::string& reason) override;

    std::unique_ptr<shared::AppSession> m_app;
    // Declared after the session so it goes first on destruction: its
    // debug target still refers to the live machine.
    std::unique_ptr<DebugController> m_debug;
};
