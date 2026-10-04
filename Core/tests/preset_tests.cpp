// Headless tests for the preset parser (PresetFile.cpp) -- step parsing
// (key:/type:/wait:/trace:), inline-comment handling, the naming rules
// (product names, slots, numbers, files) and the refusal of old forms. Note
// the `type:` payload is typed exactly as written: a mid-line `#` is valid
// BASIC (`OPEN ... AS #1`) and quotes are typed, so a `# note` after a
// `type:` step is typed too -- comments belong on their own line.
// Same no-framework, assert-and-tally style as the other Core test files.
//
// Build & run: see tools/run_tests.sh

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../Preset/PresetFile.hpp"
#include "PresetTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

bool parse(const std::string& yaml, PresetFile* out, std::string* err) {
    return parsePresetString(yaml, "/tmp/preset_tests_scratch.pc1500", out, err);
}

// Returns the value of the Nth `type:` step across all sections, or "" .
std::string nthType(const PresetFile& p, int n) {
    int seen = 0;
    for (const auto& s : p.sections) {
        if (s.kind != PresetSection::Kind::Keys) continue;
        for (const auto& step : s.keys) {
            if (step.kind != PresetStep::Kind::Type) continue;
            if (seen++ == n) return step.text;
        }
    }
    return "";
}

// A trailing `# comment` is stripped from a `key:` step, but a `type:`
// step's payload is keystroke-literal -- NOT comment-stripped -- because a
// mid-line `#` is BASIC (`... AS #1`), never a comment. See PresetFile.cpp's
// stripInlineComment / parseStepList `type` branch.
void test_inline_comment_stripped_from_key_step_only() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - key: mode   # switch to RUN\n"
        "  - type: RUN\n"
        "  - type: CALL &4100,X   # now kept verbatim\n",
        &p, &err));
    CHECK(nthType(p, 0) == "RUN");
    CHECK(nthType(p, 1) == "CALL &4100,X   # now kept verbatim");
}

// A comment right after a block key (`keys:   # note`) leaves the key's
// inline value empty, so it still opens its block.
void test_comment_after_block_key() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600   # the PC-1600\n"
        "slot-1: CE-1600M   # 32 KB\n"
        "keys:   # after the boot\n"
        "  - key: mode\n",
        &p, &err));
    if (!err.empty()) std::fprintf(stderr, "  parse error: %s\n", err.c_str());
    CHECK(p.model == "PC-1600");
    CHECK(p.sections.size() == 1);
}

void test_hash_without_leading_space_is_kept() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - type: PRINT#2,A#B\n",
        &p, &err));
    CHECK(nthType(p, 0) == "PRINT#2,A#B");
}

// A space-separated `#` in real BASIC (`OPEN ... AS #1`, `CLOSE #1`) is
// not a comment marker; the whole line must survive intact.
void test_type_step_keeps_space_hash() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600\n"
        "keys:\n"
        "  - type: OPEN\"S2:CW.CFG\" FOR OUTPUT AS #1\n"
        "  - type: CLOSE #1\n",
        &p, &err));
    CHECK(nthType(p, 0) == "OPEN\"S2:CW.CFG\" FOR OUTPUT AS #1");
    CHECK(nthType(p, 1) == "CLOSE #1");
}

// A `type:` value is typed as written: quotes around the whole value are
// typed too (`"SAVE LOAD"` is a BASIC string), and a `#` inside survives.
void test_type_step_is_literal() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - type: \"SAVE LOAD\"\n"
        "  - type: '10 A$=\"x #1\"'\n"
        "  - type: 10 PRINT \"Bank: 7\"\n",
        &p, &err));
    CHECK(nthType(p, 0) == "\"SAVE LOAD\"");
    CHECK(nthType(p, 1) == "'10 A$=\"x #1\"'");
    CHECK(nthType(p, 2) == "10 PRINT \"Bank: 7\"");
}

// A `key:` step whose value is not a single known key name is almost always
// a `type:` step written with the wrong verb (`key: X=34`, `key: CALL ...`).
// tapKey() would just press nothing and the load would carry on silently --
// so the parser rejects it up front and names the fix.
void test_key_step_with_non_key_value_is_rejected() {
    PresetFile p;
    std::string err;
    CHECK(!parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - key: mode\n"
        "  - key: X=34\n",
        &p, &err));
    CHECK(err.find("X=34") != std::string::npos);
    CHECK(err.find("type:") != std::string::npos);
}

// ...but real single keys (and the special break/on) still parse fine.
void test_key_step_with_real_key_names_ok() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - key: cl\n"
        "  - key: mode\n"
        "  - key: enter\n"
        "  - key: 7\n"
        "  - key: break\n",
        &p, &err));
}

