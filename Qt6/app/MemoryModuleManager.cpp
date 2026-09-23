#include "MemoryModuleManager.hpp"
#include "AppPaths.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

#include "Connector/BatteryCardInstance.hpp"
#include "Connector/MemoryCardCatalog.hpp"
#include "Connector/SoftwareDefinedCard.hpp"
#include "PC1500/PC1500Machine.hpp"
#include "PC1600/PC1600Machine.hpp"

namespace {

QVector<MemoryModuleManager::ModuleEntry> entriesFor(const QString& dir, CardHost host) {
    QVector<MemoryModuleManager::ModuleEntry> out;
    std::string err;
    const auto entries = scanMemoryCardDirectory(dir.toStdString(), &err);
    for (const auto& e : entries) {
        if (!e.compatibleWith(host)) continue;
        out.push_back({QString::fromStdString(e.moduleName), e.battery, e.rom});
    }
    return out;
}

}  // namespace

MemoryModuleManager::MemoryModuleManager(MachineController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {
    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setSingleShot(true);
    connect(m_debounceTimer, &QTimer::timeout, this, &MemoryModuleManager::flushPendingPersist);
}

CardHost MemoryModuleManager::hostFor(int slot) const { return hostForModel(slot, m_controller->currentModel()); }

CardHost MemoryModuleManager::hostForModel(int slot, Model model) {
    if (model == Model::PC1600) return slot == 2 ? CardHost::PC1600Slot2 : CardHost::PC1600Slot1;
    return model == Model::PC1500A ? CardHost::PC1500A : CardHost::PC1500;
}

QVector<MemoryModuleManager::ModuleEntry> MemoryModuleManager::bundledEntries(CardHost host) const {
    return entriesFor(AppPaths::bundledResourcesDir(), host);
}

// Leaves out saved cards that share a bundled card's name (any host):
// lookup is bundled-first, so they could never be loaded.
QVector<MemoryModuleManager::ModuleEntry> MemoryModuleManager::instanceEntries(CardHost host) const {
    QVector<ModuleEntry> out = entriesFor(AppPaths::instanceDir(), host);
    const QSet<QString> bundled = bundledNames();
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const ModuleEntry& e) { return bundled.contains(e.moduleName); }),
              out.end());
    return out;
}

void MemoryModuleManager::selectModule(int slot, const QString& moduleNameOrEmpty) {
    const int idx = slot - 1;
    flushPendingPersist();
    m_slots[idx].moduleName = moduleNameOrEmpty;
    m_slots[idx].instanceFilePath.clear();
    m_slots[idx].persistPending = false;
}

QString MemoryModuleManager::selectedModuleName(int slot) const { return m_slots[slot - 1].moduleName; }

bool MemoryModuleManager::isSlotBatteryBacked(int slot) const {
    const CardHost host = hostFor(slot);
    return isSlotBatteryBacked(slot, bundledEntries(host), instanceEntries(host));
}

bool MemoryModuleManager::isSlotBatteryBacked(int slot, const QVector<ModuleEntry>& bundled,
                                               const QVector<ModuleEntry>& instance) const {
    const SlotState& st = m_slots[slot - 1];
    if (st.moduleName.isEmpty()) return false;
    for (const auto& e : bundled)
        if (e.moduleName == st.moduleName) return e.battery;
    for (const auto& e : instance)
        if (e.moduleName == st.moduleName) return e.battery;
    return false;
}

bool MemoryModuleManager::slotHasInstanceFile(int slot) const {
    return !m_slots[slot - 1].instanceFilePath.isEmpty();
}

template <typename AttachFn>
void MemoryModuleManager::attachOneSlot(int slotIndex, CardHost host, AttachFn attach) {
    SlotState& st = m_slots[slotIndex];
    const QString bundledDir = AppPaths::bundledResourcesDir();
    const QString instDir = AppPaths::instanceDir();
    std::string path, err;
    if (resolveModuleSpecByName({bundledDir.toStdString(), instDir.toStdString()}, st.moduleName.toStdString(),
                                &path, &err)) {
        auto card = makeSoftwareDefinedCard(path, host, &err);
        if (card) {
            const QString resolvedPath = QString::fromStdString(path);
            st.instanceFilePath = AppPaths::isUnderDir(resolvedPath, instDir) ? resolvedPath : QString();
            attach(std::move(card));
            return;
        }
    }
    emit errorMessage(tr("Couldn't attach \"%1\": %2").arg(st.moduleName, QString::fromStdString(err)));
    st.moduleName.clear();
    st.instanceFilePath.clear();
}

