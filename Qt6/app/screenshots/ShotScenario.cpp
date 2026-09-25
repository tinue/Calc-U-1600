#include "ShotScenario.hpp"

#include "Yaml.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {

QString lineError(int line, const QString& msg) {
    return QStringLiteral("line %1: %2").arg(line).arg(msg);
}

bool scalarString(const YamlNode& node, QString* out, QString* error) {
    std::string s, err;
    if (!node.asString(&s, &err)) {
        *error = QString::fromStdString(err);
        return false;
    }
    *out = QString::fromStdString(s).trimmed();
    return true;
}

bool scalarNumber(const YamlNode& node, double* out, QString* error) {
    QString s;
    if (!scalarString(node, &s, error)) return false;
    bool ok = false;
    *out = s.toDouble(&ok);
    if (!ok || *out < 0) {
        *error = lineError(node.line, QStringLiteral("expected a non-negative number, got '%1'").arg(s));
        return false;
    }
    return true;
}

bool parseCapture(const YamlNode& node, double defaultScale, ShotCaptureSpec* out, QString* error) {
    // `capture: file.png` (the whole window) or a map with the options.
    if (node.isScalar()) {
        if (!scalarString(node, &out->file, error)) return false;
    } else if (node.isMap()) {
        std::string err;
        if (!node.requireOnlyKeys({"file", "target", "method", "padding", "scale"}, &err)) {
            *error = QString::fromStdString(err);
            return false;
        }
        for (const auto& [key, value] : node.map) {
            if (key == "file") {
                if (!scalarString(value, &out->file, error)) return false;
            } else if (key == "target") {
                QString t;
                if (!scalarString(value, &t, error)) return false;
                using T = ShotCaptureSpec::Target;
                if (t == QLatin1String("window")) out->target = T::Window;
                else if (t == QLatin1String("dialog")) out->target = T::Dialog;
                else if (t == QLatin1String("plot")) out->target = T::Plot;
                else if (t == QLatin1String("lcd-image")) out->target = T::LcdImage;
                else if (t == QLatin1String("screen-region")) out->target = T::ScreenRegion;
                else {
                    out->target = T::Widget;
                    out->objectName = t;
                }
            } else if (key == "method") {
                QString m;
                if (!scalarString(value, &m, error)) return false;
                if (m == QLatin1String("qt")) out->method = ShotCaptureSpec::Method::Qt;
                else if (m == QLatin1String("system")) out->method = ShotCaptureSpec::Method::System;
                else {
                    *error = lineError(value.line, QStringLiteral("'method' must be 'qt' or 'system'"));
                    return false;
                }
            } else if (key == "padding") {
                double p = 0;
                if (!scalarNumber(value, &p, error)) return false;
                out->padding = static_cast<int>(p);
            } else if (key == "scale") {
                if (!scalarNumber(value, &out->scale, error)) return false;
                if (out->scale <= 0) {
                    *error = lineError(value.line, QStringLiteral("'scale' must be positive"));
                    return false;
                }
            }
        }
    } else {
        *error = lineError(node.line, QStringLiteral("'capture' needs a file name or a map"));
        return false;
    }
    if (out->file.isEmpty()) {
        *error = lineError(node.line, QStringLiteral("'capture' needs a 'file'"));
        return false;
    }
    if (!out->file.endsWith(QLatin1String(".png"), Qt::CaseInsensitive)) {
        *error = lineError(node.line, QStringLiteral("capture file '%1' must be a .png").arg(out->file));
        return false;
    }
    if (out->scale <= 0) out->scale = defaultScale;
    if (out->target == ShotCaptureSpec::Target::ScreenRegion && out->method != ShotCaptureSpec::Method::System) {
        *error = lineError(node.line, QStringLiteral("target 'screen-region' needs 'method: system'"));
        return false;
    }
    return true;
}

