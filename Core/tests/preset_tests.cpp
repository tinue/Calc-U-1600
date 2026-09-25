// Headless tests for the .pc1500 preset parser (PresetFile.cpp) -- step
// parsing (key:/type:/wait:/trace:), inline-comment handling, and plotter
// field. Note the `type:` payload is deliberately NOT comment-stripped: a
// mid-line `#` is valid BASIC (`OPEN ... AS #1`), so a `# note` after a
// `type:` step is typed literally -- comments belong on their own line.
// Same no-framework, assert-and-tally style as the other Core test files.
//
// Build & run: see tools/run_tests.sh

#include <cstdio>
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
        "memory-expansion-1:   # slot 1\n"
        "  - modulespec: CE-1600M   # 32 KB\n"
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

// A `type:` value wrapped entirely in matching quotes is still unwrapped
// (upstream pc1500preset quotes colon-bearing type steps); a `#` inside
// survives, and with nothing after the closing quote it round-trips clean.
void test_type_step_fully_quoted_is_unwrapped() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - type: '10 A$=\"x #1\"'\n",
        &p, &err));
    CHECK(nthType(p, 0) == "10 A$=\"x #1\"");
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
// file lands is the trace directory's concern, mirroring Calc-U-59.
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

// A negative explicit wait value is rejected (points at the parameterless form).
void test_wait_step_rejects_negative_value() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1500A\nkeys:\n  - wait: -5\n", &p, &err));
    CHECK(err.find("negative") != std::string::npos);
}

// `- saveas: s1:<name>` / `s2:<name>` / `floppy:<name>` -- all three forms
// parse on a PC-1600 preset, with the target/name split correctly.
void test_saveas_step_parses_all_targets_pc1600() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600\n"
        "keys:\n"
        "  - saveas: s1:First Card\n"
        "  - saveas: s2:CE-1601M - Progs\n"
        "  - saveas: floppy:Progs\n",
        &p, &err));
    CHECK(!p.sections.empty());
    const auto& steps = p.sections[0].keys;
    CHECK(steps.size() == 3);
    CHECK(steps[0].kind == PresetStep::Kind::SaveAs);
    CHECK(steps[0].saveAsTarget == PresetStep::SaveAsTarget::S1);
    CHECK(steps[0].text == "First Card");
    CHECK(steps[1].saveAsTarget == PresetStep::SaveAsTarget::S2);
    CHECK(steps[1].text == "CE-1601M - Progs");
    CHECK(steps[2].saveAsTarget == PresetStep::SaveAsTarget::Floppy);
    CHECK(steps[2].text == "Progs");
}

// A missing target/name colon, an unknown target, an empty name, and a
// quoted name are all rejected.
void test_saveas_step_rejects_malformed_forms() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1600\nkeys:\n  - saveas: NoColonHere\n", &p, &err));
    CHECK(!parse("model: PC-1600\nkeys:\n  - saveas: s3:Name\n", &p, &err));
    CHECK(err.find("target") != std::string::npos);
    CHECK(!parse("model: PC-1600\nkeys:\n  - saveas: s1:\n", &p, &err));
    CHECK(err.find("needs a name") != std::string::npos);
    CHECK(!parse("model: PC-1600\nkeys:\n  - saveas: 's1:bad \"name\"'\n", &p, &err));
}

// `s1:` is valid on a PC-1500/1500A preset (its one expansion slot); `s2:`
// and `floppy:` are PC-1600 only.
void test_saveas_step_pc1500_scope() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1500A\nkeys:\n  - saveas: s1:My Card\n", &p, &err));
    CHECK(!parse("model: PC-1500A\nkeys:\n  - saveas: s2:My Card\n", &p, &err));
    CHECK(err.find("PC-1600") != std::string::npos);
    CHECK(!parse("model: PC-1500A\nkeys:\n  - saveas: floppy:My Disk\n", &p, &err));
    CHECK(err.find("PC-1600") != std::string::npos);
}

void test_plotter_ce1600p_parses_and_normalizes() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-1600P\n", &p, &err));
    CHECK(p.plotter == "ce1600p");

    PresetFile p2;
    CHECK(parse("model: PC-1600\nplotter: ce1600p\n", &p2, &err));
    CHECK(p2.plotter == "ce1600p");
}

void test_plotter_none_clears() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: none\n", &p, &err));
    CHECK(p.plotter.empty());
}

