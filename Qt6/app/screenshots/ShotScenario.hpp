#pragma once
#include <QList>
#include <QSize>
#include <QString>

// ── Screenshot scenario (`*.shots.yaml`) ──────────────────────────────────
//
// A scripted walk through the GUI that ends in image captures -- for the
// user guide (docs/screenshots/) and as a GUI smoke test. The format
// reference is docs/screenshots/README.md. Parsed with Core's YAML subset
// (Core/Yaml.hpp); all-or-nothing, line-numbered errors, like presets.
//
// Presets stay in charge of emulator state (model, cards, typed input): a
// shot starts from one (`preset:`), then its `steps:` drive the Qt UI --
// open a combo box, walk a menu, hold a faceplate key -- and `capture:`
// writes the picture.

struct ShotCaptureSpec {
    enum class Method { Qt, System };
    Method method = Method::Qt;
    // window | faceplate | lcd | paper | dialog | screen-region | <objectName>
    QString target = QStringLiteral("window");
    QString file;      // relative to the output directory
    int padding = 0;   // logical px of margin around the captured area
    double scale = 0;  // device pixels per logical px; 0 = the scenario's
};

struct ShotStep {
    enum class Kind {
        Preset,       // text = absolute preset path
        Reset,        // number = 1 for Reset All
        Key,          // text = key name: tap it
        HoldKey,      // text = key name: press (with faceplate overlay) and keep down
        ReleaseKeys,  // let go of a held key
        Type,         // text = line to type (ENTER appended)
        Run,          // number = emulated seconds
        Settle,       // number = wall-clock ms of UI event processing
        Click,        // text = objectName of a button
        Open,         // text = objectName of a combo box: show its popup
        Select,       // text = combo objectName, text2 = item text
        Menu,         // text = "Menu > Submenu": open, leave open
        Action,       // text = "Menu > ... > Item": trigger it
        Close,        // close the topmost popup/dialog
        Capture,      // capture
    };
    Kind kind = Kind::Settle;
    QString text;
    QString text2;
    double number = 0;
    ShotCaptureSpec capture;
    int line = 0;
};

struct Shot {
    QString name;
    int line = 0;
    QList<ShotStep> steps;
};

struct ShotScenario {
    QString path;        // the scenario file itself
    QString outDir;      // absolute
    QSize windowSize;    // invalid = leave the window as it is
    QString appearance;  // "light" (default) | "dark" | "system"
    double scale = 2.0;  // default capture scale
    int settleMs = 150;  // pause after each UI step
    QList<Shot> shots;
};

bool parseShotScenario(const QString& path, ShotScenario* out, QString* error);