// Returns the Nth `trace:` step's `text` across all sections, or a
// sentinel "<none>" if there is no Nth one (so an empty `text`, which is
// the legitimate "stop" form, stays distinguishable).
std::string nthTrace(const PresetFile& p, int n) {
    int seen = 0;
    for (const auto& s : p.sections) {
        if (s.kind != PresetSection::Kind::Keys) continue;
        for (const auto& step : s.keys) {
            if (step.kind != PresetStep::Kind::Trace) continue;
            if (seen++ == n) return step.text;
        }
    }
    return "<none>";
}

// `- trace: <filename>` parses to a Trace step carrying the filename;
// `- trace: off` (any case) parses to a Trace step with empty text (stop).
void test_trace_step_start_and_stop() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - trace: run1.bin\n"
        "  - wait: 0.1\n"
        "  - trace: off\n"
        "  - trace: OFF\n",
        &p, &err));
    CHECK(nthTrace(p, 0) == "run1.bin");
    CHECK(nthTrace(p, 1) == "");        // "off"
    CHECK(nthTrace(p, 2) == "");        // "OFF" -- case-insensitive
    CHECK(nthTrace(p, 3) == "<none>");
}

// A trace filename with a path separator is rejected up front -- WHERE the
// file lands is the trace directory's concern.
void test_trace_step_rejects_path_separator() {
    PresetFile p;
    std::string err;
    CHECK(!parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - trace: sub/run.bin\n",
        &p, &err));
    CHECK(err.find("path separator") != std::string::npos);

    std::string err2;
    PresetFile p2;
    CHECK(!parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - trace: a\\b.bin\n",
        &p2, &err2));
    CHECK(err2.find("path separator") != std::string::npos);
}

// A bare `- trace:` with no value hits the generic malformed-step path.
void test_trace_step_requires_a_value() {
    PresetFile p;
    std::string err;
    CHECK(!parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - trace:\n",
        &p, &err));
    CHECK(err.find("malformed step") != std::string::npos);
}

// Helper: the Nth Wait step's waitSeconds, or a large positive sentinel if
// there is no Nth Wait step.
double nthWait(const PresetFile& p, int n) {
    int seen = 0;
    for (const auto& s : p.sections) {
        if (s.kind != PresetSection::Kind::Keys) continue;
        for (const auto& step : s.keys) {
            if (step.kind != PresetStep::Kind::Wait) continue;
            if (seen++ == n) return step.waitSeconds;
        }
    }
    return 1e9;
}

// `- wait: N` carries N seconds; `- wait:` with no value is the
// "wait until idle" sentinel (negative waitSeconds).
void test_wait_step_value_and_parameterless() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - wait: 2.5\n"
        "  - type: RUN\n"
        "  - wait:\n",
        &p, &err));
    CHECK(nthWait(p, 0) == 2.5);
    CHECK(nthWait(p, 1) < 0);   // parameterless -> sentinel
    CHECK(nthWait(p, 1) == PresetStep::kWaitUntilIdle);
}

// Parameterless `- wait:` is valid for a PC-1600 preset too.
void test_wait_parameterless_pc1600() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nkeys:\n  - type: RUN\n  - wait:\n", &p, &err));
    CHECK(nthWait(p, 0) < 0);
}

// `- syncclock:` takes no value; any value is rejected.
void test_syncclock_step() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nkeys:\n  - wait: 1\n  - syncclock:\n", &p, &err));
    CHECK(!p.sections.empty() && p.sections[0].keys.size() == 2 &&
          p.sections[0].keys[1].kind == PresetStep::Kind::SyncClock);
    PresetFile bad;
    CHECK(!parse("model: PC-1600\nkeys:\n  - syncclock: now\n", &bad, &err));
    CHECK(err.find("syncclock") != std::string::npos);
}

// A wait value is a number and nothing else (`1s` is refused, not read as 1).
void test_wait_step_rejects_trailing_text() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1500A\nkeys:\n  - wait: 1s\n", &p, &err));
    CHECK(err.find("invalid 'wait' value '1s'") != std::string::npos);
    CHECK(!parse("model: PC-1500A\nkeys:\n  - wait: soon\n", &p, &err));
    CHECK(parse("model: PC-1500A\nkeys:\n  - wait: 0.5\n", &p, &err));
}

// A negative explicit wait value is rejected (points at the parameterless form).
void test_wait_step_rejects_negative_value() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1500A\nkeys:\n  - wait: -5\n", &p, &err));
    CHECK(err.find("negative") != std::string::npos);
}