bool parseStep(const YamlNode& node, const QDir& baseDir, const ShotScenario& scenario, ShotStep* out,
               QString* error) {
    out->line = node.line;
    QString verb;
    const YamlNode* value = nullptr;
    if (node.isScalar()) {
        // A bare `- close` / `- release`.
        if (!scalarString(node, &verb, error)) return false;
    } else if (node.isMap() && node.map.size() == 1) {
        verb = QString::fromStdString(node.map.front().first);
        value = &node.map.front().second;
    } else {
        *error = lineError(node.line, QStringLiteral("expected one '- verb: value' per step"));
        return false;
    }

    const bool hasValue = value && !value->isNull();
    auto needText = [&](QString* text) -> bool {
        if (!hasValue) {
            *error = lineError(node.line, QStringLiteral("'%1' needs a value").arg(verb));
            return false;
        }
        if (!scalarString(*value, text, error)) return false;
        if (text->isEmpty()) {
            *error = lineError(node.line, QStringLiteral("'%1' needs a value").arg(verb));
            return false;
        }
        return true;
    };
    auto noValue = [&]() -> bool {
        if (hasValue) {
            *error = lineError(node.line, QStringLiteral("'%1' takes no value").arg(verb));
            return false;
        }
        return true;
    };

    using K = ShotStep::Kind;
    if (verb == QLatin1String("preset")) {
        out->kind = K::Preset;
        if (!needText(&out->text)) return false;
        out->text = QDir::cleanPath(baseDir.absoluteFilePath(out->text));
        if (!QFileInfo::exists(out->text)) {
            *error = lineError(node.line, QStringLiteral("preset not found: %1").arg(out->text));
            return false;
        }
    } else if (verb == QLatin1String("reset")) {
        out->kind = K::Reset;
        if (hasValue) {
            QString which;
            if (!scalarString(*value, &which, error)) return false;
            if (which != QLatin1String("all")) {
                *error = lineError(node.line, QStringLiteral("'reset' takes no value or 'all'"));
                return false;
            }
            out->number = 1;
        }
    } else if (verb == QLatin1String("key")) {
        out->kind = K::Key;
        if (!needText(&out->text)) return false;
    } else if (verb == QLatin1String("hold-key")) {
        out->kind = K::HoldKey;
        if (!needText(&out->text)) return false;
    } else if (verb == QLatin1String("release")) {
        out->kind = K::ReleaseKeys;
        if (!noValue()) return false;
    } else if (verb == QLatin1String("type")) {
        out->kind = K::Type;
        if (!needText(&out->text)) return false;
    } else if (verb == QLatin1String("run") || verb == QLatin1String("settle")) {
        out->kind = verb == QLatin1String("run") ? K::Run : K::Settle;
        if (!hasValue) {
            *error = lineError(node.line, QStringLiteral("'%1' needs a number").arg(verb));
            return false;
        }
        if (!scalarNumber(*value, &out->number, error)) return false;
    } else if (verb == QLatin1String("click") || verb == QLatin1String("open")) {
        out->kind = verb == QLatin1String("click") ? K::Click : K::Open;
        if (!needText(&out->text)) return false;
    } else if (verb == QLatin1String("select")) {
        // `select: controlbar.model = PC-1600`
        out->kind = K::Select;
        QString spec;
        if (!needText(&spec)) return false;
        const int eq = spec.indexOf(QLatin1Char('='));
        if (eq < 0) {
            *error = lineError(node.line, QStringLiteral("expected 'select: <objectName> = <item text>'"));
            return false;
        }
        out->text = spec.left(eq).trimmed();
        out->text2 = spec.mid(eq + 1).trimmed();
        if (out->text.isEmpty() || out->text2.isEmpty()) {
            *error = lineError(node.line, QStringLiteral("expected 'select: <objectName> = <item text>'"));
            return false;
        }
    } else if (verb == QLatin1String("menu") || verb == QLatin1String("action")) {
        out->kind = verb == QLatin1String("menu") ? K::Menu : K::Action;
        if (!needText(&out->text)) return false;
        if (out->kind == K::Action && !out->text.contains(QLatin1Char('>'))) {
            *error = lineError(node.line, QStringLiteral("'action' needs a path like 'Edit > Settings…'"));
            return false;
        }
    } else if (verb == QLatin1String("close")) {
        out->kind = K::Close;
        if (!noValue()) return false;
    } else if (verb == QLatin1String("choose-file")) {
        out->kind = K::ChooseFile;
        if (!needText(&out->text)) return false;
        out->text = QDir::cleanPath(baseDir.absoluteFilePath(out->text));
        if (!QFileInfo::exists(out->text)) {
            *error = lineError(node.line, QStringLiteral("file not found: %1").arg(out->text));
            return false;
        }
    } else if (verb == QLatin1String("enter-text")) {
        out->kind = K::EnterText;
        if (!needText(&out->text)) return false;
    } else if (verb == QLatin1String("capture")) {
        out->kind = K::Capture;
        if (!hasValue) {
            *error = lineError(node.line, QStringLiteral("'capture' needs a file name or a map"));
            return false;
        }
        if (!parseCapture(*value, scenario.scale, &out->capture, error)) return false;
    } else {
        *error = lineError(node.line, QStringLiteral("unknown step '%1'").arg(verb));
        return false;
    }
    return true;
}

bool parseSize(const YamlNode& node, QSize* out, QString* error);