void test_plotter_ce150_parses_but_is_left_for_the_loader() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: ce150\n", &p, &err));
    CHECK(p.plotter == "ce150");  // applyPC1600Preset rejects it as not-yet-emulated
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
    CHECK(!parse("model: PC-1500A\nplotter: ce1600p\n", &p, &err));
    CHECK(err.find("PC-1600 device") != std::string::npos);
}

void test_plotter_ce150_on_pc1500_parses() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1500A\nplotter: ce150\n", &p, &err));
    CHECK(p.plotter == "ce150");  // applyPC1500Preset attaches it before reset()
    PresetFile p2;
    CHECK(parse("model: PC-1500\nplotter: CE-150\n", &p2, &err));
    CHECK(p2.plotter == "ce150"); // spelling normalised
}

void test_plotter_absent_leaves_field_empty() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\n", &p, &err));
    CHECK(p.plotter.empty());
}

// ── memory-expansion / general parser rules (moved from ce155_tests.cpp) ──

// The built-in `- module: <name>` form is gone -- modules come from
// definition files (`modulespec:` / `modulespecfile:`).
void test_preset_parser_rejects_module_form() {
    PresetFile preset;
    std::string error;
    CHECK(!parse(
        "model: PC-1500\n"
        "memory-expansion:\n"
        "  - module: ce155\n",
        &preset, &error));
    CHECK(error.find("modulespec") != std::string::npos);
}

void test_preset_parser_rejects_extra_field() {
    PresetFile preset;
    std::string error;
    CHECK(!parse(
        "model: PC-1500\n"
        "memory-expansion:\n"
        "  - modulespec: CE-155\n"
        "    address: 0x0000\n",
        &preset, &error));
}

void test_preset_parser_rejects_second_item() {
    PresetFile preset;
    std::string error;
    CHECK(!parse(
        "model: PC-1500\n"
        "memory-expansion:\n"
        "  - modulespec: CE-155\n"
        "  - modulespec: CE-155\n",
        &preset, &error));
}

void test_preset_parser_no_memory_expansion_key_is_unaffected() {
    // Regression: a preset that never mentions memory-expansion at all
    // still parses fine and names no module.
    PresetFile preset;
    std::string error;
    CHECK(parse("model: PC-1500A\n", &preset, &error));
    CHECK(preset.memoryExpansionModuleSpecName.empty());
    CHECK(preset.memoryExpansionModuleSpecFile.empty());
}

void test_preset_parser_unquotes_single_and_double_quoted_values() {
    // A `type:` step whose own text contains ": " (e.g. typing a BASIC
    // PRINT of a string literal) needs quoting under pc1500preset's own
    // full YAML parser, or its embedded colon would be misread as a second
    // key/value split there -- this loader's first-colon-only split never
    // needed that, but preset files are shared with that upstream loader,
    // so it still has to strip whichever quote style shows up rather than
    // typing the literal quote characters into the emulator (see
    // ce163_bankswrm.pc1500a for a real preset that hit this).
    PresetFile preset;
    std::string error;
    CHECK(parse(
        "model: PC-1500A\n"
        "keys:\n"
        "  - type: '10 PRINT \"Bank: 7\"'\n"
        "  - type: \"20 PRINT 'Bank: 0'\"\n"
        "  - type: X=0\n",
        &preset, &error));
    CHECK(preset.sections.size() == 1);
    CHECK(preset.sections[0].kind == PresetSection::Kind::Keys);
    CHECK(preset.sections[0].keys.size() == 3);
    CHECK(preset.sections[0].keys[0].text == "10 PRINT \"Bank: 7\"");
    CHECK(preset.sections[0].keys[1].text == "20 PRINT 'Bank: 0'");
    CHECK(preset.sections[0].keys[2].text == "X=0"); // unquoted values still pass through unchanged
}

void test_preset_parser_sequential_keys_and_program_blocks() {
    // The fork from the upstream format (PresetFile.hpp's top-of-file
    // comment): a preset is an ORDERED sequence of `keys:`/`program:`
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
        "  format: binary\n"
        "  path: memtest_bank.bin\n"
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
        if (!parse("model: PC-1500A\nprogram:\n  format: binary\n  path: x.bin\n  address: " + value + "\n",
                   &preset, &error))
            return false;
        *out = preset.sections[0].program.address;
        return true;
    };
    uint16_t a = 0;
    CHECK(address("0x7C01", &a) && a == 0x7C01);
    CHECK(address("7c01", &a) && a == 0x7C01);
    CHECK(address("&4100", &a) && a == 0x4100);
    CHECK(address("$4100", &a) && a == 0x4100);
    CHECK(!address("14100", &a)); // > &FFFF, used to wrap to &4100
    CHECK(!address("41zz", &a));  // trailing junk, used to parse as &41
}