void MemoryModuleManager::attachAllToFreshMachine() {
    if (auto* m1500 = m_controller->pc1500()) {
        m1500->expansionConnector().detach();  // no-op on a fresh machine; defensive/self-documenting
        if (!m_slots[0].moduleName.isEmpty()) {
            const CardHost host = hostFor(1);
            attachOneSlot(0, host,
                          [&](std::unique_ptr<ExpansionCard> card) { m1500->attachExpansionCard(std::move(card)); });
        }
        return;
    }
    if (auto* m1600 = m_controller->pc1600()) {
        m1600->detachSlot1();
        m1600->detachSlot2();
        if (!m_slots[0].moduleName.isEmpty())
            attachOneSlot(0, CardHost::PC1600Slot1,
                          [&](std::unique_ptr<ExpansionCard> c) { m1600->attachSlot1Card(std::move(c)); });
        if (!m_slots[1].moduleName.isEmpty())
            attachOneSlot(1, CardHost::PC1600Slot2,
                          [&](std::unique_ptr<ExpansionCard> c) { m1600->attachSlot2Card(std::move(c)); });
        return;
    }
}

QString MemoryModuleManager::attachedModuleName(int slot) const {
    if (auto* m1500 = m_controller->pc1500()) {
        const ExpansionCard* card = slot == 1 ? m1500->expansionConnector().attachedCard() : nullptr;
        return card ? QString::fromStdString(card->moduleName()) : QString();
    }
    if (auto* m1600 = m_controller->pc1600()) return QString::fromStdString(m1600->memory().slotModuleName(slot));
    return QString();
}

void MemoryModuleManager::syncFromPresetLoad(int slot, const QString& resolvedPathOrEmpty) {
    const int idx = slot - 1;
    m_slots[idx].moduleName = attachedModuleName(slot);
    m_slots[idx].instanceFilePath =
        AppPaths::isUnderDir(resolvedPathOrEmpty, AppPaths::instanceDir()) ? resolvedPathOrEmpty : QString();
    m_slots[idx].persistPending = false;
    emit moduleChanged(slot);
}

bool MemoryModuleManager::currentSlotImage(int slot, int* bankCount, std::vector<uint8_t>* image) const {
    if (auto* m1500 = m_controller->pc1500()) {
        auto* card = m1500->expansionConnector().attachedCard();
        if (!card) return false;
        *image = card->debugImage();
        // Passed through unchanged (not collapsed to 1) --
        // formatBatteryCardInitialContentBlock() itself distinguishes a
        // genuinely banked region from an unbanked one by this sign, per
        // ExpansionCard::debugBankCount()'s own -1-means-no-bank-concept
        // convention.
        *bankCount = card->debugBankCount();
        return !image->empty();
    }
    if (auto* m1600 = m_controller->pc1600()) {
        if (slot == 1) {
            *image = m1600->memory().slot1CardImage();
            *bankCount = m1600->memory().slot1CardBankCount();
        } else {
            *image = m1600->memory().slot2CardImage();
            *bankCount = m1600->memory().slot2CardBankCount();
        }
        return !image->empty();
    }
    return false;
}

QSet<QString> MemoryModuleManager::bundledNames() const {
    QSet<QString> names;
    for (const auto& e : scanMemoryCardDirectory(AppPaths::bundledResourcesDir().toStdString(), nullptr))
        names.insert(QString::fromStdString(e.moduleName));
    return names;
}

bool MemoryModuleManager::nameCollides(const QString& instanceName) const {
    const QString path = AppPaths::instancePathFor(instanceName);
    if (QFile::exists(path)) return true;
    std::string err;
    const auto entries = scanMemoryCardDirectory(AppPaths::instanceDir().toStdString(), &err);
    for (const auto& e : entries)
        if (QString::fromStdString(e.moduleName) == instanceName) return true;
    return false;
}