bool parseShot(const YamlNode& node, const QDir& baseDir, const ShotScenario& scenario, Shot* out,
               QString* error) {
    out->line = node.line;
    if (!node.isMap()) {
        *error = lineError(node.line, QStringLiteral("each shot must be a map ('- name: ...')"));
        return false;
    }
    std::string err;
    if (!node.requireOnlyKeys({"name", "window", "preset", "steps", "capture"}, &err)) {
        *error = QString::fromStdString(err);
        return false;
    }
    const YamlNode* name = node.find("name");
    if (!name || !scalarString(*name, &out->name, error) || out->name.isEmpty()) {
        if (error->isEmpty()) *error = lineError(node.line, QStringLiteral("shot needs a 'name'"));
        return false;
    }
    if (const YamlNode* window = node.find("window")) {
        if (!parseSize(*window, &out->windowSize, error)) return false;
    }
    // `preset:` first, then the steps, then the shot-level `capture:` --
    // sugar for a leading `- preset:` / trailing `- capture:` step.
    if (const YamlNode* preset = node.find("preset")) {
        YamlNode step;
        step.type = YamlNode::Type::Map;
        step.line = preset->line;
        step.map.emplace_back("preset", *preset);
        ShotStep s;
        if (!parseStep(step, baseDir, scenario, &s, error)) return false;
        out->steps.append(s);
    }
    if (const YamlNode* steps = node.find("steps")) {
        if (!steps->isSeq()) {
            *error = lineError(steps->line, QStringLiteral("'steps' must be a list"));
            return false;
        }
        for (const YamlNode& item : steps->seq) {
            ShotStep s;
            if (!parseStep(item, baseDir, scenario, &s, error)) return false;
            out->steps.append(s);
        }
    }
    if (const YamlNode* capture = node.find("capture")) {
        ShotStep s;
        s.kind = ShotStep::Kind::Capture;
        s.line = capture->line;
        if (!parseCapture(*capture, scenario.scale, &s.capture, error)) return false;
        out->steps.append(s);
    }
    return true;
}

bool parseSize(const YamlNode& node, QSize* out, QString* error) {
    // `window: 900x1100`
    QString s;
    if (!scalarString(node, &s, error)) return false;
    const QStringList parts = s.split(QLatin1Char('x'));
    bool okW = false, okH = false;
    if (parts.size() == 2) *out = QSize(parts[0].trimmed().toInt(&okW), parts[1].trimmed().toInt(&okH));
    if (!okW || !okH || out->width() <= 0 || out->height() <= 0) {
        *error = lineError(node.line, QStringLiteral("'window' must be '<width>x<height>'"));
        return false;
    }
    return true;
}

} // namespace

bool parseShotScenario(const QString& path, ShotScenario* out, QString* error) {
    auto fail = [&](const QString& msg) {
        *error = QStringLiteral("%1: %2").arg(path, msg);
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(file.errorString());
    YamlNode root;
    std::string err;
    if (!parseYaml(file.readAll().toStdString(), &root, &err)) return fail(QString::fromStdString(err));
    if (!root.isMap()) return fail(QStringLiteral("expected a map at the top level"));
    if (!root.requireOnlyKeys({"out", "window", "appearance", "scale", "settle", "shots"}, &err))
        return fail(QString::fromStdString(err));

    const QDir baseDir = QFileInfo(path).absoluteDir();
    out->path = QFileInfo(path).absoluteFilePath();
    out->outDir = baseDir.absolutePath();
    out->appearance = QStringLiteral("light");

    QString msg;
    if (const YamlNode* n = root.find("out")) {
        QString dir;
        if (!scalarString(*n, &dir, &msg)) return fail(msg);
        out->outDir = QDir::cleanPath(baseDir.absoluteFilePath(dir));
    }
    if (const YamlNode* n = root.find("window")) {
        if (!parseSize(*n, &out->windowSize, &msg)) return fail(msg);
    }
    if (const YamlNode* n = root.find("appearance")) {
        if (!scalarString(*n, &out->appearance, &msg)) return fail(msg);
        if (out->appearance != QLatin1String("light") && out->appearance != QLatin1String("dark") &&
            out->appearance != QLatin1String("system"))
            return fail(lineError(n->line, QStringLiteral("'appearance' must be light, dark or system")));
    }
    if (const YamlNode* n = root.find("scale")) {
        if (!scalarNumber(*n, &out->scale, &msg)) return fail(msg);
        if (out->scale <= 0) return fail(lineError(n->line, QStringLiteral("'scale' must be positive")));
    }
    if (const YamlNode* n = root.find("settle")) {
        double ms = 0;
        if (!scalarNumber(*n, &ms, &msg)) return fail(msg);
        out->settleMs = static_cast<int>(ms);
    }
    const YamlNode* shots = root.find("shots");
    if (!shots || !shots->isSeq() || shots->seq.empty()) return fail(QStringLiteral("'shots' must be a non-empty list"));
    for (const YamlNode& item : shots->seq) {
        Shot shot;
        if (!parseShot(item, baseDir, *out, &shot, &msg)) return fail(msg);
        for (const Shot& other : out->shots) {
            if (other.name == shot.name)
                return fail(lineError(shot.line, QStringLiteral("duplicate shot name '%1'").arg(shot.name)));
        }
        out->shots.append(shot);
    }
    return true;
}