// `- saveas: template|live slot-1:<name>` / `slot-2:` / `floppy:` -- all three
// targets parse on a PC-1600 preset, with the kind, target and name split
// correctly.
void test_saveas_step_parses_all_targets_pc1600() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600\n"
        "keys:\n"
        "  - saveas: live slot-1:First Card\n"
        "  - saveas: template slot-2:CE-1601M - Progs\n"
        "  - saveas: LIVE floppy:Progs\n",
        &p, &err));
    CHECK(!p.sections.empty());
    const auto& steps = p.sections[0].keys;
    CHECK(steps.size() == 3);
    CHECK(steps[0].kind == PresetStep::Kind::SaveAs);
    CHECK(steps[0].saveAsTarget == PresetStep::SaveAsTarget::Slot1);
    CHECK(steps[0].text == "First Card");
    CHECK(!steps[0].saveAsTemplate);
    CHECK(steps[0].saveAsPath.empty());
    CHECK(steps[1].saveAsTarget == PresetStep::SaveAsTarget::Slot2);
    CHECK(steps[1].text == "CE-1601M - Progs");
    CHECK(steps[1].saveAsTemplate);
    CHECK(steps[2].saveAsTarget == PresetStep::SaveAsTarget::Floppy);
    CHECK(steps[2].text == "Progs");
    CHECK(!steps[2].saveAsTemplate);
}

// `file:<path>` is resolved against the preset's directory, and the name is
// the file name minus the target's suffix.
void test_saveas_file_form_resolves_against_the_preset() {
    std::filesystem::create_directories("/tmp/calcu_preset_dir");
    PresetFile p;
    std::string err;
    CHECK(parsePresetString("model: PC-1600\n"
                            "keys:\n"
                            "  - saveas: template slot-2:file:CE-1601M - Progs.card.yaml\n"
                            "  - saveas: live floppy:file:disks/Progs.floppy.yaml\n",
                            "/tmp/calcu_preset_dir/make.pc1600", &p, &err));
    CHECK(p.sections.size() == 1 && p.sections[0].keys.size() == 2);
    if (p.sections.size() == 1 && p.sections[0].keys.size() == 2) {
        const auto& card = p.sections[0].keys[0];
        CHECK(card.text == "CE-1601M - Progs");
        CHECK(card.saveAsPath == "/tmp/calcu_preset_dir/CE-1601M - Progs.card.yaml");
        CHECK(card.saveAsTemplate);
        const auto& disk = p.sections[0].keys[1];
        CHECK(disk.text == "Progs");
        CHECK(disk.saveAsPath == "/tmp/calcu_preset_dir/disks/Progs.floppy.yaml");
        CHECK(!disk.saveAsTemplate);
    }
}

// A missing template/live word, a missing target/name colon, an unknown
// target, an empty name, a quoted name, and a `file:` with the wrong suffix
// are all rejected.
void test_saveas_step_rejects_malformed_forms() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1600\nkeys:\n  - saveas: s1:Name\n", &p, &err));
    CHECK(err.find("'template' or 'live'") != std::string::npos);
    CHECK(!parse("model: PC-1600\nkeys:\n  - saveas: live NoColonHere\n", &p, &err));
    CHECK(!parse("model: PC-1600\nkeys:\n  - saveas: live s1:Name\n", &p, &err));
    CHECK(err.find("target must be 'slot-1'") != std::string::npos);
    CHECK(!parse("model: PC-1600\nkeys:\n  - saveas: live slot-1:\n", &p, &err));
    CHECK(err.find("needs a name") != std::string::npos);
    CHECK(!parse("model: PC-1600\nkeys:\n  - saveas: 'live slot-1:bad \"name\"'\n", &p, &err));
    CHECK(!parse("model: PC-1600\nkeys:\n  - saveas: template slot-2:file:Card.floppy.yaml\n", &p, &err));
    CHECK(err.find(".card.yaml") != std::string::npos);
    CHECK(!parse("model: PC-1600\nkeys:\n  - saveas: template floppy:file:.floppy.yaml\n", &p, &err));
}

// `slot-1` is valid on a PC-1500/1500A preset (its one slot); `slot-2`
// and `floppy` are PC-1600 only.
void test_saveas_step_pc1500_scope() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1500A\nkeys:\n  - saveas: live slot-1:My Card\n", &p, &err));
    CHECK(!parse("model: PC-1500A\nkeys:\n  - saveas: live slot-2:My Card\n", &p, &err));
    CHECK(err.find("PC-1600") != std::string::npos);
    CHECK(!parse("model: PC-1500A\nkeys:\n  - saveas: live floppy:My Disk\n", &p, &err));
    CHECK(err.find("PC-1600") != std::string::npos);
}