bool MemoryModuleManager::spliceCardImageInto(int slot, const QString& sourcePath, const QString& sourceModuleName,
                                               const QString& targetName, std::string* spliced, QString* error) {
    QFile srcFile(sourcePath);
    if (!srcFile.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("Couldn't read \"%1\".").arg(sourcePath);
        return false;
    }
    const std::string sourceText = srcFile.readAll().toStdString();

    int bankCount = 0;
    std::vector<uint8_t> image;
    if (!currentSlotImage(slot, &bankCount, &image)) {
        if (error) *error = tr("Couldn't read the live card contents.");
        return false;
    }
    const auto contentLines = formatBatteryCardInitialContentBlock(bankCount, image);

    std::string splErr;
    if (!spliceBatteryCardInstance(sourceText, targetName.toStdString(), sourceModuleName.toStdString(),
                                   contentLines, spliced, &splErr)) {
        if (error) *error = tr("Couldn't generate the instance file: %1").arg(QString::fromStdString(splErr));
        return false;
    }
    return true;
}

bool MemoryModuleManager::nameAndSave(int slot, const QString& instanceName, QString* error) {
    return saveSlotAs(slot, instanceName, /*fromPreset=*/false, error);
}

bool MemoryModuleManager::saveAsFromPreset(int slot, const QString& instanceName, QString* error) {
    return saveSlotAs(slot, instanceName, /*fromPreset=*/true, error);
}

bool MemoryModuleManager::saveSlotAs(int slot, const QString& instanceName, bool fromPreset, QString* error) {
    const QString name = instanceName.trimmed();
    if (name.isEmpty()) {
        *error = tr("Name cannot be empty.");
        return false;
    }
    SlotState& st = m_slots[slot - 1];
    if (st.moduleName.isEmpty()) {
        *error = tr("No module attached.");
        return false;
    }
    if (!fromPreset && !st.instanceFilePath.isEmpty()) {
        *error = tr("\"%1\" is already saved; changes are saved automatically.").arg(st.moduleName);
        return false;
    }
    if (name.contains(QLatin1Char('"'))) {
        *error = tr("Name cannot contain '\"'.");
        return false;
    }
    if (bundledNames().contains(name)) {
        *error = tr("\"%1\" is a built-in card name. Choose a different name.").arg(name);
        return false;
    }
    if (!fromPreset && nameCollides(name)) {
        *error = tr("A card named \"%1\" already exists. Choose a different name.").arg(name);
        return false;
    }

    std::string p;
    if (!resolveModuleSpecByName(AppPaths::bundledResourcesDir().toStdString(), st.moduleName.toStdString(), &p,
                                 nullptr)) {
        *error = tr("Couldn't find the source template for \"%1\".").arg(st.moduleName);
        return false;
    }
    const QString sourcePath = QString::fromStdString(p);

    std::string spliced;
    if (!spliceCardImageInto(slot, sourcePath, st.moduleName, name, &spliced, error)) return false;

    const QString newPath = AppPaths::instancePathFor(name);
    if (!AppPaths::atomicWriteFile(newPath, spliced)) {
        *error = tr("Couldn't write \"%1\".").arg(newPath);
        return false;
    }

    // Retarget the slot at the saved copy: it now shows under its new name
    // and autosaves there.
    st.moduleName = name;
    st.instanceFilePath = newPath;
    st.persistPending = false;
    emit moduleChanged(slot);
    return true;
}

void MemoryModuleManager::writeInstance(int slot) {
    SlotState& st = m_slots[slot - 1];
    if (st.instanceFilePath.isEmpty()) return;

    std::string spliced;
    if (!spliceCardImageInto(slot, st.instanceFilePath, st.moduleName, st.moduleName, &spliced, nullptr)) return;
    AppPaths::atomicWriteFile(st.instanceFilePath, spliced);
}

void MemoryModuleManager::markDirtyAndSchedulePersist() {
    bool anyPending = false;
    for (auto& st : m_slots) {
        if (!st.instanceFilePath.isEmpty()) {
            st.persistPending = true;
            anyPending = true;
        }
    }
    if (!anyPending) return;
    if (m_debounceTimer->isActive()) return;  // already pending -- coalesce
    m_debounceTimer->start(500);
}

void MemoryModuleManager::flushPendingPersist() {
    m_debounceTimer->stop();
    for (int i = 0; i < 2; ++i) {
        if (!m_slots[i].persistPending) continue;
        writeInstance(i + 1);
        m_slots[i].persistPending = false;
    }
}

void MemoryModuleManager::onModelChanged() {
    for (int i = 0; i < 2; ++i) {
        if (m_slots[i].persistPending) writeInstance(i + 1);
        m_slots[i] = SlotState{};
    }
}
