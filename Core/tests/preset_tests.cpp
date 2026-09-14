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

#include "../PC1500/PresetFile.hpp"
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
        "firmware: A04\n"
        "keys:\n"
        "  - key: mode   # switch to RUN\n"
        "  - type: RUN\n"
        "  - type: CALL &4100,X   # now kept verbatim\n",
        &p, &err));
    CHECK(nthType(p, 0) == "RUN");
    CHECK(nthType(p, 1) == "CALL &4100,X   # now kept verbatim");
}

void test_hash_without_leading_space_is_kept() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1500A\n"
        "firmware: A04\n"
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
        "firmware: A04\n"
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
        "firmware: A04\n"
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
        "firmware: A04\n"
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
        "firmware: A04\n"
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
        "firmware: A04\n"
        "keys:\n"
        "  - trace: sub/run.bin\n",
        &p, &err));
    CHECK(err.find("path separator") != std::string::npos);

    std::string err2;
    PresetFile p2;
    CHECK(!parse(
        "model: PC-1500A\n"
        "firmware: A04\n"
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
        "firmware: A04\n"
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
        "firmware: A04\n"
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

// A negative explicit wait value is rejected (points at the parameterless form).
void test_wait_step_rejects_negative_value() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1500A\nfirmware: A04\nkeys:\n  - wait: -5\n", &p, &err));
    CHECK(err.find("negative") != std::string::npos);
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
    CHECK(!parse("model: PC-1500A\nfirmware: A04\nplotter: ce1600p\n", &p, &err));
    CHECK(err.find("PC-1600 device") != std::string::npos);
}

void test_plotter_ce150_on_pc1500_parses() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1500A\nfirmware: A04\nplotter: ce150\n", &p, &err));
    CHECK(p.plotter == "ce150");  // applyPC1500Preset attaches it before reset()
    PresetFile p2;
    CHECK(parse("model: PC-1500\nfirmware: A04\nplotter: CE-150\n", &p2, &err));
    CHECK(p2.plotter == "ce150"); // spelling normalised
}

void test_plotter_absent_leaves_field_empty() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\n", &p, &err));
    CHECK(p.plotter.empty());
}

} // namespace

int run_preset_tests() {
    test_inline_comment_stripped_from_key_step_only();
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