// Sharp's spelling, in any case; the hyphen is part of the name.
void test_product_names_any_case_hyphen_required() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-1600P\n", &p, &err));
    CHECK(p.plotter == "ce1600p");
    CHECK(parse("model: pc-1600\nplotter: ce-1600p\n", &p, &err));
    CHECK(p.model == "PC-1600" && p.isPC1600() && p.plotter == "ce1600p");
    CHECK(parse("model: pc-1500a\ninterface: ce-158\n", &p, &err));
    CHECK(p.model == "PC-1500A" && p.interfaceName == "ce158");
    CHECK(!parse("model: PC-1600\nplotter: ce1600p\n", &p, &err));
    CHECK(err.find("expected CE-1600P or CE-150") != std::string::npos);
    CHECK(!parse("model: PC-1500A\ninterface: ce158\n", &p, &err));
    CHECK(!parse("model: PC1600\n", &p, &err));
    CHECK(err.find("unsupported model") != std::string::npos);
}

void test_plotter_ce150_on_pc1600_parses() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-150\n", &p, &err));
    CHECK(p.plotter == "ce150");  // on the LH5803 side, MODE 1
}

void test_plotter_unknown_value_is_rejected() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1600\nplotter: laserjet\n", &p, &err));
    CHECK(err.find("not a known plotter") != std::string::npos);
}

void test_plotter_ce1600p_on_pc1500_is_rejected() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1500A\nplotter: CE-1600P\n", &p, &err));
    CHECK(err.find("PC-1600 device") != std::string::npos);
}

void test_plotter_ce150_on_pc1500_parses() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1500A\nplotter: CE-150\n", &p, &err));
    CHECK(p.plotter == "ce150");  // applyPC1500Preset attaches it before reset()
    PresetFile p2;
    CHECK(parse("model: PC-1500\nplotter: CE-150\n", &p2, &err));
    CHECK(p2.plotter == "ce150");
}

void test_plotter_absent_leaves_field_empty() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\n", &p, &err));
    CHECK(p.plotter.empty());
}

// ── slots / general parser rules ──

// `slot-N: <name>` / `slot-N-file: <path>`, like `floppy:` / `floppy-file:`.
void test_slot_keys() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nslot-1: CE-1600M\nslot-2-file: cards/my.card.yaml\n", &p, &err));
    CHECK(p.slot1ModuleSpecName == "CE-1600M" && p.slot1ModuleSpecFile.empty());
    CHECK(p.slot2ModuleSpecFile == "/tmp/cards/my.card.yaml" && p.slot2ModuleSpecName.empty());
    CHECK(parse("model: PC-1500\nslot-1-file: ~/x.card.yaml\n", &p, &err));
    CHECK(p.slot1ModuleSpecFile == std::string(std::getenv("HOME")) + "/x.card.yaml");
    CHECK(!parse("model: PC-1600\nslot-1: CE-1600M\nslot-1-file: x.card.yaml\n", &p, &err));
    CHECK(err.find("only one of 'slot-1:'") != std::string::npos);
    CHECK(!parse("model: PC-1500A\nslot-2: CE-155\n", &p, &err));
    CHECK(err.find("PC-1600 only") != std::string::npos);
    CHECK(!parse("model: PC-1500\nslot-1:\n  - modulespec: CE-155\n", &p, &err));
}

// Old forms fail as invalid, without a hint at the new one (pre-1.0) --
// except `slot:` in a program block (docs/background/Decisions.md).
void test_old_forms_are_unrecognized() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1500\nmemory-expansion:\n  - modulespec: CE-155\n", &p, &err));
    CHECK(err.find("unrecognized field 'memory-expansion'") != std::string::npos);
    CHECK(!parse("model: PC-1600\nmemory-expansion-1:\n  - modulespec: CE-1600M\n", &p, &err));
    CHECK(!parse("model: PC-1500\nfirmware: A03\n", &p, &err));
    CHECK(err.find("unrecognized field 'firmware'") != std::string::npos);
    CHECK(!parse("model: PC-1500A\npre-load-keys:\n  - key: cl\n", &p, &err));
    CHECK(!parse("model: PC-1500A\npost-load-keys:\n  - key: cl\n", &p, &err));
    CHECK(!parse("model: PC-1500A\nrom-modules: x\n", &p, &err));
    CHECK(!parse("model: PC-1600\nplotter: none\n", &p, &err));
    CHECK(!parse("model: PC-1500\ninterface: off\n", &p, &err));
    CHECK(!parse("model: PC-1500A\nkeys:\n  - check: 0\n", &p, &err));
    CHECK(err.find("unrecognized step verb 'check'") != std::string::npos);
    CHECK(!parse("model: PC-1500A\nprogram:\n  format: binary\n  file: x.bin\n  address: 0x7C01\n", &p, &err));
    CHECK(err.find("unrecognized 'program' field 'format'") != std::string::npos);
    CHECK(!parse("model: PC-1500A\nprogram:\n  path: x.bin\n  address: 0x7C01\n", &p, &err));
    CHECK(!parse("model: PC-1500A\nbus-rom:\n  - file: x.bin\n    address: 0x8000\n    me1: true\n", &p, &err));
    CHECK(!parse("model: PC-1500A\nprogram:\n  slot: S0\n  file: x.bin\n", &p, &err));
    CHECK(err.find("'slot:' was removed") != std::string::npos);
}

