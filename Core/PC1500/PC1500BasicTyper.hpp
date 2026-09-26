#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../Basic/BasicLoadResults.hpp"

class PC1500Machine;

// ── Keystroke-injection primitives for scripted PC-1500 input ───────────
//
// Shared by BASIC program loading (typeBasicProgramText) and preset
// script steps (PC1500PresetLoader's key:/type: handling) -- both drive
// the ROM's own line editor/key-scan loop via simulated keystrokes rather
// than poking memory directly: typing a program in like a human and
// letting the ROM tokenize it avoids needing a from-scratch tokenizer.
//
// Scope: lowercase letters are typed via a SHIFT-tap before the base key
// (confirmed by the project owner against real hardware: a physical
// PC-1500 has one keycap per letter, and SHIFT -- a one-shot latch, same
// mechanism already used for shifted symbols below -- is what selects
// lowercase for it; this is NOT SML, a separate, persistent lowercase-
// mode toggle real hardware also has, deliberately not used here since
// per-character SHIFT is simpler and doesn't need to track/restore a
// mode across calls). Applies only to this file's scripted/typed input
// (preset steps, BASIC program text) -- the live interactive keyboard
// never does this: a physical or on-screen 'a' key press always sends
// the same "a" key regardless of case, matching how a real keyboard has
// no separate upper/lowercase keycaps. No multi-
// pass LIST-append handling for lines over the line editor's 79-char
// limit -- not needed by the debug scenarios driving this port.

/// Presses and releases the named key (PC1500Keyboard's vocabulary),
/// pacing the CPU forward through a real keystroke's hold/idle cycle. An
/// unmapped `name` is silently a no-op, same as PC1500Machine's own
/// pressKey/releaseKey.
void tapKey(PC1500Machine& machine, const std::string& name);

/// Runs a machine that was just reset flat out through its boot (the ROM's
/// power-on RAM check) until it waits at the prompt for keys: a fixed 2 s,
/// then the BUSY tail (waitIdle, 5 s cap). Stops at whatever prompt the ROM
/// shows -- including "NEW0? :CHECK" after a memory-map change. Used by the
/// preset loader and by the GUI's Reset.
void runBootToPrompt(PC1500Machine& machine);

/// Steps the CPU forward, a frame at a time, until the display's BUSY
/// indicator clears or `maxCycles` is exhausted (a safety cap, so a ROM
/// that never goes idle can't hang the caller). Returns the number of
/// cycles actually consumed.
uint64_t waitIdle(PC1500Machine& machine, uint64_t maxCycles);

/// Runs the CPU until the BASIC interpreter is back idling in its ROM
/// command loop (detected structurally -- the PC collapsing to a small
/// window high in ROM), or `maxCycles` is consumed. Unlike waitIdle() this
/// catches a plain `RUN` / plot finishing, which never asserts BUSY. Used
/// after each scripted `type:` line to let the line editor / a short RUN
/// settle. Returns the cycles consumed.
uint64_t settleUntilIdle(PC1500Machine& machine, uint64_t maxCycles);

/// Runs the CPU until a running BASIC program / plot has really finished --
/// the interpreter back spinning (HLT-per-frame) in its prompt command
/// loop at ~$E2AA, held continuously for ~a third of a second -- or
/// `maxCycles` is consumed. Returns the cycles consumed.
///
/// This is what a parameterless `- wait:` preset step needs, and it is
/// deliberately stricter than settleUntilIdle(): while a CE-150 plot runs,
/// the ROM sits for long stretches in a *tight* pen-move busy-wait loop
/// high in ROM ($F7xx), which settleUntilIdle()'s "small stable PC window
/// high in ROM" test reads as idle and returns early from. Keying off the
/// prompt loop's own address instead (measured: at the `>` prompt the
/// end-of-frame PC is $E2AA ~90% of frames and always within $E2xx-$E4xx;
/// during a plot it is never in that range) tells the two apart. The
/// PC-1500 ROM set is fixed, so the address is stable. Not covered: a
/// tight single-line `FOR I=1 TO big : NEXT` delay or a long-spinning
/// `CALL` into machine code -- both want an explicit `wait: N` step.
uint64_t waitUntilBasicIdle(PC1500Machine& machine, uint64_t maxCycles);

/// Types `line` character by character (a lowercase letter gets its own
/// SHIFT-tap first -- see file doc comment), optionally tapping Enter
/// afterward, then waits for BUSY to clear. Returns false
/// only for a character with no keyboard mapping (`error` names it) --
/// never for a ROM-side syntax rejection, which callers that care (like
/// typeBasicProgramText) detect themselves via the program-end pointer.
bool typeLine(PC1500Machine& machine, const std::string& line, bool pressEnter, std::string* error);

/// Types a whole BASIC program's source text in through the ROM's own
/// PRO-mode line editor, one statement line at a time, tokenizing exactly
/// as it would for a human typist (see file doc comment). Like the
/// PC-1600's and the fast basic-binary loaders, it neither clears nor
/// resets anything first: the lines are added to whatever program is
/// resident, and the caller must have left the machine ready to store
/// lines (on a cold-booted machine, CL then NEW0 -- a preset's own
/// `- key: cl` / `- type: NEW0` steps). A line that leaves the program
/// unchanged (BASPRG_END put and its own line, if resident, not rewritten)
/// is collected into the result as rejected rather than aborting the load.
BasicTypeResult typeBasicProgramText(PC1500Machine& machine, const std::string& text);