void test_preset_parser_rejects_old_pre_post_load_keys() {
    // pre-load-keys/post-load-keys are no longer recognized at all -- both
    // are now a single, repeatable 'keys:' block name.
    PresetFile preset;
    std::string error;
    CHECK(!parse("model: PC-1500A\npre-load-keys:\n  - key: cl\n", &preset, &error));
    CHECK(!parse("model: PC-1500A\npost-load-keys:\n  - key: cl\n", &preset, &error));
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
    // is now an error (it used to be silently ignored).
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

void test_preset_parser_firmware_key_is_gone() {
    // The old `firmware:` key is a parse error that points at the new syntax.
    PresetFile preset;
    std::string error;
    CHECK(!parse("model: PC-1500\nfirmware: A03\n", &preset, &error));
    CHECK(error.find("model: PC-1500:A01") != std::string::npos);
    CHECK(!parse("model: PC-1600\nfirmware: old\n", &preset, &error));
}

void test_preset_parser_plotter_ce1600p_rom() {
    // `plotter: ce1600p:old` -- independent of the PC-1600's own ROM.
    PresetFile preset;
    std::string error;
    CHECK(parse("model: PC-1600\nplotter: ce1600p\n", &preset, &error));
    CHECK(preset.plotter == "ce1600p");
    CHECK(preset.ce1600pRomVariant == "new");
    CHECK(parse("model: PC-1600\nplotter: ce1600p:old\n", &preset, &error));
    CHECK(preset.plotter == "ce1600p");
    CHECK(preset.ce1600pRomVariant == "old");
    CHECK(preset.romVariant == "new");
    CHECK(parse("model: PC-1600:old\nplotter: CE-1600P:New\n", &preset, &error));
    CHECK(preset.ce1600pRomVariant == "new");
    CHECK(preset.romVariant == "old");
    CHECK(!parse("model: PC-1600\nplotter: ce1600p:A04\n", &preset, &error));
    CHECK(!parse("model: PC-1600\nplotter: ce1600p:\n", &preset, &error));
    // Only the CE-1600P has a ROM choice.
    CHECK(!parse("model: PC-1600\nplotter: ce150:old\n", &preset, &error));
    CHECK(error.find("CE-1600P") != std::string::npos);
    CHECK(!parse("model: PC-1500A\nplotter: ce150:new\n", &preset, &error));
    // Still a PC-1600 device.
    CHECK(!parse("model: PC-1500A\nplotter: ce1600p:old\n", &preset, &error));
}

} // namespace

int run_preset_tests() {
    test_preset_parser_model_rom_pc1600();
    test_preset_parser_firmware_key_is_gone();
    test_preset_parser_plotter_ce1600p_rom();
    test_preset_parser_model_rom_pc1500a_is_a04_only();
    test_preset_parser_rejects_module_form();
    test_preset_parser_rejects_extra_field();
    test_preset_parser_rejects_second_item();
    test_preset_parser_no_memory_expansion_key_is_unaffected();
    test_preset_parser_unquotes_single_and_double_quoted_values();
    test_preset_parser_sequential_keys_and_program_blocks();
    test_preset_parser_rejects_old_pre_post_load_keys();
    test_preset_parser_program_address_forms();
    test_preset_parser_model_rom_pc1500();
    test_inline_comment_stripped_from_key_step_only();
    test_comment_after_block_key();
    test_hash_without_leading_space_is_kept();
    test_type_step_keeps_space_hash();
    test_type_step_fully_quoted_is_unwrapped();
    test_key_step_with_non_key_value_is_rejected();
    test_key_step_with_real_key_names_ok();
    test_trace_step_start_and_stop();
    test_trace_step_rejects_path_separator();
    test_trace_step_requires_a_value();
    test_wait_step_value_and_parameterless();
    test_wait_parameterless_pc1600();
    test_syncclock_step();
    test_saveas_step_parses_all_targets_pc1600();
    test_saveas_step_rejects_malformed_forms();
    test_saveas_step_pc1500_scope();
    test_wait_step_rejects_negative_value();
    test_plotter_ce1600p_parses_and_normalizes();
    test_plotter_none_clears();
    test_plotter_ce150_parses_but_is_left_for_the_loader();
    test_plotter_unknown_value_is_rejected();
    test_plotter_ce1600p_on_pc1500_is_rejected();
    test_plotter_ce150_on_pc1500_parses();
    test_plotter_absent_leaves_field_empty();

    std::printf("preset_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