void test_preset_parser_no_slot_key_is_unaffected() {
    // A preset that names no module leaves both slots empty.
    PresetFile preset;
    std::string error;
    CHECK(parse("model: PC-1500A\n", &preset, &error));
    CHECK(preset.slot1ModuleSpecName.empty());
    CHECK(preset.slot1ModuleSpecFile.empty());
}


void test_preset_parser_sequential_keys_and_program_blocks() {
    // The sequential-blocks rule (PresetFile.hpp's top-of-file comment):
    // a preset is an ORDERED sequence of `keys:`/`program:`
    // blocks, applied in file order -- not a fixed pre-load-keys/program/
    // post-load-keys triple. Two of each here, interleaved, must come back
    // as four sections in exactly this order.
    PresetFile preset;
    std::string error;
    CHECK(parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - type: CALL&4100\n"
        "program:\n"
        "  file: x.bin\n"
        "  address: 0x7C01\n"
        "keys:\n"
        "  - type: X$=\"3.8\"\n"
        "  - type: CALL&7C01,X$\n",
        &preset, &error));
    CHECK(preset.sections.size() == 3);
    CHECK(preset.sections[0].kind == PresetSection::Kind::Keys);
    CHECK(preset.sections[0].keys.size() == 1);
    CHECK(preset.sections[0].keys[0].text == "CALL&4100");
    CHECK(preset.sections[1].kind == PresetSection::Kind::Program);
    CHECK(preset.sections[1].program.address == 0x7C01);
    CHECK(preset.sections[2].kind == PresetSection::Kind::Keys);
    CHECK(preset.sections[2].keys.size() == 2);
    CHECK(preset.sections[2].keys[0].text == "X$=\"3.8\"");
    CHECK(preset.sections[2].keys[1].text == "CALL&7C01,X$");
}

void test_preset_parser_program_address_forms() {
    auto address = [](const std::string& value, uint16_t* out) {
        PresetFile preset;
        std::string error;
        if (!parse("model: PC-1500A\nprogram:\n  file: x.bin\n  address: " + value + "\n",
                   &preset, &error))
            return false;
        *out = preset.sections[0].program.address;
        return true;
    };
    uint16_t a = 0;
    CHECK(address("0x7C01", &a) && a == 0x7C01);
    CHECK(address("&4100", &a) && a == 0x4100);
    CHECK(address("$4100", &a) && a == 0x4100);
    CHECK(address("4100", &a) && a == 4100);  // bare = decimal, as everywhere
    CHECK(!address("7c01", &a));              // hex needs its prefix
    CHECK(!address("&14100", &a));            // > &FFFF
    CHECK(!address("&41zz", &a));             // trailing junk
    CHECK(!address("65536", &a));
}

