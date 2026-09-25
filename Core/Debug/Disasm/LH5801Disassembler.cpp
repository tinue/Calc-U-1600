#include "LH5801Disassembler.hpp"

#include <array>
#include <string>

namespace disasm {

namespace {

// One opcode-map entry. `fmt` is the sdas text with operand placeholders,
// consumed left to right in byte order:
//   {n} 8-bit immediate        {a} 16-bit address (symbol if known)
//   {w} 16-bit immediate       {v} vector index byte (VMJ and friends)
//   {f} forward relative (+e)  {b} backward relative (-e)
// nullptr marks an undocumented opcode.
struct Op {
    const char* fmt = nullptr;
    Flow flow = Flow::None;
};

struct Tables {
    std::array<Op, 256> main{};
    std::array<Op, 256> fd{};
};

Tables buildTables() {
    Tables t;
    auto& m = t.main;
    auto& f = t.fd;
    static const char* const kRL[3] = {"xl", "yl", "ul"};
    static const char* const kRH[3] = {"xh", "yh", "uh"};
    static const char* const kR[3] = {"x", "y", "u"};
    // The pair-indexed rows keep their formats in static storage: the
    // table stores plain char pointers.
    static std::string store[3][64];
    for (int k = 0; k < 3; k++) {
        const int b = k << 4;
        std::string* s = store[k];
        int i = 0;
        auto set = [&](std::array<Op, 256>& tab, int op, std::string text, Flow flow = Flow::None) {
            s[i] = std::move(text);
            tab[op] = {s[i].c_str(), flow};
            i++;
        };
        const std::string rl = kRL[k], rh = kRH[k], r = kR[k];
        // 00-2F: the register/(Rreg) block
        set(m, b + 0x00, "sbc " + rl);        set(m, b + 0x01, "sbc (" + r + ")");
        set(m, b + 0x02, "adc " + rl);        set(m, b + 0x03, "adc (" + r + ")");
        set(m, b + 0x04, "lda " + rl);        set(m, b + 0x05, "lda (" + r + ")");
        set(m, b + 0x06, "cpa " + rl);        set(m, b + 0x07, "cpa (" + r + ")");
        set(m, b + 0x08, "sta " + rh);        set(m, b + 0x09, "and (" + r + ")");
        set(m, b + 0x0A, "sta " + rl);        set(m, b + 0x0B, "ora (" + r + ")");
        set(m, b + 0x0C, "dcs (" + r + ")");  set(m, b + 0x0D, "eor (" + r + ")");
        set(m, b + 0x0E, "sta (" + r + ")");  set(m, b + 0x0F, "bit (" + r + ")");
        // 40-6F: inc/dec, block moves, immediates
        set(m, b + 0x40, "inc " + rl);        set(m, b + 0x41, "sin " + r);
        set(m, b + 0x42, "dec " + rl);        set(m, b + 0x43, "sde " + r);
        set(m, b + 0x44, "inc " + r);         set(m, b + 0x45, "lin " + r);
        set(m, b + 0x46, "dec " + r);         set(m, b + 0x47, "lde " + r);
        set(m, b + 0x48, "ldi " + rh + ",{n}"); set(m, b + 0x49, "ani (" + r + "),{n}");
        set(m, b + 0x4A, "ldi " + rl + ",{n}"); set(m, b + 0x4B, "ori (" + r + "),{n}");
        set(m, b + 0x4C, "cpi " + rh + ",{n}"); set(m, b + 0x4D, "bii (" + r + "),{n}");
        set(m, b + 0x4E, "cpi " + rl + ",{n}"); set(m, b + 0x4F, "adi (" + r + "),{n}");
        // 80-AF: the high-byte register forms
        set(m, b + 0x80, "sbc " + rh);
        set(m, b + 0x82, "adc " + rh);
        set(m, b + 0x84, "lda " + rh);
        set(m, b + 0x86, "cpa " + rh);
        set(m, b + 0x8C, "dca (" + r + ")");
        // FD 00-2F: the ME1 twins of the (Rreg) forms
        set(f, b + 0x01, "sbc #(" + r + ")");  set(f, b + 0x03, "adc #(" + r + ")");
        set(f, b + 0x05, "lda #(" + r + ")");  set(f, b + 0x07, "cpa #(" + r + ")");
        set(f, b + 0x09, "and #(" + r + ")");  set(f, b + 0x0B, "ora #(" + r + ")");
        set(f, b + 0x0C, "dcs #(" + r + ")");  set(f, b + 0x0D, "eor #(" + r + ")");
        set(f, b + 0x0E, "sta #(" + r + ")");  set(f, b + 0x0F, "bit #(" + r + ")");
        set(f, b + 0x8C, "dca #(" + r + ")");
        set(f, b + 0x0A, "pop " + r);
        // FD 40-6F
        set(f, b + 0x40, "inc " + rh);
        set(f, b + 0x42, "dec " + rh);
        set(f, b + 0x49, "ani #(" + r + "),{n}");
        set(f, b + 0x4B, "ori #(" + r + "),{n}");
        set(f, b + 0x4D, "bii #(" + r + "),{n}");
        set(f, b + 0x4F, "adi #(" + r + "),{n}");
        // FD 88/98/A8: psh; FD CA/DA/EA: adr
        set(f, b + 0x88, "psh " + r);
        set(f, 0xCA + b, "adr " + r);
    }

    // (pp) absolute forms, ME0 and ME1
    m[0xA1] = {"sbc ({a})"};  f[0xA1] = {"sbc #({a})"};
    m[0xA3] = {"adc ({a})"};  f[0xA3] = {"adc #({a})"};
    m[0xA5] = {"lda ({a})"};  f[0xA5] = {"lda #({a})"};
    m[0xA7] = {"cpa ({a})"};  f[0xA7] = {"cpa #({a})"};
    m[0xA9] = {"and ({a})"};  f[0xA9] = {"and #({a})"};
    m[0xAB] = {"ora ({a})"};  f[0xAB] = {"ora #({a})"};
    m[0xAD] = {"eor ({a})"};  f[0xAD] = {"eor #({a})"};
    m[0xAE] = {"sta ({a})"};  f[0xAE] = {"sta #({a})"};
    m[0xAF] = {"bit ({a})"};  f[0xAF] = {"bit #({a})"};
    m[0xE9] = {"ani ({a}),{n}"}; f[0xE9] = {"ani #({a}),{n}"};
    m[0xEB] = {"ori ({a}),{n}"}; f[0xEB] = {"ori #({a}),{n}"};
    m[0xED] = {"bii ({a}),{n}"}; f[0xED] = {"bii #({a}),{n}"};
    m[0xEF] = {"adi ({a}),{n}"}; f[0xEF] = {"adi #({a}),{n}"};

    // Accumulator immediates and inherent forms
    m[0x38] = {"nop"};
    m[0xB1] = {"sbi a,{n}"};  m[0xB3] = {"adi a,{n}"};
    m[0xB5] = {"ldi a,{n}"};  m[0xB7] = {"cpi a,{n}"};
    m[0xB9] = {"ani a,{n}"};  m[0xBB] = {"ori a,{n}"};
    m[0xBD] = {"eai a,{n}"};  m[0xBF] = {"bii a,{n}"};
    m[0xAA] = {"ldi s,{w}"};
    m[0xA8] = {"spv"};  m[0xB8] = {"rpv"};
    m[0xE1] = {"spu"};  m[0xE3] = {"rpu"};
    m[0xF9] = {"rec"};  m[0xFB] = {"sec"};
    m[0xD1] = {"ror"};  m[0xD5] = {"shr"};  m[0xD9] = {"shl"};  m[0xDB] = {"rol"};
    m[0xD3] = {"drr (x)"};  m[0xD7] = {"drl (x)"};
    m[0xDD] = {"inc a"};    m[0xDF] = {"dec a"};
    m[0xF1] = {"aex"};  m[0xF5] = {"tin"};  m[0xF7] = {"cin"};

    // Jumps, branches, calls, returns
    m[0xBA] = {"jmp {a}", Flow::Jump};
    m[0xBE] = {"sjp {a}", Flow::Call};
    m[0x8E] = {"bch {f}", Flow::Jump};      m[0x9E] = {"bch {b}", Flow::Jump};
    m[0x81] = {"bcr {f}", Flow::CondJump};  m[0x91] = {"bcr {b}", Flow::CondJump};
    m[0x83] = {"bcs {f}", Flow::CondJump};  m[0x93] = {"bcs {b}", Flow::CondJump};
    m[0x85] = {"bhr {f}", Flow::CondJump};  m[0x95] = {"bhr {b}", Flow::CondJump};
    m[0x87] = {"bhs {f}", Flow::CondJump};  m[0x97] = {"bhs {b}", Flow::CondJump};
    m[0x89] = {"bzr {f}", Flow::CondJump};  m[0x99] = {"bzr {b}", Flow::CondJump};
    m[0x8B] = {"bzs {f}", Flow::CondJump};  m[0x9B] = {"bzs {b}", Flow::CondJump};
    m[0x8D] = {"bvr {f}", Flow::CondJump};  m[0x9D] = {"bvr {b}", Flow::CondJump};
    m[0x8F] = {"bvs {f}", Flow::CondJump};  m[0x9F] = {"bvs {b}", Flow::CondJump};
    m[0x88] = {"lop ul,{b}", Flow::CondJump};
    m[0xCD] = {"vmj {v}", Flow::Vector};
    m[0xC1] = {"vcr {v}", Flow::CondCall};  m[0xC3] = {"vcs {v}", Flow::CondCall};
    m[0xC5] = {"vhr {v}", Flow::CondCall};  m[0xC7] = {"vhs {v}", Flow::CondCall};
    m[0xC9] = {"vzr {v}", Flow::CondCall};  m[0xCB] = {"vzs {v}", Flow::CondCall};
    m[0xCF] = {"vvs {v}", Flow::CondCall};
    for (int op = 0xC0; op <= 0xFE; op += 2) m[op] = {"vej", Flow::Vector}; // the opcode is the vector index
    m[0x9A] = {"rtn", Flow::Return};
    m[0x8A] = {"rti", Flow::Return};

    // FD-prefixed transfers and CPU control
    f[0x18] = {"ldx y"};  f[0x28] = {"ldx u"};  f[0x48] = {"ldx s"};  f[0x58] = {"ldx p"};
    f[0x5A] = {"stx y"};  f[0x6A] = {"stx u"};  f[0x4E] = {"stx s"};
    f[0x5E] = {"stx p", Flow::Jump};  // P = X: an indirect jump, no static target
    f[0xC8] = {"psh a"};  f[0x8A] = {"pop a"};
    f[0xEC] = {"att"};    f[0xAA] = {"tta"};
    f[0xD3] = {"drr #(x)"};  f[0xD7] = {"drl #(x)"};
    f[0x81] = {"sie"};  f[0xBE] = {"rie"};
    f[0xC1] = {"sdp"};  f[0xC0] = {"rdp"};
    f[0xCE] = {"am0"};  f[0xDE] = {"am1"};
    f[0xCC] = {"atp"};  f[0xBA] = {"ita"};
    f[0x8E] = {"cdv"};
    f[0xB1] = {"hlt", Flow::Halt};
    f[0x4C] = {"off", Flow::Halt};
    return t;
}

const Tables& tables() {
    static const Tables t = buildTables();
    return t;
}

uint16_t vectorTarget(uint8_t index, const FetchFn& fetch) {
    const uint16_t base = uint16_t(0xFF00 + index);
    return uint16_t((fetch(base) << 8) | fetch(uint16_t(base + 1))); // big-endian, as vectorCall() reads it
}

} // namespace

Decoded decodeLH5801(uint16_t addr, const FetchFn& fetch, const SymbolFn& symbols) {
    Decoded d;
    uint16_t pc = addr;
    const uint8_t op = fetch(pc++);
    const Op* entry;
    if (op == 0xFD) {
        const uint8_t op2 = fetch(pc++);
        entry = &tables().fd[op2];
        if (!entry->fmt) {
            d.text = ".db " + hex8(0xFD) + "," + hex8(op2);
            d.len = 2;
            d.illegal = true;
            return d;
        }
    } else {
        entry = &tables().main[op];
        if (!entry->fmt) {
            d.text = ".db " + hex8(op);
            d.len = 1;
            d.illegal = true;
            return d;
        }
    }
    d.flow = entry->flow;

    // VEJ: the opcode itself is the vector index.
    if (op != 0xFD && op >= 0xC0 && (op & 1) == 0) {
        d.text = "vej " + hex8(op);
        d.hasTarget = true;
        d.target = vectorTarget(op, fetch);
        d.len = 1;
        return d;
    }

    std::string out;
    for (const char* p = entry->fmt; *p; p++) {
        if (*p != '{') { out += *p; continue; }
        const char kind = p[1];
        p += 2; // at '}'
        switch (kind) {
            case 'n': out += hex8(fetch(pc++)); break;
            case 'v': {
                const uint8_t index = fetch(pc++);
                out += hex8(index);
                d.hasTarget = true;
                d.target = vectorTarget(index, fetch);
                break;
            }
            case 'a':
            case 'w': {
                const uint16_t v = uint16_t((fetch(pc) << 8) | fetch(uint16_t(pc + 1))); // big-endian
                pc = uint16_t(pc + 2);
                out += kind == 'a' ? addrText(v, symbols) : hex16(v);
                if (kind == 'a' && d.flow != Flow::None) { d.hasTarget = true; d.target = v; }
                break;
            }
            case 'f':
            case 'b': {
                const uint8_t e = fetch(pc++);
                const uint16_t target = kind == 'f' ? uint16_t(pc + e) : uint16_t(pc - e);
                out += addrText(target, symbols);
                d.hasTarget = true;
                d.target = target;
                break;
            }
            default: break;
        }
    }
    d.text = std::move(out);
    d.len = uint8_t(pc - addr);
    return d;
}

} // namespace disasm
