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

// The Qt side of shared::MachineSession (Shared/MachineSession.hpp has the
// behaviour and the docs): owns the session, turns its Listener calls into
// signals, message boxes and the memory-module / floppy / debugger hooks,
// and keeps the QString-typed calls the widgets use. Everything else is
// reached through session().
class MachineController : public QObject, private shared::MachineSession::Listener {
    Q_OBJECT
public:
    explicit MachineController(QObject* parent = nullptr);
    ~MachineController() override;

    shared::MachineSession& session() { return *m_session; }
    const shared::MachineSession& session() const { return *m_session; }

    // ---- Forwarded unchanged (see MachineSession) ----
    void resetRomSelectionsToDefault() { m_session->resetRomSelectionsToDefault(); }
    void switchModel(Model model, bool keepPlotter = false) { m_session->switchModel(model, keepPlotter); }
    Model currentModel() const { return m_session->currentModel(); }
    void setPC1500RomRevision(PC1500RomRevision r) { m_session->setPC1500RomRevision(r); }
    PC1500RomRevision pc1500RomRevision() const { return m_session->pc1500RomRevision(); }
    void setPC1600RomVersion(PC1600RomVersion v) { m_session->setPC1600RomVersion(v); }
    PC1600RomVersion pc1600RomVersion() const { return m_session->pc1600RomVersion(); }
    void setCE1600PRomVersion(CE1600PRomVersion v) { m_session->setCE1600PRomVersion(v); }
    CE1600PRomVersion ce1600pRomVersion() const { return m_session->ce1600pRomVersion(); }
    void resetToPrompt(bool allReset) { m_session->resetToPrompt(allReset); }
    void seedClockFromHost() { m_session->seedClockFromHost(); }
    void powerCycleAround(const std::function<void()>& change) { m_session->powerCycleAround(change); }

    void pressKey(const std::string& name) { m_session->pressKey(name); }
    void releaseKey(const std::string& name) { m_session->releaseKey(name); }
    void setOnKeyPressed(bool pressed) { m_session->setOnKeyPressed(pressed); }
    void tapShiftedKey(const std::string& baseName) { m_session->tapShiftedKey(baseName); }
    void tapKey(const std::string& name) { m_session->tapKey(name); }
    void enqueueKey(const std::string& name) { m_session->enqueueKey(name); }
    void enqueueShiftedKey(const std::string& baseName) { m_session->enqueueShiftedKey(baseName); }
    void pasteText(const std::string& text) { m_session->pasteText(text); }
    void typeCommand(const std::string& line) { m_session->typeCommand(line); }
    bool pasteActive() const { return m_session->pasteActive(); }
    void cancelPaste() { m_session->cancelPaste(); }
    void interruptPaste() { m_session->interruptPaste(); }
    void typeLiveStep(const PasteStep& step) { m_session->typeLiveStep(step); }
    bool liveTypingActive() const { return m_session->liveTypingActive(); }

    GrayImage currentScreenImage() const { return m_session->currentScreenImage(); }
    void advance(std::uint64_t cyclesBudget) { m_session->advance(cyclesBudget); }
    std::size_t drainAudio(std::int16_t* out, std::size_t max) { return m_session->drainAudio(out, max); }
    void discardAudio() { m_session->discardAudio(); }
    DisplayFrame currentDisplay() const { return m_session->currentDisplay(); }
    bool lcdText(LcdText* out) const { return m_session->lcdText(out); }
    double clockHz() const { return m_session->clockHz(); }

    PC1500Machine* pc1500() const { return m_session->pc1500(); }
    PC1600Machine* pc1600() const { return m_session->pc1600(); }
    bool hasLiveMachine() const { return m_session->hasLiveMachine(); }
    bool isMachinePoweredOn() const { return m_session->isMachinePoweredOn(); }
    template <class F>
    void withMachine(F&& f) const { m_session->withMachine(std::forward<F>(f)); }
    template <class R, class F>
    R withMachine(R none, F&& f) const { return m_session->withMachine(std::move(none), std::forward<F>(f)); }

    PC1500Machine& resetBareForPresetPC1500(PC1500Variant variant) { return m_session->resetBareForPresetPC1500(variant); }
    PC1600Machine& resetBareForPresetPC1600(PC1600RomVersion version, CE1600PRomVersion ce1600pVersion) {
        return m_session->resetBareForPresetPC1600(version, ce1600pVersion);
    }
    void finishPresetLoad(Model model) { m_session->finishPresetLoad(model); }