// `program: file:` is loaded by what the file holds; `text: |` and
// `typed: true` type a listing in.
void test_program_kind_follows_the_file() {
    std::ofstream("/tmp/preset_tests_data.txt") << "just some words\n";
    PresetFile p;
    std::string err;
    p = PresetFile{};
    CHECK(parse("model: PC-1600\nprogram:\n  file: x.bas\n", &p, &err));
    CHECK(p.sections.size() == 1 && p.sections[0].program.format == PresetProgram::Format::BasicBinary);
    p = PresetFile{};
    CHECK(parse("model: PC-1600\nprogram:\n  file: x.bas\n  typed: true\n", &p, &err));
    CHECK(p.sections.size() == 1 && p.sections[0].program.format == PresetProgram::Format::BasicText &&
          p.sections[0].program.text == "10 PRINT 1\n");
    p = PresetFile{};
    CHECK(parse("model: PC-1600\nprogram:\n  file: x.bin\n  address: &C0C5\n", &p, &err));
    CHECK(p.sections.size() == 1 && p.sections[0].program.format == PresetProgram::Format::Binary);
    p = PresetFile{};
    CHECK(parse("model: PC-1600\nprogram:\n  text: |\n    10 END\n", &p, &err));
    CHECK(p.sections.size() == 1 && p.sections[0].program.format == PresetProgram::Format::BasicText);
    // Mismatched fields.
    CHECK(!parse("model: PC-1600\nprogram:\n  file: x.bin\n  typed: true\n", &p, &err));
    CHECK(err.find("machine code") != std::string::npos);
    CHECK(!parse("model: PC-1600\nprogram:\n  file: x.bas\n  address: &C0C5\n", &p, &err));
    CHECK(err.find("is BASIC") != std::string::npos);
    CHECK(!parse("model: PC-1600\nprogram:\n  file: x.bas\n  length: 8\n", &p, &err));
    CHECK(!parse("model: PC-1600\nprogram:\n  text: |\n    10 END\n  address: 0\n", &p, &err));
    CHECK(!parse("model: PC-1600\nprogram:\n  file: x.bas\n  text: |\n    10 END\n", &p, &err));
    CHECK(!parse("model: PC-1600\nprogram:\n  typed: true\n", &p, &err));
    CHECK(err.find("either 'file:' or 'text: |'") != std::string::npos);
    CHECK(!parse("model: PC-1600\nprogram:\n  file: x.bas\n  typed: yes\n", &p, &err));
    CHECK(!parse("model: PC-1600\nprogram:\n  file: preset_tests_data.txt\n", &p, &err));
    CHECK(err.find("not a program") != std::string::npos);
    CHECK(!parse("model: PC-1600\nprogram:\n  file: missing.bin\n", &p, &err));
    CHECK(err.find("could not open") != std::string::npos);
}


void test_preset_parser_model_rom_pc1500() {
    // `model: PC-1500:A03` -- the ROM rides on the model. `model` itself
    // stays the bare name; the revision goes into romVariant.
    PresetFile preset;
    std::string error;
    CHECK(parse("model: PC-1500:A03\n", &preset, &error));
    CHECK(preset.model == "PC-1500");
    CHECK(preset.romVariant == "A03");
    CHECK(parse("model: PC-1500:a01\n", &preset, &error));  // case-insensitive
    CHECK(preset.romVariant == "A01");
    CHECK(parse("model: PC-1500\n", &preset, &error));      // default
    CHECK(preset.romVariant == "A04");
    CHECK(!parse("model: PC-1500:A02\n", &preset, &error));
    CHECK(error.find("A01") != std::string::npos);
    CHECK(!parse("model: PC-1500:\n", &preset, &error));
}

void test_preset_parser_model_rom_pc1500a_is_a04_only() {
    // PC-1500A can only run A04: no suffix or A04 are fine, anything else
    // is an error.
    PresetFile preset;
    std::string error;
    CHECK(parse("model: PC-1500A\n", &preset, &error));
    CHECK(preset.romVariant == "A04");
    CHECK(parse("model: PC-1500A:A04\n", &preset, &error));
    CHECK(preset.romVariant == "A04");
    CHECK(!parse("model: PC-1500A:A01\n", &preset, &error));
    CHECK(error.find("A04") != std::string::npos);
}

void test_preset_parser_model_rom_pc1600() {
    // PC-1600: `model: PC-1600:new|old` selects the calculator ROM version.
    PresetFile preset;
    std::string error;
    CHECK(parse("model: PC-1600\n", &preset, &error));
    CHECK(preset.romVariant == "new");
    CHECK(parse("model: PC-1600:old\n", &preset, &error));
    CHECK(preset.model == "PC-1600");
    CHECK(preset.isPC1600());
    CHECK(preset.romVariant == "old");
    CHECK(parse("model: PC-1600:new\n", &preset, &error));
    CHECK(preset.romVariant == "new");
    CHECK(!parse("model: PC-1600:A04\n", &preset, &error));
    CHECK(error.find("new") != std::string::npos);
}


void test_preset_parser_plotter_ce1600p_rom() {
    // `plotter: CE-1600P:old` -- independent of the PC-1600's own ROM.
    PresetFile preset;
    std::string error;
    CHECK(parse("model: PC-1600\nplotter: CE-1600P\n", &preset, &error));
    CHECK(preset.plotter == "ce1600p");
    CHECK(preset.ce1600pRomVariant == "new");
    CHECK(parse("model: PC-1600\nplotter: CE-1600P:old\n", &preset, &error));
    CHECK(preset.plotter == "ce1600p");
    CHECK(preset.ce1600pRomVariant == "old");
    CHECK(preset.romVariant == "new");
    CHECK(parse("model: PC-1600:old\nplotter: CE-1600P:New\n", &preset, &error));
    CHECK(preset.ce1600pRomVariant == "new");
    CHECK(preset.romVariant == "old");
    CHECK(!parse("model: PC-1600\nplotter: CE-1600P:A04\n", &preset, &error));
    CHECK(!parse("model: PC-1600\nplotter: CE-1600P:\n", &preset, &error));
    // Only the CE-1600P has a ROM choice.
    CHECK(!parse("model: PC-1600\nplotter: CE-150:old\n", &preset, &error));
    CHECK(error.find("CE-1600P") != std::string::npos);
    CHECK(!parse("model: PC-1500A\nplotter: CE-150:new\n", &preset, &error));
    // Still a PC-1600 device.
    CHECK(!parse("model: PC-1500A\nplotter: CE-1600P:old\n", &preset, &error));
}

} // namespace

// `debug:` -- the debugger's attach settings in a project preset: nested
// YAML, paths resolved against the preset's directory, the rest kept as is.
void test_debug_block_parses_and_resolves_paths() {
    PresetFile p;
    std::string err;
    const bool ok = parse("model: PC-1600\n"
                          "debug:\n"
                          "  stopOnEntry: true\n"
                          "  command: CALL &C0C5,1   # with an argument\n"
                          "\n"
                          "  program:\n"
                          "    bin: build/hello.bin\n"
                          "    listing: /abs/hello.lst\n"
                          "    entry: START\n"
                          "    symbols:\n"
                          "      - hello.sym\n"
                          "  listings:\n"
                          "    - rom/b7.lst\n"
                          "    - path: rom/b3.lst\n"
                          "      cpu: z80\n"
                          "      bank: 3\n"
                          "keys:\n"
                          "  - key: mode\n",
                          &p, &err);
    CHECK(ok);
    if (!ok) { std::fprintf(stderr, "  %s\n", err.c_str()); return; }
    CHECK(p.debug.isMap());
    CHECK(p.sections.size() == 1); // `keys:` after the block still parses
    std::string s;
    const YamlNode* program = p.debug.find("program");
    CHECK(program && program->find("bin")->asString(&s, &err) && s == "/tmp/build/hello.bin");
    CHECK(program && program->find("listing")->asString(&s, &err) && s == "/abs/hello.lst");
    CHECK(program && program->find("entry")->asString(&s, &err) && s == "START");
    CHECK(program && program->find("symbols")->seq.at(0).asString(&s, &err) && s == "/tmp/hello.sym");
    const YamlNode* listings = p.debug.find("listings");
    CHECK(listings && listings->seq.size() == 2);
    CHECK(listings && listings->seq[0].asString(&s, &err) && s == "/tmp/rom/b7.lst");
    CHECK(listings && listings->seq[1].find("path")->asString(&s, &err) && s == "/tmp/rom/b3.lst");
    long bank = 0;
    CHECK(listings && listings->seq[1].find("bank")->asInt(&bank, &err) && bank == 3);
    CHECK(p.debug.find("command")->asString(&s, &err) && s == "CALL &C0C5,1");
    bool stop = false;
    CHECK(p.debug.find("stopOnEntry")->asBool(&stop, &err) && stop);
}

void test_debug_block_absent_is_null() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\n", &p, &err));
    CHECK(p.debug.isNull());
}

void test_debug_block_rejects_unknown_keys_and_shapes() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1600\ndebug:\n  bogus: 1\n", &p, &err));
    CHECK(err.find("unknown key 'bogus'") != std::string::npos);
    CHECK(!parse("model: PC-1600\ndebug:\n  program:\n    listing: a.lst\n", &p, &err));
    CHECK(err.find("'bin'") != std::string::npos);
    CHECK(!parse("model: PC-1600\ndebug: yes\n", &p, &err));
    CHECK(!parse("model: PC-1600\ndebug:\n  program:\n    bin: a.bin\n    slot: S1\n", &p, &err));
    // Errors carry the file's line number.
    CHECK(!parse("model: PC-1600\ndebug:\n  listings:\n    - cpu: z80\n", &p, &err));
    CHECK(err.find("line 4") != std::string::npos);
}