    std::vector<std::string> inspectorView(inspect::View view) const { return m_session->inspectorView(view); }
    std::vector<inspect::MenuEntry> inspectorMenu(bool dumps) const { return m_session->inspectorMenu(dumps); }
    std::vector<std::uint8_t> debugSlotCardImage(int slot, int* bankCount) const {
        return m_session->debugSlotCardImage(slot, bankCount);
    }
    void endTrace() { m_session->endTrace(); }
    std::uint64_t traceBytes() const { return m_session->traceBytes(); }
    bool traceActive() const { return m_session->traceActive(); }

    void detachCE150() { m_session->detachCE150(); }
    bool ce150Attached() const { return m_session->ce150Attached(); }
    void detachCE1600P() { m_session->detachCE1600P(); }
    bool ce1600pAttached() const { return m_session->ce1600pAttached(); }
    void detachCE158() { m_session->detachCE158(); }
    bool ce158Attached() const { return m_session->ce158Attached(); }
    std::vector<std::uint8_t> drainCE158PrinterOutput() { return m_session->drainCE158PrinterOutput(); }
    void detachHostDrive() { m_session->detachHostDrive(); }
    bool hostDriveAttached() const { return m_session->hostDriveAttached(); }
    bool tapeInterfaceAttached() const { return m_session->tapeInterfaceAttached(); }
    void tapeRecord(const QString& path) { m_session->tapeRecord(path.toStdString()); }
    TapeDeck::Status tapeStatus() const { return m_session->tapeStatus(); }
    std::vector<AlpsPlotterMechanism::FlatPoint> ce150PlotPoints() const { return m_session->ce150PlotPoints(); }
    std::uint64_t ce150PlotRevision() const { return m_session->ce150PlotRevision(); }
    void clearCE150Paper() { m_session->clearCE150Paper(); }
    std::vector<AlpsPlotterMechanism::FlatPoint> ce1600pPlotPoints() const { return m_session->ce1600pPlotPoints(); }
    std::uint64_t ce1600pPlotRevision() const { return m_session->ce1600pPlotRevision(); }
    void clearCE1600PPaper() { m_session->clearCE1600PPaper(); }

    // ---- QString translations ----
    bool beginTrace(const QString& path) { return m_session->beginTrace(path.toStdString()); }
    bool attachCE150(QString* error = nullptr);
    bool attachCE1600P(QString* error = nullptr);
    bool swapCE1600PRom(CE1600PRomVersion version, QString* error = nullptr);
    bool attachCE158(QString* error = nullptr);
    bool attachHostDrive(const QString& dir, QString* error = nullptr);
    QString hostDriveDirectory() const { return QString::fromStdString(m_session->hostDriveDirectory()); }
    bool tapePlay(const QString& path, QString* error = nullptr);
    bool tapeEject(QString* error = nullptr);
    QString tapeTakeLastError() { return QString::fromStdString(m_session->tapeTakeLastError()); }
    void refreshSerialLinkDirectory() { m_session->refreshSerialLinkDirectory(); }
    QString serialLinkStatus() const { return QString::fromStdString(m_session->serialLinkStatus()); }
    QString ce158SerialLinkStatus() const { return QString::fromStdString(m_session->ce158SerialLinkStatus()); }

    // ---- Qt side ----
    // Wired up once by MainWindow right after constructing both objects
    // (raw, not owned -- breaks the construction-order cycle: the managers'
    // ctors need a MachineController*, but the session's first boot, from
    // this constructor, calls back into them). Until set, the hooks are
    // skipped -- harmless, since no module or disk is selected at cold start.
    void setModuleManager(MemoryModuleManager* mgr) { m_moduleManager = mgr; }
    void setFloppyManager(FloppyDiskManager* mgr) { m_floppyManager = mgr; }

    // The debugger (DAP server + session), see debug/DebugController.
    DebugController* debugController() const { return m_debug.get(); }
    // Starts, stops or rebinds the DAP server after a Settings change.
    void refreshDebugServer();

signals:
    void modelChanged(Model model);
    // refreshSerialLinkDirectory() moved the PTY symlinks.
    void serialLinksMoved();

private:
    // shared::MachineSession::Listener
    void romMissing(const std::string& error, const std::string& searchedIn) override;
    void notice(shared::MachineSession::Notice notice, const std::string& detail) override;
    void machineAboutToChange() override;
    void machineReplaced() override;
    void modelSwitched(Model model) override { emit modelChanged(model); }
    void attachModulesToFreshMachine() override;
    void flushFloppy() override;
    void insertSelectedDisk() override;
    bool runDebuggerSlice(std::uint64_t cycles) override;
    void serialLinksRelinked() override { emit serialLinksMoved(); }

    MemoryModuleManager* m_moduleManager = nullptr; // not owned
    FloppyDiskManager* m_floppyManager = nullptr;   // not owned
    std::unique_ptr<shared::MachineSession> m_session;
    // Declared after the session so it goes first on destruction: its
    // debug target still refers to the live machine.
    std::unique_ptr<DebugController> m_debug;
};