// Numbers in `debug:` follow the preset's rule: `&`, `0x` or `$` is hex, a
// bare number decimal. Bare hex, and values out of range, are refused.
void test_debug_block_numbers_follow_the_preset_rule() {
    PresetFile p;
    std::string err;
    const auto program = [](const std::string& line) {
        return "model: PC-1600\ndebug:\n  program:\n    bin: a.bin\n    " + line + "\n";
    };
    CHECK(parse(program("address: 1234"), &p, &err));
    CHECK(parse(program("address: &C0C5"), &p, &err));
    CHECK(parse(program("address: 0x40C5"), &p, &err));
    CHECK(parse(program("address: $7C01"), &p, &err));
    CHECK(!parse(program("address: C0C5"), &p, &err));
    CHECK(err.find("line 5") != std::string::npos && err.find("'address'") != std::string::npos);
    CHECK(!parse(program("address: &10000"), &p, &err));
    CHECK(parse(program("bank: &7"), &p, &err));
    CHECK(parse(program("bank: 0x6"), &p, &err));
    CHECK(!parse(program("bank: 8"), &p, &err));
    CHECK(!parse(program("me: 2"), &p, &err));
    CHECK(parse(program("entry: START"), &p, &err)); // a symbol stays text
    CHECK(!parse("model: PC-1600\ndebug:\n  listings:\n    - path: a.lst\n      pv: x\n", &p, &err));
    CHECK(err.find("line 5") != std::string::npos);
}

// The other values are checked against the attach-key table too, with the
// line number, instead of reaching the debugger unchecked.
void test_debug_block_checks_choices_and_booleans() {
    PresetFile p;
    std::string err;
    const auto program = [](const std::string& line) {
        return "model: PC-1600\ndebug:\n  program:\n    bin: a.bin\n    " + line + "\n";
    };
    CHECK(parse(program("after: call"), &p, &err));
    CHECK(!parse(program("after: foo"), &p, &err));
    CHECK(err.find("line 5") != std::string::npos && err.find("stopOnEntry, call, none") != std::string::npos);
    CHECK(parse(program("cpu: lh5803"), &p, &err));
    CHECK(!parse(program("cpu: x86"), &p, &err));
    CHECK(!parse(program("cleanStart: maybe"), &p, &err));
    CHECK(!parse(program("cleanStart: \"false\""), &p, &err)); // would reach the debugger as text
    CHECK(parse("model: PC-1600\ndebug:\n  boot: debug\n", &p, &err));
    CHECK(!parse("model: PC-1600\ndebug:\n  boot: foo\n", &p, &err));
    CHECK(err.find("line 3") != std::string::npos);
    CHECK(!parse("model: PC-1600\ndebug:\n  reset: yes\n", &p, &err));
    CHECK(!parse("model: PC-1600\ndebug:\n  stopOnEntry: maybe\n", &p, &err));
    CHECK(!parse("model: PC-1600\ndebug:\n  symbols:\n    - path: a.sym\n      source: a.asm\n", &p, &err));
    CHECK(err.find("unknown key 'source'") != std::string::npos);
}

int run_preset_tests() {
    test_debug_block_parses_and_resolves_paths();
    test_debug_block_absent_is_null();
    test_debug_block_rejects_unknown_keys_and_shapes();
    test_debug_block_numbers_follow_the_preset_rule();
    test_debug_block_checks_choices_and_booleans();
    test_preset_parser_model_rom_pc1600();
    test_preset_parser_plotter_ce1600p_rom();
    test_preset_parser_model_rom_pc1500a_is_a04_only();
    test_preset_parser_no_slot_key_is_unaffected();
    test_preset_parser_sequential_keys_and_program_blocks();
    test_preset_parser_program_address_forms();
    test_preset_parser_model_rom_pc1500();
    test_inline_comment_stripped_from_key_step_only();
    test_comment_after_block_key();
    test_hash_without_leading_space_is_kept();
    test_type_step_keeps_space_hash();
    test_type_step_is_literal();
    test_key_step_with_non_key_value_is_rejected();
    test_key_step_with_real_key_names_ok();
    test_trace_step_start_and_stop();
    test_trace_step_rejects_path_separator();
    test_trace_step_requires_a_value();
    test_wait_step_value_and_parameterless();
    test_wait_parameterless_pc1600();
    test_syncclock_step();
    test_saveas_step_parses_all_targets_pc1600();
    test_saveas_file_form_resolves_against_the_preset();
    test_saveas_step_rejects_malformed_forms();
    test_saveas_step_pc1500_scope();
    test_wait_step_rejects_negative_value();
    test_wait_step_rejects_trailing_text();
    test_slot_keys();
    test_old_forms_are_unrecognized();
    test_program_kind_follows_the_file();
    test_product_names_any_case_hyphen_required();
    test_plotter_ce150_on_pc1600_parses();
    test_plotter_unknown_value_is_rejected();
    test_plotter_ce1600p_on_pc1500_is_rejected();
    test_plotter_ce150_on_pc1500_parses();
    test_plotter_absent_leaves_field_empty();

    std::printf("preset_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
