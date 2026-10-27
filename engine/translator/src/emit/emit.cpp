#include "dream/translator/emit.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#include "dream/translator/analysis/constprop.h"
#include "dream/translator/analysis/switch.h"
#include "dream/translator/sh4/decoder.h"

namespace dream::translator {

using sh4::Instr;
using sh4::Op;

std::string default_name(std::uint32_t entry) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "fn_%08x", entry);
    return buf;
}

namespace {

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}
std::string hexu(std::uint32_t v) {
    return hex(v) + "u";
}
std::string label(std::uint32_t a) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "L_%08x", a);
    return buf;
}
std::string R(unsigned n) {
    return "c.r[" + std::to_string(n) + "]";
}
std::string S32(const std::string& e) {
    return "static_cast<std::int32_t>(" + e + ")";
}
std::string U32(const std::string& e) {
    return "static_cast<std::uint32_t>(" + e + ")";
}
std::string I(std::int32_t v) {
    return v < 0 ? "(" + std::to_string(v) + ")" : std::to_string(v);
}

// ---- the register cache -------------------------------------------------------------------------
// A guest store may alias anything, so with guest registers in the context the compiler re-reads
// them after every store. This pass rewrites a finished function's text so R0..R15 and FR0..FR15
// live in locals (rN_, fN_), loaded once at entry, before the resume switch. Registers that may
// differ from the context are stored back before anything that leaves the function or calls out,
// and reloaded where next read after a call. Which locals are current, and which may be dirty, is
// followed over the function's gotos to a fixed point: at a label, current on every way in, dirty
// on any. An interrupt poll stores and reloads inside its own braces. Lines that leave, call out
// or swap a register bank keep their context form, which is up to date there.
namespace regcache {

bool ident_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '_';
}

std::string local(char kind, unsigned n) {
    return std::string(1, kind) + std::to_string(n) + "_";
}

// R0..R15 (r), FR0..FR15 (f), the back bank XF0..XF15 (x), T (t, bit 0: c.t as `t0_`) and FPUL
// (p, bit 0: c.fpul as `p0_`). XF is read by FTRV and moved by the XD pair moves, so XF locals can
// be dirty; a bank swap stays in context form.
struct Set {
    std::uint32_t r = 0, f = 0, x = 0, t = 0, p = 0;
    void add(char kind, unsigned n) {
        (kind == 'r' ? r : kind == 'f' ? f : kind == 'x' ? x : kind == 't' ? t : p) |= 1u << n;
    }
    void add(const Set& o) {
        r |= o.r;
        f |= o.f;
        x |= o.x;
        t |= o.t;
        p |= o.p;
    }
    bool empty() const { return r == 0 && f == 0 && x == 0 && t == 0 && p == 0; }
};

// The ops.h helpers that take the context for T: these read it, and all but read_sr write it.
bool t_helper(const std::string& name, bool* writes = nullptr) {
    static const std::set<std::string> kT = {"addc", "subc",  "addv",  "subv",  "negc",   "rotcl",
                                             "rotcr", "div0u", "div0s", "div1", "read_sr",
                                             "rotcl_div1_run"};
    if (writes)
        *writes = name != "read_sr";
    return kT.count(name) != 0;
}

// `prefix` then a decimal at s[i]: the number, and i past it.
bool number_after(const std::string& s, std::size_t& i, const char* prefix, unsigned& v) {
    const std::size_t len = std::char_traits<char>::length(prefix);
    if (s.compare(i, len, prefix) != 0)
        return false;
    std::size_t j = i + len;
    if (j >= s.size() || s[j] < '0' || s[j] > '9')
        return false;
    v = 0;
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') v = v * 10 + static_cast<unsigned>(s[j++] - '0');
    i = j;
    return true;
}

// The calls a line makes with the context: the identifiers written as `name(c,` or `name(c)`.
std::vector<std::string> context_calls(const std::string& s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        if (s[i] != '(' || s[i + 1] != 'c' || (s[i + 2] != ',' && s[i + 2] != ')'))
            continue;
        std::size_t b = i;
        while (b > 0 && ident_char(s[b - 1])) --b;
        if (b < i)
            out.push_back(s.substr(b, i - b));
    }
    return out;
}

// What the line does with the cache.
enum class Kind { Code, Label, Leave, Call, SwapFr, SwapR };

bool has_word(const std::string& s, const char* w) {
    const std::size_t len = std::char_traits<char>::length(w);
    for (std::size_t p = s.find(w); p != std::string::npos; p = s.find(w, p + 1)) {
        const bool left = p == 0 || !ident_char(s[p - 1]);
        const bool right = p + len >= s.size() || !ident_char(s[p + len]);
        if (left && right)
            return true;
    }
    return false;
}

Kind classify(const std::string& s) {
    std::size_t a = s.find_first_not_of(' ');
    if (a != std::string::npos && s.compare(a, 2, "L_") == 0 && a == 0) {
        const std::size_t e = s.find_last_not_of(" ;");
        if (e != std::string::npos && s[e] == ':')
            return Kind::Label;
    }
    if (a != std::string::npos && s.compare(a, 2, "//") == 0)
        return Kind::Code;  // a comment
    bool call = false, swap_fr = false, swap_r = false;
    for (const std::string& f : context_calls(s)) {
        // Helpers that take the context but touch neither R0..R15 nor FR0..FR15 (T, SR, MACH,
        // MACL, FPUL, the back bank): the line keeps its locals.
        static const std::set<std::string> kQuiet = {
            "read_sr", "addc",  "subc",  "addv",  "subv",  "negc",  "rotcl",
            "rotcr",   "div0u", "div0s", "div1",  "dmuls", "dmulu", "mac_l",
            "mac_w",   "get_dr", "set_dr", "fmov_pair_load", "fmov_pair_store",
            "ftrv",    "fipr",  "fsca",  "get_xd", "set_xd", "rotcl_div1_run"};
        if (f == "frchg" || f == "write_fpscr")
            swap_fr = true;
        else if (f == "write_sr")
            swap_r = true;
        else if (!kQuiet.count(f))
            call = true;
    }
    if (call)
        return Kind::Call;
    if (has_word(s, "goto") || has_word(s, "return") || has_word(s, "switch") ||
        has_word(s, "case") || has_word(s, "default"))
        return Kind::Leave;
    if (swap_fr)
        return Kind::SwapFr;
    if (swap_r)
        return Kind::SwapR;
    return Kind::Code;
}

// The front-bank helpers in their local forms (ops.h), then c.r[n] / c.fr[n] as locals.
std::string to_locals(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    auto f = [](unsigned n) { return local('f', n); };
    const bool runtime_mode = s.find("c.fpscr &") != std::string::npos;
    for (std::size_t i = 0; i < s.size();) {
        const bool boundary = i == 0 || !ident_char(s[i - 1]);
        unsigned n = 0, m = 0;
        std::size_t j = i;
        if (boundary && number_after(s, j, "get_dr(c, ", n) && j < s.size() && s[j] == ')') {
            out += "dr_of(" + f(n) + ", " + f(n + 1) + ")";
            i = j + 1;
            continue;
        }
        j = i;
        if (boundary && number_after(s, j, "set_dr(c, ", n) && s.compare(j, 2, ", ") == 0) {
            out += "dr_set(" + f(n) + ", " + f(n + 1) + ", ";
            i = j + 2;
            continue;
        }
        j = i;
        if (boundary && number_after(s, j, "fmov_pair_load(c, ", n) && (n & 1u) == 0 &&
            s.compare(j, 2, ", ") == 0) {
            out += "pair_set(" + f(n) + ", " + f(n + 1) + ", ";
            i = j + 2;
            continue;
        }
        // The back bank's pairs (XD: an odd field, ops.h): XF n-1 and n as locals too. Not under a
        // runtime test of the mode: there the XD move is the way rarely taken, and locals for it
        // would cost loads and stores on the common way.
        j = i;
        if (boundary && !runtime_mode && number_after(s, j, "fmov_pair_load(c, ", n) && (n & 1u) == 1 &&
            s.compare(j, 2, ", ") == 0) {
            out += "pair_set(" + local('x', n - 1) + ", " + local('x', n) + ", ";
            i = j + 2;
            continue;
        }
        j = i;
        if (boundary && !runtime_mode && number_after(s, j, "fmov_pair_store(c, ", n) && (n & 1u) == 1 &&
            j < s.size() && s[j] == ')') {
            out += "pair_of(" + local('x', n - 1) + ", " + local('x', n) + ")";
            i = j + 1;
            continue;
        }
        j = i;
        if (boundary && number_after(s, j, "fmov_pair_store(c, ", n) && (n & 1u) == 0 &&
            j < s.size() && s[j] == ')') {
            out += "pair_of(" + f(n) + ", " + f(n + 1) + ")";
            i = j + 1;
            continue;
        }
        j = i;
        if (boundary && number_after(s, j, "ftrv(c, ", n) && j < s.size() && s[j] == ')') {
            out += "ftrv_x(";  // the matrix from XF locals: loop-invariant where XF does not change
            for (unsigned k = 0; k < 16; ++k) out += local('x', k) + ", ";
            out += f(n) + ", " + f(n + 1) + ", " + f(n + 2) + ", " + f(n + 3) + ")";
            i = j + 1;
            continue;
        }
        j = i;
        if (boundary && number_after(s, j, "fipr(c, ", m) && number_after(s, j, ", ", n) &&
            j < s.size() && s[j] == ')') {
            out += f(n + 3) + " = fipr_of(" + f(n) + ", " + f(n + 1) + ", " + f(n + 2) + ", " +
                   f(n + 3) + ", " + f(m) + ", " + f(m + 1) + ", " + f(m + 2) + ", " + f(m + 3) +
                   ")";
            i = j + 1;
            continue;
        }
        j = i;
        if (boundary && number_after(s, j, "fsca(c, ", n) && j < s.size() && s[j] == ')') {
            out += "fsca_set(c, " + f(n) + ", " + f(n + 1) + ")";
            i = j + 1;
            continue;
        }
        j = i;
        if (boundary && number_after(s, j, "c.r[", n) && j < s.size() && s[j] == ']') {
            out += local('r', n);
            i = j + 1;
            continue;
        }
        j = i;
        if (boundary && number_after(s, j, "c.fr[", n) && j < s.size() && s[j] == ']') {
            out += local('f', n);
            i = j + 1;
            continue;
        }
        if (boundary && s.compare(i, 3, "c.t") == 0 && (i + 3 >= s.size() || !ident_char(s[i + 3]))) {
            out += local('t', 0);
            i += 3;
            continue;
        }
        if (boundary && s.compare(i, 6, "c.fpul") == 0 && (i + 6 >= s.size() || !ident_char(s[i + 6]))) {
            out += local('p', 0);
            i += 6;
            continue;
        }
        out += s[i++];
    }
    return out;
}

// The locals a rewritten line names, the ones it writes (an assignment, plain or compound, an
// increment, or a by-reference argument of a local-form helper) and the ones it reads: every
// occurrence but the target of a plain assignment, a helper's arguments included.
void scan(const std::string& s, Set& named, Set& written, Set* read = nullptr) {
    auto skip = [&](std::size_t p) {
        while (p < s.size() && s[p] == ' ') ++p;
        return p;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((s[i] != 'r' && s[i] != 'f' && s[i] != 'x' && s[i] != 't' && s[i] != 'p') ||
            (i > 0 && ident_char(s[i - 1])))
            continue;
        std::size_t j = i + 1;
        unsigned n = 0;
        if (j >= s.size() || s[j] < '0' || s[j] > '9')
            continue;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') n = n * 10 + static_cast<unsigned>(s[j++] - '0');
        if (j >= s.size() || s[j] != '_' || (j + 1 < s.size() && ident_char(s[j + 1])) || n > 15)
            continue;
        named.add(s[i], n);
        const std::size_t after = skip(j + 1);
        bool write = false, plain = false;
        if (after < s.size()) {
            const char c0 = s[after], c1 = after + 1 < s.size() ? s[after + 1] : '\0';
            const char c2 = after + 2 < s.size() ? s[after + 2] : '\0';
            if (c0 == '=' && c1 != '=')
                write = plain = true;
            else if (c1 == '=' && std::string("+-*/%&|^").find(c0) != std::string::npos && c0 != '\0')
                write = true;
            else if ((c0 == '<' && c1 == '<' && c2 == '=') || (c0 == '>' && c1 == '>' && c2 == '='))
                write = true;
            else if ((c0 == '+' && c1 == '+') || (c0 == '-' && c1 == '-'))
                write = true;
        }
        std::size_t before = i;
        while (before > 0 && s[before - 1] == ' ') --before;
        if (before >= 2 && ((s[before - 1] == '+' && s[before - 2] == '+') ||
                            (s[before - 1] == '-' && s[before - 2] == '-')))
            write = true;
        if (write)
            written.add(s[i], n);
        if (read && !plain)
            read->add(s[i], n);
        i = j;
    }
    // rotcl_div1_run(c, rA_, rN_, rM_, k) writes its first two registers (by reference).
    for (std::size_t p = s.find("rotcl_div1_run(c, r"); p != std::string::npos;
         p = s.find("rotcl_div1_run(c, r", p + 1)) {
        std::size_t q = p + std::char_traits<char>::length("rotcl_div1_run(c, r");
        for (int arg = 0; arg < 2; ++arg) {
            unsigned n = 0;
            while (q < s.size() && s[q] >= '0' && s[q] <= '9') n = n * 10 + static_cast<unsigned>(s[q++] - '0');
            written.add('r', n);
            q = s.find(", r", q);
            if (q == std::string::npos)
                break;
            q += 3;
        }
    }
    // By-reference arguments of the local forms. dr_set(fN_, fN+1_, value) and pair_set(...) write
    // the pair their first argument starts; ftrv_x(...) and fsca_set(c, ...) write every FR
    // register they name (ftrv_x only reads its XF arguments).
    for (const char* h : {"dr_set(f", "pair_set(f", "pair_set(x"}) {
        const std::size_t len = std::char_traits<char>::length(h);
        const char kind = h[len - 1];
        for (std::size_t p = s.find(h); p != std::string::npos; p = s.find(h, p + 1)) {
            unsigned n = 0;
            for (std::size_t q = p + len; q < s.size() && s[q] >= '0' && s[q] <= '9'; ++q)
                n = n * 10 + static_cast<unsigned>(s[q] - '0');
            written.add(kind, n);
            written.add(kind, n + 1);
        }
    }
    // A runtime test of the FP mode (`if (c.fpscr & FPSCR_SZ) { ... } else { ... }`) assigns some
    // registers on one way only: every register the line names counts as read too, so its local
    // is current before the line whichever way it goes.
    if (read && s.find("c.fpscr &") != std::string::npos) {
        read->add(named);
        return;
    }
    // Otherwise a pair set writes both registers whole: one named nowhere else on the line is not
    // read, so it needs no load first.
    if (!read)
        return;
    for (const char* h : {"dr_set(", "pair_set("}) {
        const std::size_t len = std::char_traits<char>::length(h);
        for (std::size_t p = s.find(h); p != std::string::npos; p = s.find(h, p + 1)) {
            if (p > 0 && ident_char(s[p - 1]))
                continue;
            std::size_t q = p + len;
            for (int arg = 0; arg < 2; ++arg) {
                const std::size_t e = s.find(',', q);
                if (e == std::string::npos)
                    break;
                std::string name = s.substr(q, e - q);
                name.erase(0, name.find_first_not_of(' '));
                q = e + 1;
                if (name.size() < 3 || name.back() != '_')
                    continue;
                std::size_t uses = 0;
                for (std::size_t u = s.find(name); u != std::string::npos; u = s.find(name, u + 1))
                    if ((u == 0 || !ident_char(s[u - 1])) &&
                        (u + name.size() >= s.size() || !ident_char(s[u + name.size()])))
                        ++uses;
                if (uses != 1 || (name[0] != 'f' && name[0] != 'x'))
                    continue;
                unsigned n = 0;
                bool digits = name.size() > 2;
                for (std::size_t d = 1; d + 1 < name.size(); ++d) {
                    digits = digits && name[d] >= '0' && name[d] <= '9';
                    n = n * 10 + static_cast<unsigned>(name[d] - '0');
                }
                if (!digits || n > 15)
                    continue;
                const std::uint32_t bit = 1u << n;
                if (name[0] == 'f')
                    read->f &= ~bit;
                else if (name[0] == 'x')
                    read->x &= ~bit;
            }
        }
    }
    for (const char* h : {"ftrv_x(", "fsca_set(c, "}) {
        const std::size_t len = std::char_traits<char>::length(h);
        for (std::size_t p = s.find(h); p != std::string::npos; p = s.find(h, p + 1)) {
            const std::size_t close = s.find(')', p);
            Set args, ignored;
            scan(s.substr(p + len, close == std::string::npos ? std::string::npos : close - p - len),
                 args, ignored);
            args.x = 0;
            written.add(args);
        }
    }
}

// The context's spelling of a local: c.r[n], c.fr[n], c.xf[n], c.t.
std::string field(char kind, unsigned n) {
    if (kind == 't')
        return "c.t";
    if (kind == 'p')
        return "c.fpul";
    return std::string(kind == 'r' ? "c.r[" : kind == 'f' ? "c.fr[" : "c.xf[") + std::to_string(n) +
           "]";
}

// `local = field;` (load) or `field = local;` for every register of the set, each followed by
// `sep`.
std::string moves(const Set& s, bool load, const std::string& before, const char* sep) {
    std::string out;
    for (const auto& [kind, mask] :
         {std::pair<char, std::uint32_t>{'r', s.r}, {'f', s.f}, {'x', s.x}, {'t', s.t}, {'p', s.p}})
        for (unsigned n = 0; n < 16; ++n)
            if (mask & (1u << n))
                out += before + (load ? local(kind, n) + " = " + field(kind, n)
                                      : field(kind, n) + " = " + local(kind, n)) +
                       sep;
    return out;
}

std::string loads(const Set& s, const char* indent) {
    return moves(s, true, indent, ";\n");
}

std::string stores(const Set& s, const char* indent) {
    return moves(s, false, indent, ";\n");
}

// The whole pass, over one function as FunctionEmitter::run writes it.
std::string apply(const std::string& text) {
    std::vector<std::string> lines;
    for (std::size_t p = 0; p < text.size();) {
        const std::size_t e = text.find('\n', p);
        lines.push_back(text.substr(p, e == std::string::npos ? std::string::npos : e - p));
        p = e == std::string::npos ? text.size() : e + 1;
    }
    // The body starts at the first label; the function's opening line is where locals go.
    std::size_t open = lines.size(), body = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (open == lines.size() && !lines[i].empty() && lines[i].back() == '{' &&
            lines[i].find("(Ctx& c, Memory& m") != std::string::npos)
            open = i;
        if (classify(lines[i]) == Kind::Label) {
            body = i;
            break;
        }
    }
    if (open == lines.size() || body == lines.size())
        return text;
    std::size_t close = lines.size();
    while (close > body && lines[close - 1] != "}") --close;
    if (close == body)
        return text;
    --close;  // the function's closing brace
    // Each body line, as the pass treats it.
    //  - Code is rewritten to locals. It may branch within the function (`goto`) and may hold an
    //    interrupt poll, which writes back and reloads inside its own braces.
    //  - Exit (a `return` without a call), Call and the bank swaps stay as written and read the
    //    context, so whatever may be dirty goes back first. A call may change any register, a swap
    //    its bank, and a line as written may change any register it names.
    //  - A recovered switch's head writes back too: nothing may go between its case labels.
    enum class Role { Code, Label, Exit, Call, SwapFr, SwapR };
    struct Line {
        Role role = Role::Code;
        std::string text, label;
        Set named, written, read;
        std::vector<std::string> gotos;
        bool head = false;
        bool xd_write = false;  // an XD move into the back bank, in context form: XF locals stale
        bool t_reads = false, t_writes = false;  // an ops.h helper reads (writes) c.t itself
        bool p_reads = false;                    // an ops.h helper reads c.fpul itself (FSCA)
        bool terminator = false;  // nothing after it runs until the next label (see below)
        std::size_t poll = std::string::npos, poll_end = std::string::npos;  // `{` and `}`
    };
    static const std::string kPoll = "if (c.cycles >= c.next_event) {";
    auto label_name = [](const std::string& s) { return s.substr(0, s.find(':')); };
    // A line after which nothing runs until the next label: a bare `return;` (an rts), a statement
    // list ending in an unconditional `; return;` (a tail call), a bare `goto L_...;`, or a
    // back-edge's poll and then its `goto`. The dead fall-through into the next label must not
    // count in the label's merged state.
    auto terminates = [](const std::string& s) {
        const std::size_t a = s.find_first_not_of(' ');
        if (a == std::string::npos)
            return false;
        const std::string t = s.substr(a);
        if (t == "return;")
            return true;
        if (t.rfind("goto L_", 0) == 0 && t.back() == ';' && t.find(' ', 5) == std::string::npos)
            return true;
        if (t.rfind(kPoll, 0) == 0) {
            int depth = 0;
            std::size_t e = kPoll.size() - 1;
            for (; e < t.size(); ++e) {
                if (t[e] == '{')
                    ++depth;
                else if (t[e] == '}' && --depth == 0)
                    break;
            }
            const std::string after = e < t.size() ? t.substr(e + 1) : std::string();
            return after.rfind(" goto L_", 0) == 0 && after.back() == ';' &&
                   after.find(' ', 6) == std::string::npos;
        }
        const char* kTail = "; return;";
        if (t.size() > 9 && t.compare(t.size() - 9, 9, kTail) == 0) {
            for (const char* w : {"if", "else", "case", "default", "switch", "{", "}"})
                if (t.rfind(w, 0) == 0)
                    return false;
            return true;
        }
        return false;
    };
    std::vector<Line> out(lines.size());
    Set all;
    for (std::size_t i = body; i < close; ++i) {
        Line& l = out[i];
        const std::string& s = lines[i];
        if (classify(s) == Kind::Label) {
            l.role = Role::Label;
            l.label = label_name(s);
            l.text = s;
            continue;
        }
        l.terminator = terminates(s);
        for (std::size_t p = s.find("goto L_"); p != std::string::npos; p = s.find("goto L_", p + 1)) {
            std::size_t e = p + 5;
            while (e < s.size() && ident_char(s[e])) ++e;
            l.gotos.push_back(s.substr(p + 5, e - p - 5));
        }
        // The line without its poll decides its role.
        std::string rest = s;
        const std::size_t poll = s.find(kPoll);
        if (poll != std::string::npos) {
            int depth = 0;
            std::size_t e = poll + kPoll.size() - 1;
            for (; e < s.size(); ++e) {
                if (s[e] == '{')
                    ++depth;
                else if (s[e] == '}' && --depth == 0)
                    break;
            }
            l.poll = poll + kPoll.size() - 1;
            l.poll_end = e;
            rest = s.substr(0, poll) + s.substr(std::min(e + 1, s.size()));
        }
        const Kind k = classify(rest);
        if (k == Kind::Call || k == Kind::SwapFr || k == Kind::SwapR || has_word(rest, "return")) {
            l.role = k == Kind::Call     ? Role::Call
                     : k == Kind::SwapFr ? Role::SwapFr
                     : k == Kind::SwapR  ? Role::SwapR
                                         : Role::Exit;
            l.text = s;
            l.poll = l.poll_end = std::string::npos;  // the context is current around the line
            Set ignored;
            scan(to_locals(s), l.named, ignored);
            continue;
        }
        l.text = to_locals(s);
        scan(l.text, l.named, l.written, &l.read);
        for (const std::string& f : context_calls(rest)) {
            bool writes = false;
            if (t_helper(f, &writes)) {
                l.t_reads = true;
                l.t_writes = l.t_writes || writes;
            }
            if (f == "fsca_set" || f == "fsca")
                l.p_reads = true;  // takes the angle from c.fpul
        }
        if ((l.t_reads && l.named.t) || (l.p_reads && l.named.p)) {
            // T or FPUL both through a helper and by name: kept simple, as a call.
            l.role = Role::Call;
            l.text = s;
            l.poll = l.poll_end = std::string::npos;
            l.named = l.written = l.read = Set{};
            Set ignored;
            scan(to_locals(s), l.named, ignored);
            continue;
        }
        l.head = rest.find("switch (") != std::string::npos;
        // Pair moves were rewritten above, XD ones too; any left in context form write XF.
        l.xd_write = l.text.find("fmov_pair_load(c, ") != std::string::npos ||
                     l.text.find("set_xd(c, ") != std::string::npos;
        Set others = l.named;
        others.t = 0;
        // A branch on T beside a poll (`if (c.t) { ... poll ... goto ...; }`) keeps the poll's
        // inline moves: T is only read, before the poll, and `t0_` is as long as `c.t`, so the
        // poll's position holds.
        const bool t_read_only = others.empty() && !l.written.t && l.text.size() == s.size();
        if (l.poll != std::string::npos && !t_read_only && (!l.named.empty() || l.text != s)) {
            // A poll beside register code: kept simple, as a call.
            l.role = Role::Call;
            l.text = s;
            l.poll = l.poll_end = std::string::npos;
            l.written = l.read = Set{};
            continue;
        }
        all.add(l.named);
    }
    if (all.empty())
        return text;

    struct State {
        Set cur, dirty;  // locals equal to or newer than the context; locals that may be newer
        bool operator==(const State& o) const {
            return cur.r == o.cur.r && cur.f == o.cur.f && cur.x == o.cur.x && cur.t == o.cur.t &&
                   cur.p == o.cur.p && dirty.r == o.dirty.r && dirty.f == o.dirty.f &&
                   dirty.x == o.dirty.x && dirty.t == o.dirty.t && dirty.p == o.dirty.p;
        }
    };
    auto meet = [](const State& a, const State& b) {
        State m;
        m.cur.r = a.cur.r & b.cur.r;
        m.cur.f = a.cur.f & b.cur.f;
        m.cur.x = a.cur.x & b.cur.x;
        m.cur.t = a.cur.t & b.cur.t;
        m.cur.p = a.cur.p & b.cur.p;
        m.dirty.r = a.dirty.r | b.dirty.r;
        m.dirty.f = a.dirty.f | b.dirty.f;
        m.dirty.x = a.dirty.x | b.dirty.x;
        m.dirty.t = a.dirty.t | b.dirty.t;
        m.dirty.p = a.dirty.p | b.dirty.p;
        return m;
    };
    auto inline_stores = [](const Set& s) { return moves(s, false, "", "; "); };
    auto inline_loads = [](const Set& s) { return moves(s, true, "", "; "); };
    // Entry: every local loaded (before the resume switch, so a resumed body starts the same way),
    // none dirty. A resume `case` therefore brings nothing a label's meet has to account for.
    State entry;
    entry.cur = all;

    // One walk over the body: the state each line leaves, the states sent along gotos, each
    // label's merged state, and, when `result` is given, the text with its loads and stores.
    // `in` and `prev` are the previous walk's gotos and labels; walks repeat until both settle
    // (current sets only shrink, dirty sets only grow).
    //
    // A register dirty on one way into a label and not current on another would leave neither
    // the local nor the context right after it. So on every way in, a register the label has as
    // dirty and the way has not current is loaded first: before the label for the fall-through,
    // inside the goto's own statement for a jump. A loop head reached from code after a call loads
    // once on the way in, not on every pass.
    using Edges = std::map<std::string, State>;
    auto dirty_at = [](const Edges& labs, const std::string& l) {
        auto it = labs.find(l);
        return it == labs.end() ? Set{} : it->second.dirty;
    };
    auto minus = [](const Set& a, const Set& b) {
        Set d;
        d.r = a.r & ~b.r;
        d.f = a.f & ~b.f;
        d.x = a.x & ~b.x;
        d.t = a.t & ~b.t;
        d.p = a.p & ~b.p;
        return d;
    };
    auto walk = [&](const Edges& in, const Edges& prev, Edges& sent, Edges& labs,
                    std::string* result) {
        auto emit = [&](const std::string& t) {
            if (result)
                *result += t;
        };
        State s = entry;
        bool live = true;  // false after a terminator, until the next label
        for (std::size_t i = body; i < close; ++i) {
            const Line& l = out[i];
            if (l.role == Role::Label) {
                if (live) {
                    const Set need = minus(dirty_at(prev, l.label), s.cur);
                    emit(loads(need, "    "));  // the fall-through's way in
                    s.cur.add(need);
                } else {
                    // No fall-through: the label is reached by its gotos, or as a resumed body
                    // enters, with every local loaded and none dirty (the meet's neutral state).
                    s = entry;
                }
                live = true;
                if (auto it = in.find(l.label); it != in.end())
                    s = meet(s, it->second);
                if (auto it = sent.find(l.label); it != sent.end())  // earlier in this walk
                    s = meet(s, it->second);
                labs[l.label] = s;
                emit(l.text + "\n");
                continue;
            }
            if (!live) {
                emit(l.text + "\n");  // never runs: its moves and gotos do not matter
                continue;
            }
            std::string t = l.text;
            if (l.role != Role::Code) {
                emit(stores(s.dirty, "    "));
                s.dirty = Set{};
                if (l.role == Role::Call) {
                    s.cur = Set{};
                } else if (l.role == Role::SwapFr) {
                    s.cur.f = 0;  // FRCHG, or FPSCR.FR through write_fpscr: the banks trade places
                    s.cur.x = 0;
                } else if (l.role == Role::SwapR) {
                    s.cur.r = 0;
                    s.cur.t = 0;  // write_sr sets T too
                }
                s.cur.r &= ~l.named.r;
                s.cur.f &= ~l.named.f;
                s.cur.t &= ~l.named.t;
                s.cur.p &= ~l.named.p;
            } else {
                if (l.head) {
                    emit(stores(s.dirty, "    "));
                    s.dirty = Set{};
                }
                if (l.t_reads && s.dirty.t) {
                    Set only_t;
                    only_t.t = 1;
                    emit(stores(only_t, "    "));
                    s.dirty.t = 0;
                }
                if (l.p_reads && s.dirty.p) {
                    Set only_p;
                    only_p.p = 1;
                    emit(stores(only_p, "    "));
                    s.dirty.p = 0;
                }
                const Set need = minus(l.read, s.cur);
                emit(loads(need, "    "));
                s.cur.add(need);
                if (l.poll != std::string::npos && result) {
                    t.insert(l.poll_end, inline_loads(s.cur));
                    t.insert(l.poll + 1, " " + inline_stores(s.dirty));
                }
                s.cur.add(l.named);
                s.dirty.add(l.written);
                if (l.xd_write)
                    s.cur.x = 0;
                if (l.t_writes)
                    s.cur.t = 0;
            }
            for (const std::string& g : l.gotos) {
                const Set need = minus(dirty_at(prev, g), s.cur);
                State e = s;
                e.cur.add(need);
                if (auto it = sent.find(g); it == sent.end())
                    sent.emplace(g, e);
                else
                    it->second = meet(it->second, e);
                if (result && !need.empty()) {
                    const std::string go = "goto " + g + ";";
                    if (const std::size_t p = t.find(go); p != std::string::npos)
                        t.replace(p, go.size(), "{ " + inline_loads(need) + go + " }");
                }
            }
            emit(t + "\n");
            if (l.terminator)
                live = false;
        }
        emit(stores(s.dirty, "    "));
    };
    Edges in, prev;
    bool settled = false;
    for (int pass = 0; pass < 1000 && !settled; ++pass) {
        Edges sent, labs;
        walk(in, prev, sent, labs, nullptr);
        settled = sent == in && labs == prev;
        in = std::move(sent);
        prev = std::move(labs);
    }
    if (!settled)
        return text;  // the function keeps every register in the context

    std::string result;
    bool loaded = false;
    for (std::size_t i = 0; i < body; ++i) {
        // The entry loads go after the entry poll, before the resume switch.
        if (!loaded && lines[i].rfind("    if (resume_pc)", 0) == 0) {
            result += loads(all, "    ");
            loaded = true;
        }
        result += lines[i] + "\n";
        if (i == open) {
            std::string decl;
            for (unsigned n = 0; n < 16; ++n)
                if (all.r & (1u << n))
                    decl += (decl.empty() ? "" : ", ") + local('r', n);
            if (all.t)
                decl += (decl.empty() ? "" : ", ") + local('t', 0);
            if (all.p)
                decl += (decl.empty() ? "" : ", ") + local('p', 0);
            if (!decl.empty())
                result += "    std::uint32_t " + decl + ";\n";
            decl.clear();
            for (unsigned n = 0; n < 16; ++n)
                if (all.f & (1u << n))
                    decl += (decl.empty() ? "" : ", ") + local('f', n);
            for (unsigned n = 0; n < 16; ++n)
                if (all.x & (1u << n))
                    decl += (decl.empty() ? "" : ", ") + local('x', n);
            if (!decl.empty())
                result += "    float " + decl + ";\n";
        }
    }
    if (!loaded)
        result += loads(all, "    ");
    Edges unused_sent, unused_labs;
    walk(in, prev, unused_sent, unused_labs, &result);
    for (std::size_t i = close; i < lines.size(); ++i) {
        result += lines[i];
        if (i + 1 < lines.size() || (!text.empty() && text.back() == '\n'))
            result += "\n";
    }
    return result;
}

}  // namespace regcache

// ---- guest memory through local bases -----------------------------------------------------------
// Memory's accessors read the RAM and store-queue pointers and the fast-store flag from the Memory
// object, which the compiler must re-read after every guest store (a store may alias anything).
// This pass takes them into locals once the function starts (after its entry poll, so a hook or a
// handler run there is already seen) and rewrites the accesses into ops.h's forms that take them
// (rd32, wr32, ...): the same tests and slow paths.
namespace memcache {

// The guest clock lives in a local too, `cy_`, taken with the bases. The context's clock is made
// current wherever something else may read it, and taken back after:
//  - a line that calls anything but ops.h's helpers (another function, the interrupt delivery,
//    the runtime), calls the memory object (a PREF), or returns: written back before the line and
//    taken again after it; the line itself keeps the context's spelling;
//  - an interrupt poll in any other line: inside the poll's braces;
//  - a slow memory access: inside the accessor (ops.h, the forms with the clock);
//  - the function's end.
// Everything else (the block costs, the polls' tests) uses the local.

// The text of a line with every interrupt poll's braces cut out; `polls` gets each poll's `{` and
// matching `}`.
const std::string kPollTest = "if (c.cycles >= c.next_event) {";
const std::string kLocalPollTest = "if (cy_ >= c.next_event) {";
std::string without_polls(const std::string& s, std::vector<std::pair<std::size_t, std::size_t>>* polls,
                          const std::string& test = kPollTest) {
    std::string rest;
    std::size_t from = 0;
    for (std::size_t p = s.find(test); p != std::string::npos; p = s.find(test, p + 1)) {
        const std::size_t open = p + test.size() - 1;
        int depth = 0;
        std::size_t e = open;
        for (; e < s.size(); ++e) {
            if (s[e] == '{')
                ++depth;
            else if (s[e] == '}' && --depth == 0)
                break;
        }
        if (e >= s.size())
            break;
        if (polls)
            polls->emplace_back(open, e);
        rest += s.substr(from, p - from);
        from = e + 1;
        p = e;
    }
    return rest + s.substr(std::min(from, s.size()));
}

// Whether the line, polls left out, needs the context's clock current.
bool needs_clock(const std::string& rest) {
    // ops.h's helpers: none reads or writes the clock.
    static const std::set<std::string> kHelpers = {
        "addc",   "subc",   "addv",   "subv",   "negc",           "rotcl",           "rotcr",
        "div0u",  "div0s",  "div1",   "dmuls",  "dmulu",          "mac_l",           "mac_w",
        "read_sr", "write_sr", "frchg", "write_fpscr", "get_dr",  "set_dr",          "get_xd",
        "set_xd", "fmov_pair_load", "fmov_pair_store", "ftrv", "fipr", "fsca", "fsca_set",
        "rotcl_div1_run"};
    for (const std::string& f : regcache::context_calls(rest))
        if (!kHelpers.count(f))
            return true;
    // A call on the memory object other than the accesses this pass rewrites.
    for (std::size_t p = rest.find("m."); p != std::string::npos; p = rest.find("m.", p + 1)) {
        if (p > 0 && regcache::ident_char(rest[p - 1]))
            continue;
        static const char* kAccess[] = {"read8(", "read16(", "read32(", "write8(", "write16(", "write32("};
        bool access = false;
        for (const char* a : kAccess)
            access = access || rest.compare(p + 2, std::char_traits<char>::length(a), a) == 0;
        if (!access)
            return true;
    }
    return regcache::has_word(rest, "return") || regcache::has_word(rest, "throw");
}

// `from` replaced by `to` wherever it starts an identifier.
void replace_word(std::string& s, const std::string& from, const std::string& to) {
    for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p)) {
        if (p > 0 && regcache::ident_char(s[p - 1])) {
            p += from.size();
            continue;  // a longer name ending the same way
        }
        s.replace(p, from.size(), to);
        p += to.size();
    }
}

// Runs of stack pushes and pops: a prologue's `mov.l rN,@-r15`s and an epilogue's
// `mov.l @r15+,rN`s, one RAM test each. Two or more in a row (the guest's listing comments between
// them allowed) become one test that the whole range is fast RAM in one window (ops.h, mem_run),
// then plain accesses; the lines as they were in the other way, which the write hash and the
// journal take (their views are empty). A pop into r15 ends a run: it moves the rest.
std::string stack_runs(const std::string& text) {
    static const std::string kPush = "    r15_ -= 4; wr32(c, cy_, m, ms_, mq_, r15_, ";
    static const std::string kPopTail = "rd32(c, cy_, m, mr_, r15_)";
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string l;
        while (std::getline(in, l)) lines.push_back(std::move(l));
    }
    auto is_comment = [](const std::string& l) {
        const std::size_t a = l.find_first_not_of(' ');
        return a != std::string::npos && l.compare(a, 2, "//") == 0;
    };
    // A push's value expression, or empty when the line is not one.
    auto push_value = [&](const std::string& l) -> std::string {
        if (l.rfind(kPush, 0) != 0 || l.size() < kPush.size() + 2 || l.compare(l.size() - 2, 2, ");") != 0)
            return {};
        std::string v = l.substr(kPush.size(), l.size() - 2 - kPush.size());
        if (v.find("r15_") != std::string::npos)
            return {};  // r15 itself: the fast way moves it only at the end
        int depth = 0;
        for (char ch : v) {  // one argument: no top-level comma
            if (ch == '(')
                ++depth;
            else if (ch == ')')
                --depth;
            else if (ch == ',' && depth == 0)
                return {};
        }
        return depth == 0 ? v : std::string();
    };
    // A pop's destination and whether it is a float (`f = u2f(...)`), or empty.
    auto pop_target = [&](const std::string& l, bool& fp) -> std::string {
        const std::string tail = "; r15_ += 4;";
        if (l.rfind("    ", 0) != 0 || l.size() < tail.size() || l.compare(l.size() - tail.size(), tail.size(), tail) != 0)
            return {};
        const std::size_t eq = l.find(" = ");
        if (eq == std::string::npos)
            return {};
        const std::string dst = l.substr(4, eq - 4);
        const std::string rhs = l.substr(eq + 3, l.size() - tail.size() - eq - 3);
        if (dst == "r15_" || dst.find_first_of(" ([") != std::string::npos)
            return {};
        if (rhs == kPopTail) {
            fp = false;
            return dst;
        }
        if (rhs == "u2f(" + kPopTail + ")") {
            fp = true;
            return dst;
        }
        return {};
    };
    std::ostringstream out;
    for (std::size_t i = 0; i < lines.size();) {
        // Gather a run from i: its lines (comments kept with the fast way) and their accesses.
        std::vector<std::size_t> run;
        bool pushes = false;
        std::size_t j = i;
        for (; j < lines.size(); ++j) {
            if (is_comment(lines[j]))
                continue;
            bool fp = false;
            const bool is_push = !push_value(lines[j]).empty();
            const bool is_pop = !is_push && !pop_target(lines[j], fp).empty();
            if (!is_push && !is_pop)
                break;
            if (run.empty())
                pushes = is_push;
            else if (is_push != pushes)
                break;
            run.push_back(j);
        }
        if (run.size() < 2) {
            out << lines[i] << "\n";
            ++i;
            continue;
        }
        const std::size_t end = run.back() + 1;  // comments after the last access stay outside
        const unsigned bytes = 4u * static_cast<unsigned>(run.size());
        // The fast way: the run's first byte once, each word at its offset, r15 moved at the end.
        // Pushes go down from the top (the first at offset bytes - 4), pops up from 0.
        const std::string lo = pushes ? "r15_ - " + std::to_string(bytes) + "u" : "r15_";
        out << "    if (mem_run(" << (pushes ? "ms_, " : "mr_, ") << lo << ", " << bytes
            << "u)) [[likely]] {\n";
        out << "        std::uint8_t* const run_ = mem_run_at(" << (pushes ? "ms_, " : "mr_, ") << lo
            << ");\n";
        unsigned off = pushes ? bytes : 0u;
        for (std::size_t k = i; k < end; ++k) {
            if (is_comment(lines[k])) {
                out << lines[k] << "\n";
                continue;
            }
            bool fp = false;
            if (pushes) {
                off -= 4;
                out << "        st_at(run_, " << off << "u, " << push_value(lines[k]) << ");\n";
            } else {
                const std::string dst = pop_target(lines[k], fp);
                out << "        " << dst << " = " << (fp ? "u2f(ld_at(run_, " : "ld_at(run_, ") << off
                    << (fp ? "u));\n" : "u);\n");
                off += 4;
            }
        }
        out << "        r15_ " << (pushes ? "-= " : "+= ") << bytes << "u;\n";
        out << "    } else {\n";
        for (std::size_t k = i; k < end; ++k)
            if (!is_comment(lines[k]))
                out << lines[k] << "\n";
        out << "    }\n";
        i = end;
    }
    return out.str();
}

std::string apply(const std::string& text) {
    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    bool pending = false;  // inside a function, its bases not declared yet
    bool in_body = false;  // past the function's first label
    bool in_function = false;
    const std::string decl =
        "    [[maybe_unused]] MemView mr_; [[maybe_unused]] MemView ms_; "
        "[[maybe_unused]] std::uint32_t* mq_; mem_bases(m, mr_, ms_, mq_); "
        "[[maybe_unused]] std::uint64_t cy_ = c.cycles;\n";
    static const std::pair<const char*, const char*> kForms[] = {
        {"m.read8(", "rd8(m, mr_, "},          {"m.read16(", "rd16(m, mr_, "},
        {"m.read32(", "rd32(m, mr_, "},        {"m.write8(", "wr8(m, ms_, mq_, "},
        {"m.write16(", "wr16(m, ms_, mq_, "},  {"m.write32(", "wr32(m, ms_, mq_, "},
        {"read_pair(m, ", "rd_pair(m, mr_, "}, {"write_pair(m, ", "wr_pair(m, ms_, mq_, "}};
    static const std::pair<const char*, const char*> kClockForms[] = {
        {"m.read8(", "rd8(c, cy_, m, mr_, "},
        {"m.read16(", "rd16(c, cy_, m, mr_, "},
        {"m.read32(", "rd32(c, cy_, m, mr_, "},
        {"m.write8(", "wr8(c, cy_, m, ms_, mq_, "},
        {"m.write16(", "wr16(c, cy_, m, ms_, mq_, "},
        {"m.write32(", "wr32(c, cy_, m, ms_, mq_, "},
        {"read_pair(m, ", "rd_pair(c, cy_, m, mr_, "},
        {"write_pair(m, ", "wr_pair(c, cy_, m, ms_, mq_, "},
        {"pref_sq(m, ", "pref_sq(c, cy_, m, "}};
    auto accesses = [](std::string& s, const auto& forms) {
        for (const auto& [from, to] : forms) replace_word(s, from, to);
    };
    while (std::getline(in, line)) {
        const bool header = (line.rfind("void fn_", 0) == 0 || line.rfind("static void fn_", 0) == 0) &&
                            line.size() >= 3 && line.compare(line.size() - 3, 3, ") {") == 0;
        if (header) {
            out << line << "\n";
            pending = in_function = true;
            in_body = false;
            continue;
        }
        if (pending) {
            // After the locals' declarations, entry_pr and the entry poll; before anything else.
            if (line.rfind("    if (c.cycles >= c.next_event) {", 0) == 0) {
                out << line << "\n" << decl;
                pending = false;
                continue;
            }
            const bool declaration = line.rfind("    std::uint32_t ", 0) == 0 ||
                                     line.rfind("    float ", 0) == 0 ||
                                     line.rfind("    [[maybe_unused]]", 0) == 0 ||
                                     line.rfind("    //", 0) == 0;
            if (!declaration) {
                out << decl;
                pending = false;
            }
        }
        if (pending || !in_function) {
            out << line << "\n";
            continue;
        }
        if (line == "}") {
            out << "    c.cycles = cy_;\n";
            out << line << "\n";
            in_function = in_body = false;
            continue;
        }
        if (line.rfind("L_", 0) == 0)
            in_body = true;
        if (!in_body) {
            // The prologue's lines (the entry loads, the resume switch) run with the clock just
            // taken: nothing there changes it.
            accesses(line, kForms);
            out << line << "\n";
            continue;
        }
        const std::string rest = without_polls(line, nullptr);
        if (needs_clock(rest)) {
            accesses(line, kForms);
            const std::size_t a = line.find_first_not_of(' ');
            if (a != std::string::npos && (line.compare(a, 5, "case ") == 0 || line.compare(a, 8, "default:") == 0)) {
                // A case of a recovered switch: nothing may go between its labels, and it ends
                // in a jump or a return.
                const std::size_t colon = line.find(':', a);
                line.insert(colon + 1, " c.cycles = cy_;");
                out << line << "\n";
            } else {
                out << "    c.cycles = cy_;\n" << line << "\n    cy_ = c.cycles;\n";
            }
            continue;
        }
        accesses(line, kClockForms);
        // Every mention of the context's clock becomes the local (the block costs, the polls'
        // tests); then each poll gets the context's clock current inside it (the handler reads
        // it, and a return from inside leaves with it) and takes it back before its closing
        // brace. From the last poll back, so earlier positions hold.
        replace_word(line, "c.cycles", "cy_");
        std::vector<std::pair<std::size_t, std::size_t>> polls;
        without_polls(line, &polls, kLocalPollTest);
        for (auto it = polls.rbegin(); it != polls.rend(); ++it) {
            line.insert(it->second, " cy_ = c.cycles; ");
            line.insert(it->first + 1, " c.cycles = cy_;");
        }
        out << line << "\n";
    }
    return stack_runs(out.str());
}

}  // namespace memcache

// ---- non-local returns by a flag ----------------------------------------------------------------
// abi.h, nonlocal_return: a non-local return sets c.unwinding, and every call that can reach guest
// code comes back to emitted code with it set. The code must then return at once, frame by frame,
// to run_guest, which continues at the target. Each such call statement gets
// `if (c.unwinding) return;` after it, unless a `return` follows anyway.
namespace unwind {

bool reaches_guest(const std::string& name) {
    auto ends_with = [&](const char* tail) {
        const std::size_t n = std::char_traits<char>::length(tail);
        return name.size() > n && name.compare(name.size() - n, n, tail) == 0;
    };
    return ends_with("__in") || name == "call_indirect_in" || name == "deliver_irq_in" ||
           name == "trapa_in" || name == "nonlocal_return" || name == "rte";
}

std::string add_checks(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    std::size_t copied = 0;
    for (std::size_t i = 0; i + 4 <= s.size(); ++i) {
        if (s.compare(i, 4, "(c, ") != 0)
            continue;
        std::size_t b = i;
        while (b > 0 && regcache::ident_char(s[b - 1])) --b;
        if (b == i || !reaches_guest(s.substr(b, i - b)))
            continue;
        int depth = 0;
        std::size_t j = i;
        for (; j < s.size(); ++j) {
            if (s[j] == '(')
                ++depth;
            else if (s[j] == ')' && --depth == 0)
                break;
        }
        // A statement only: not a condition (`if (rte(c, m)) return;`) or an argument.
        if (j + 1 >= s.size() || s[j + 1] != ';')
            continue;
        std::size_t n = j + 2;
        while (n < s.size() && (s[n] == ' ' || s[n] == '\n')) ++n;
        if (s.compare(n, 7, "return;") == 0)
            continue;
        out.append(s, copied, j + 2 - copied);
        out += " if (c.unwinding) return;";
        copied = j + 2;
        i = j + 1;
    }
    out.append(s, copied, std::string::npos);
    return out;
}

}  // namespace unwind

// FPSCR precision (PR) and transfer-size (SZ) bits at a program point, as a five-valued
// lattice per bit: a known constant, "same as at function entry", "entry toggled", or unknown.
// The entry-relative values let a function's net effect on the mode be summarised without knowing
// what its callers establish; callers then resolve the summary against their own state.
constexpr std::uint8_t kUnknown = 2;
constexpr std::uint8_t kEntry = 3;
constexpr std::uint8_t kEntryToggled = 4;

struct FpMode {
    std::uint8_t pr = kEntry, sz = kEntry;
    bool operator==(const FpMode&) const = default;
};

std::uint8_t merge_bit(std::uint8_t a, std::uint8_t b) {
    return a == b ? a : kUnknown;
}
FpMode merge(FpMode a, FpMode b) {
    return {merge_bit(a.pr, b.pr), merge_bit(a.sz, b.sz)};
}

std::uint8_t toggle_bit(std::uint8_t v) {
    switch (v) {
        case 0:
            return 1;
        case 1:
            return 0;
        case kEntry:
            return kEntryToggled;
        case kEntryToggled:
            return kEntry;
        default:
            return kUnknown;
    }
}

// Apply a callee's net effect (expressed relative to the callee's entry) to the caller's state.
std::uint8_t apply_effect(std::uint8_t state, std::uint8_t effect) {
    switch (effect) {
        case kEntry:
            return state;
        case kEntryToggled:
            return toggle_bit(state);
        case 0:
        case 1:
            return effect;
        default:
            return kUnknown;
    }
}
FpMode apply_summary(FpMode state, FpMode summary) {
    return {apply_effect(state.pr, summary.pr), apply_effect(state.sz, summary.sz)};
}

// Resolve an entry-relative bit against the concrete entry value (0, 1 or kUnknown).
std::uint8_t resolve_bit(std::uint8_t v, std::uint8_t entry) {
    switch (v) {
        case kEntry:
            return entry;
        case kEntryToggled:
            return entry == kUnknown ? kUnknown : static_cast<std::uint8_t>(entry ^ 1);
        default:
            return v;
    }
}

using Summaries = std::map<std::uint32_t, FpMode>;  // function entry -> net effect

class FunctionEmitter {
public:
    FunctionEmitter(const Image& img, const FunctionSpec& spec, const EmitOptions& opt,
                    const std::map<std::uint32_t, std::string>& known, const Summaries& summaries,
                    EmitResult& result)
        : img_(img),
          spec_(spec),
          opt_(opt),
          known_(known),
          summaries_(summaries),
          result_(result) {}

    // Net effect of this function on PR/SZ from entry to return, for callers' analyses.
    void set_written(const std::set<std::uint32_t>* w) { written_ = w; }

    // Pass 0 of emit_unit: every store whose address is a block-local constant inside the image
    // marks the word(s) it writes, so literal folding leaves them alone.
    void scan_stores(std::set<std::uint32_t>& out) {
        if (blocks_.empty())
            discover();
        for (std::uint32_t start : blocks_) {
            const std::uint32_t stop = block_end_.at(start);
            for (std::uint32_t pc = start; pc < stop; pc += 2) {
                const Instr i = at(pc);
                std::optional<std::uint32_t> base;
                std::uint32_t size = 0;
                std::int32_t disp = 0;
                bool needs_r0 = false;
                switch (i.op) {
                    case Op::MOV_B_S:
                        size = 1;
                        break;
                    case Op::MOV_W_S:
                        size = 2;
                        break;
                    case Op::MOV_L_S:
                    case Op::MOVCA_L:
                        size = 4;
                        break;
                    case Op::MOV_B_SD:
                        size = 1;
                        disp = -1;
                        break;
                    case Op::MOV_W_SD:
                        size = 2;
                        disp = -2;
                        break;
                    case Op::MOV_L_SD:
                        size = 4;
                        disp = -4;
                        break;
                    case Op::MOV_B_S_DISP0:
                        size = 1;
                        disp = i.imm;
                        break;
                    case Op::MOV_W_S_DISP0:
                        size = 2;
                        disp = i.imm * 2;
                        break;
                    case Op::MOV_L_S_DISP:
                        size = 4;
                        disp = i.imm * 4;
                        break;
                    case Op::MOV_B_S_R0:
                        size = 1;
                        needs_r0 = true;
                        break;
                    case Op::MOV_W_S_R0:
                        size = 2;
                        needs_r0 = true;
                        break;
                    case Op::MOV_L_S_R0:
                        size = 4;
                        needs_r0 = true;
                        break;
                    case Op::FMOV_STORE:
                    case Op::FMOV_STORE_R0:
                        size = 8;
                        needs_r0 = i.op == Op::FMOV_STORE_R0;
                        break;
                    case Op::FMOV_STORE_DEC:
                        size = 8;
                        disp = -8;
                        break;
                    default:
                        break;
                }
                if (!size)
                    continue;
                base = constant_at(img_, start, pc, i.n, false);
                if (!base)
                    continue;
                std::uint32_t addr = *base + static_cast<std::uint32_t>(disp);
                if (needs_r0) {
                    const auto r0 = constant_at(img_, start, pc, 0, false);
                    if (!r0)
                        continue;
                    addr += *r0;
                }
                addr = normalise(addr);
                for (std::uint32_t w = addr & ~3u; w < addr + size; w += 4)
                    if (img_.contains(w, 4))
                        out.insert(w);
            }
        }
    }

    FpMode summarise() {
        discover();
        analyse_fp_modes(/*relative=*/true);
        return summary_;
    }

    // Entry-mode clone: this emitter writes only a body named <name><suffix>, compiled with the
    // spec's entry mode taken as known.
    void set_clone_suffix(std::string s) { clone_suffix_ = std::move(s); }
    // The wrapper picks <name>__fpd when the guest's FPSCR PR/SZ bits equal `bits`.
    void set_fast_clone(std::uint32_t bits) { fast_clone_bits_ = bits; }

    std::string run() {
        discover();
        analyse_fp_modes(/*relative=*/false);
        std::ostringstream out;
        const std::string name = spec_.name.empty() ? default_name(spec_.entry) : spec_.name;
        out << "// " << name << (clone_suffix_.empty() ? "" : clone_suffix_) << ": guest "
            << hex(spec_.entry) << ".." << hex(spec_.end) << "\n";
        collect_call_returns();
        // The body is the resume entry: entered with resume_pc = 0 by the plain function, or at a
        // block start / call-return address after a non-local return. entry_pr is what an `rts`
        // must return to; anything else is a non-local return.
        out << (clone_suffix_.empty() && opt_.shards > 1 ? "void " : "static void ") << name
            << (clone_suffix_.empty() ? "__resume" : clone_suffix_)
            << "(Ctx& c, Memory& m, std::uint32_t resume_pc) {\n";
        out << "    [[maybe_unused]] const std::uint32_t entry_pr = c.pr;\n";
        // Poll points record the exact guest pc first: an interrupt handler that switches tasks
        // returns through RTE to SPC, and run_guest re-enters this function there.
        // This poll runs before the resume switch, so on a resumed entry the guest is at
        // resume_pc, not at the function entry. Recording the entry made a task switch taken here
        // come back to SPC = entry and re-run the prologue on the resumed frame's stack.
        out << "    if (c.cycles >= c.next_event) { c.pc = resume_pc ? resume_pc : "
            << hexu(spec_.entry) << "; deliver_irq_in(c, m); }\n";
        out << "    if (resume_pc) {\n        switch (resume_pc) {\n";
        for (std::uint32_t b : blocks_)
            out << "            case " << hexu(b) << ": goto " << label(b) << ";\n";
        for (std::uint32_t r : call_returns_)
            if (!blocks_.count(r))
                out << "            case " << hexu(r) << ": goto " << label(r) << ";\n";
        out << "            default: resume_miss(c, m, resume_pc); return;\n        }\n    }\n";
        for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
            const std::uint32_t start = *it;
            const std::uint32_t stop = block_end_.at(start);
            out << label(start) << ":\n";
            std::uint32_t pc = start;
            pending_cycles_ = 0;
            mode_ = block_mode_.at(start);
            out << "    // block " << hex(start)
                << " fp mode pr=" << (mode_.pr == kUnknown ? "?" : std::to_string(mode_.pr))
                << " sz=" << (mode_.sz == kUnknown ? "?" : std::to_string(mode_.sz)) << "\n";
            while (pc < stop) pc = emit_instruction(out, pc);
            flush_cycles(out);
            // A block ending in a conditional branch still falls through on the not-taken path,
            // so it needs the same continuation as one ending in a plain instruction (a loop
            // whose exit is the next function).
            if (!ends_in_transfer_.at(start) || is_conditional_branch(last_op(start))) {
                auto next = std::next(it);
                if (next == blocks_.end() || *next != stop) {
                    if (known_.count(normalise(stop))) {
                        // Discovery trimmed this function at another entry (a pointer seed
                        // landing one instruction into a prologue, or shared code): control
                        // continues in that function, so tail-call it.
                        out << "    // falls through into the function at " << hex(stop) << "\n";
                        out << "    " << call_target_expr(stop) << " return;\n";
                    } else {
                        // Control continues at `stop`, which no function of this unit starts at:
                        // hand it to the runtime.
                        out << "    // falls off the end of the function range at " << hex(stop)
                            << "\n";
                        out << "    c.pc = " << hexu(stop) << "; call_indirect_in(c, m, " << hexu(stop)
                            << "); return;\n";
                    }
                }
            }
        }
        out << "}\n";
        const std::string text = unwind::add_checks(out.str());
        return memcache::apply(regcache::apply(text));
    }

    // The plain entry every call goes through (after run(), which it does not repeat).
    std::string wrapper() {
        const std::string name = spec_.name.empty() ? default_name(spec_.entry) : spec_.name;
        std::ostringstream out;
        // Non-local returns (abi.h, nonlocal_return): emitted code and the function table call
        // NAME__in, which leaves c.unwinding set for its caller to see; the plain NAME, which
        // native code calls, turns it into the NonLocalReturn exception native frames expect.
        const char* linkage = opt_.shards > 1 ? "void " : "static void ";
        out << linkage << name << "__in(Ctx& c, Memory& m) { ";
        if (!spec_.hook.empty() && spec_.hook_entry)
            out << "if (dream_hook_entry_" << spec_.hook << "(c, m)) return; ";
        if (fast_clone_bits_ != 0xFFFFFFFFu)
            out << "if ((c.fpscr & 0x00180000u) == " << hexu(fast_clone_bits_) << ") " << name
                << "__fpd(c, m, 0); else ";
        out << name << "__resume(c, m, 0); ";
        // An unwound body skips the exit hook.
        if (!spec_.hook.empty() && spec_.hook_exit)
            out << "if (c.unwinding) return; dream_hook_exit_" << spec_.hook << "(c, m); ";
        out << "}\n";
        out << "void " << name << "(Ctx& c, Memory& m) { " << name
            << "__in(c, m); rethrow_unwinding(c); }\n";
        return out.str();
    }

private:
    std::string clone_suffix_;
    std::uint32_t fast_clone_bits_ = 0xFFFFFFFFu;

    Instr at(std::uint32_t a) const { return sh4::decode(img_.read16(a)); }

    Op last_op(std::uint32_t start) const {
        Op op = Op::Invalid;
        for (std::uint32_t pc = start; pc < block_end_.at(start);) {
            const Instr i = at(pc);
            op = i.op;
            pc += sh4::has_delay_slot(i.op) ? 4 : 2;
        }
        return op;
    }
    static bool is_conditional_branch(Op op) {
        return op == Op::BT || op == Op::BF || op == Op::BT_S || op == Op::BF_S;
    }

    // Return addresses of the calls in this function (resume points for non-local returns).
    void collect_call_returns() {
        call_returns_.clear();
        for (std::uint32_t b : blocks_) {
            for (std::uint32_t pc = b; pc < block_end_.at(b);) {
                const Instr i = at(pc);
                const bool delayed = sh4::has_delay_slot(i.op);
                if ((i.op == Op::BSR || i.op == Op::JSR || i.op == Op::BSRF) && in_range(pc + 4) &&
                    !slot_labels_.count(pc + 4))
                    call_returns_.insert(pc + 4);
                pc += delayed ? 4 : 2;
            }
        }
    }
    bool in_range(std::uint32_t a) const { return a >= spec_.entry && a < spec_.end; }

    // Reachable blocks inside [entry, end): follow fallthrough and local branches only, so data
    // after the last RTS (literal pools) is never decoded as code.
    void discover() {
        std::vector<std::uint32_t> work{spec_.entry};
        std::set<std::uint32_t> seen;
        // Addresses that are the delay slot of some branch, and addresses decoding actually began
        // at. A branch may legally target a delay slot, and telling the two apart is what lets
        // decoding continue after such a branch pair instead of stopping at an address that was
        // only ever marked seen in passing.
        std::set<std::uint32_t> delay_slots, starts;
        while (!work.empty()) {
            std::uint32_t pc = work.back();
            work.pop_back();
            if (!in_range(pc))
                continue;
            if (seen.count(pc)) {
                // Already decoded, with one exception. A delay slot is marked seen when its branch
                // is decoded, so a *branch to that slot* finds it seen and is dropped, and the
                // instructions after the branch pair are never decoded at all. Entering through the
                // slot runs that one instruction and then continues after the pair, so that is
                // where decoding has to carry on.
                if (delay_slots.count(pc) && !starts.count(pc)) {
                    labels_.insert(pc);
                    work.push_back(pc + 2);
                }
                continue;
            }
            starts.insert(pc);
            const std::uint32_t start = pc;
            bool transfer = false;
            while (in_range(pc)) {
                if (pc != start && labels_.count(pc))
                    break;
                seen.insert(pc);
                const Instr ins = at(pc);
                const bool delayed = sh4::has_delay_slot(ins.op);
                const std::uint32_t next = pc + (delayed ? 4 : 2);
                if (delayed) {
                    seen.insert(pc + 2);
                    delay_slots.insert(pc + 2);
                }
                bool stop = true;
                switch (ins.op) {
                    case Op::BT:
                    case Op::BF:
                    case Op::BT_S:
                    case Op::BF_S: {
                        const std::uint32_t t = sh4::pcrel_target(ins, pc);
                        if (in_range(t)) {
                            labels_.insert(t);
                            work.push_back(t);
                        }
                        work.push_back(next);
                        break;
                    }
                    case Op::BRA: {
                        const std::uint32_t t = sh4::pcrel_target(ins, pc);
                        if (in_range(t)) {
                            labels_.insert(t);
                            work.push_back(t);
                        }
                        break;
                    }
                    case Op::RTS:
                    case Op::RTE:
                        break;
                    case Op::JMP:
                    case Op::BRAF: {
                        SwitchTable sw;
                        if (recover_switch(img_, start, pc, ins, spec_.entry, spec_.end, sw)) {
                            switches_[pc] = sw;
                            for (std::uint32_t t : sw.targets) {
                                labels_.insert(t);
                                work.push_back(t);
                            }
                        }
                        break;
                    }
                    case Op::Invalid:
                        // Data reached by fallthrough: the block ends with the fault the lowering
                        // emits for the undefined word; nothing after it is code.
                        break;
                    default:
                        stop = false;
                        break;
                }
                pc = next;
                if (stop) {
                    transfer = true;
                    break;
                }
            }
            blocks_.insert(start);
            block_end_[start] = pc;
            ends_in_transfer_[start] = transfer;
        }
        // Labels that land on a delay slot are not block starts: SH-4 lets a branch target the slot
        // instruction of another branch (it then executes as an ordinary instruction and falls
        // through). Those are emitted specially after the owning branch; everything else splits
        // its block at the label.
        std::set<std::uint32_t> boundary_labels;
        // Where control continues after entering through a delay slot. A branch that targets
        // another branch's slot executes that one instruction and then carries on at the
        // instruction *after* the branch pair, which is a path nothing else reaches: the owning
        // branch's own target is elsewhere and no other branch points there. Unless it is made a
        // block of its own, the instructions there are never decoded and never emitted, and the
        // fall-through lands in whichever block the emitter happened to write next (a `bf` that
        // targets the slot of the `bra` that follows it).
        std::set<std::uint32_t> after_slot;
        for (std::uint32_t start : blocks_) {
            const std::uint32_t stop = block_end_[start];
            std::uint32_t pc = start;
            while (pc < stop) {
                const Instr i = at(pc);
                const bool delayed = sh4::has_delay_slot(i.op);
                if (labels_.count(pc))
                    boundary_labels.insert(pc);
                if (delayed && labels_.count(pc + 2)) {
                    slot_labels_.insert(pc + 2);
                    after_slot.insert(pc + 4);
                }
                pc += delayed ? 4 : 2;
            }
        }
        for (std::uint32_t a : after_slot) {
            if (a < spec_.entry || a >= spec_.end || blocks_.count(a))
                continue;
            labels_.insert(a);
            boundary_labels.insert(a);
        }
        for (std::uint32_t l : labels_) {
            if (!boundary_labels.count(l) && !slot_labels_.count(l))
                slot_labels_.insert(l);  // defensive
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
                const std::uint32_t start = *it, stop = block_end_[start];
                for (auto lab = labels_.upper_bound(start); lab != labels_.end() && *lab < stop;
                     ++lab) {
                    if (slot_labels_.count(*lab))
                        continue;
                    const std::uint32_t split = *lab;
                    block_end_[start] = split;
                    const bool t = ends_in_transfer_[start];
                    ends_in_transfer_[start] = false;
                    blocks_.insert(split);
                    block_end_[split] = stop;
                    ends_in_transfer_[split] = t;
                    changed = true;
                    break;
                }
                if (changed)
                    break;
            }
        }
    }

    // ---- FPSCR PR/SZ data-flow --------------------------------------------------------------

    FpMode entry_mode(bool relative) const {
        if (relative)
            return {kEntry, kEntry};
        if (spec_.fpscr_entry == 0xFFFFFFFFu)
            return {kUnknown, kUnknown};
        return {spec_.entry_pr_unknown ? kUnknown
                                       : static_cast<std::uint8_t>((spec_.fpscr_entry >> 19) & 1),
                spec_.entry_sz_unknown ? kUnknown
                                       : static_cast<std::uint8_t>((spec_.fpscr_entry >> 20) & 1)};
    }

public:
    // Every direct call or tail jump to a function of this unit, with the entry-relative FPSCR
    // mode in force at that point. emit_unit joins these over the call graph to learn each callee's
    // entry mode instead of assuming the Katana default (a context-switch helper called under SZ=1
    // was emitted with 4-byte fmovs and mis-restored a task).
    void call_site_modes(std::vector<std::pair<std::uint32_t, FpMode>>& out) {
        discover();
        analyse_fp_modes(/*relative=*/true);
        for (std::uint32_t b : blocks_) {
            FpMode m = block_mode_[b];
            for (std::uint32_t pc = b; pc < block_end_[b]; pc += 2) {
                const Instr i = at(pc);
                std::uint32_t t = 0;
                bool call = false;
                switch (i.op) {
                    case Op::BSR:
                    case Op::BRA:
                        t = sh4::pcrel_target(i, pc);
                        call = true;
                        break;
                    case Op::JSR:
                    case Op::JMP:
                        call = constant_in_register(b, pc, i.m, t, true);
                        break;
                    case Op::BSRF: {
                        std::uint32_t k;
                        call = constant_in_register(b, pc, i.m, k, true);
                        t = pc + 4 + k;
                        break;
                    }
                    default:
                        break;
                }
                if (call) {
                    t = normalise(t);
                    if (known_.count(t) && t != spec_.entry)
                        out.push_back({t, m});
                }
                m = transfer(m, b, pc, i);
            }
        }
    }

private:
    // Constant loaded into Rn just before `pc` within the block, if it can be seen. Recognises
    // the idiom `mov.l @(disp,pc),Rn ; lds Rn,fpscr` (possibly with unrelated instructions in
    // between) and `mov #imm,Rn`. Any other write to Rn in between gives up.
    bool constant_in_register(std::uint32_t block_start, std::uint32_t pc, unsigned reg,
                              std::uint32_t& value, bool alias_or = false) const {
        const auto v = constant_at(img_, block_start, pc, reg, alias_or);
        if (!v)
            return false;
        value = *v;
        return true;
    }

    static bool writes_rn(const Instr& i) {
        switch (i.op) {
            case Op::MOV_B_S:
            case Op::MOV_W_S:
            case Op::MOV_L_S:
            case Op::MOV_B_S_R0:
            case Op::MOV_W_S_R0:
            case Op::MOV_L_S_R0:
            case Op::MOV_L_S_DISP:
            case Op::CMP_EQ:
            case Op::CMP_HS:
            case Op::CMP_GE:
            case Op::CMP_HI:
            case Op::CMP_GT:
            case Op::CMP_STR:
            case Op::TST:
            case Op::DIV0S:
            case Op::MUL_L:
            case Op::MULS_W:
            case Op::MULU_W:
            case Op::DMULS_L:
            case Op::DMULU_L:
            case Op::CMP_PZ:
            case Op::CMP_PL:
            case Op::JSR:
            case Op::JMP:
            case Op::BRAF:
            case Op::BSRF:
            case Op::PREF:
            case Op::OCBI:
            case Op::OCBP:
            case Op::OCBWB:
            case Op::MOVCA_L:
            case Op::LDC_SR:
            case Op::LDC_GBR:
            case Op::LDC_VBR:
            case Op::LDC_SSR:
            case Op::LDC_SPC:
            case Op::LDC_DBR:
            case Op::LDC_SGR:
            case Op::LDC_BANK:
            case Op::LDS_MACH:
            case Op::LDS_MACL:
            case Op::LDS_PR:
            case Op::LDS_FPUL:
            case Op::LDS_FPSCR:
            case Op::TAS_B:
                return false;  // read Rn (or Rm) only
            default:
                return i.n != 0 ||
                       true;  // conservatively: anything else with an n field may write it
        }
    }

    FpMode transfer(FpMode m, std::uint32_t block_start, std::uint32_t pc, const Instr& i) const {
        switch (i.op) {
            case Op::FSCHG:
                m.sz = toggle_bit(m.sz);
                return m;
            case Op::LDS_FPSCR: {
                std::uint32_t v;
                if (constant_in_register(block_start, pc, i.m, v)) {
                    return {static_cast<std::uint8_t>((v >> 19) & 1),
                            static_cast<std::uint8_t>((v >> 20) & 1)};
                }
                return {kUnknown, kUnknown};
            }
            case Op::LDS_L_FPSCR:
                // `lds.l @r15+,fpscr` paired with a `sts.l fpscr,@-r15` in the same function is the
                // prologue/epilogue save-restore idiom: the mode returns to its entry value.
                if (i.m == 15 && saves_fpscr_)
                    return {kEntry, kEntry};
                return {kUnknown, kUnknown};
            case Op::BSR: {
                const std::uint32_t t = sh4::pcrel_target(i, pc);
                return call_effect(m, t, true);
            }
            case Op::JSR: {
                std::uint32_t t;
                if (constant_in_register(block_start, pc, i.m, t))
                    return call_effect(m, t, true);
                return m;  // unknown target: assume the callee preserves the mode
            }
            default:
                return m;
        }
    }

    // Effect of calling `target` on the mode. Known callee: its summary. Unknown callee: identity.
    FpMode call_effect(FpMode m, std::uint32_t target, bool) const {
        auto it = summaries_.find(target);
        if (it == summaries_.end())
            return m;
        return apply_summary(m, it->second);
    }

    // Successors of a block for the mode propagation: fallthrough and in-range branch targets.
    // A target that is a slot label (not a block start) continues in the block containing it.
    void push_successor(std::vector<std::uint32_t>& out, std::uint32_t t) const {
        if (!in_range(t))
            return;
        if (blocks_.count(t)) {
            out.push_back(t);
            return;
        }
        auto it = blocks_.upper_bound(t);
        if (it != blocks_.begin())
            out.push_back(*std::prev(it));
    }

    std::vector<std::uint32_t> successors(std::uint32_t start) const {
        std::vector<std::uint32_t> out;
        const std::uint32_t stop = block_end_.at(start);
        if (!ends_in_transfer_.at(start)) {
            if (blocks_.count(stop))
                out.push_back(stop);
            return out;
        }
        std::uint32_t last = stop - 2;
        Instr li = at(last);
        if (!sh4::is_control_flow(li.op) && last >= start + 2) {
            last -= 2;
            li = at(last);
        }
        switch (li.op) {
            case Op::BT:
            case Op::BF:
            case Op::BT_S:
            case Op::BF_S:
                push_successor(out, sh4::pcrel_target(li, last));
                if (blocks_.count(stop))
                    out.push_back(stop);
                break;
            case Op::BRA:
                push_successor(out, sh4::pcrel_target(li, last));
                break;
            case Op::BRAF:
            case Op::JMP: {
                auto it = switches_.find(last);
                if (it != switches_.end()) {
                    for (std::uint32_t t : it->second.targets) push_successor(out, t);
                }
                break;
            }
            default:
                break;
        }
        return out;
    }

    void analyse_fp_modes(bool relative) {
        saves_fpscr_ = false;
        for (std::uint32_t b : blocks_) {
            for (std::uint32_t pc = b; pc < block_end_[b]; pc += 2) {
                const Instr i = at(pc);
                if (i.op == Op::STS_L_FPSCR && i.n == 15)
                    saves_fpscr_ = true;
            }
        }
        for (std::uint32_t b : blocks_) block_mode_[b] = {kUnknown, kUnknown};
        block_mode_[spec_.entry] = entry_mode(relative);
        std::vector<std::uint32_t> work{spec_.entry};
        std::set<std::uint32_t> reached{spec_.entry};
        bool have_exit = false;
        FpMode exit_state{kUnknown, kUnknown};
        while (!work.empty()) {
            const std::uint32_t b = work.back();
            work.pop_back();
            if (!blocks_.count(b))
                continue;
            FpMode m = block_mode_[b];
            for (std::uint32_t pc = b; pc < block_end_[b]; pc += 2) m = transfer(m, b, pc, at(pc));
            // Exits: returns, and tail jumps to other functions (their effect applies before
            // return).
            if (ends_in_transfer_[b]) {
                const std::uint32_t stop = block_end_[b];
                std::uint32_t last = stop - 2;
                Instr li = at(last);
                if (!sh4::is_control_flow(li.op) && last >= b + 2) {
                    last -= 2;
                    li = at(last);
                }
                FpMode e = m;
                bool is_exit = false;
                switch (li.op) {
                    case Op::RTS:
                    case Op::RTE:
                        is_exit = true;
                        break;
                    case Op::BRA: {
                        const std::uint32_t t = sh4::pcrel_target(li, last);
                        if (!in_range(t)) {
                            e = call_effect(m, t, false);
                            is_exit = true;
                        }
                        break;
                    }
                    case Op::JMP: {
                        std::uint32_t t;
                        if (constant_in_register(b, last, li.m, t))
                            e = call_effect(m, t, false);
                        is_exit = true;
                        break;
                    }
                    case Op::BRAF:
                        break;  // computed jump within the function (switch), not an exit
                    default:
                        break;
                }
                if (is_exit) {
                    exit_state = have_exit ? merge(exit_state, e) : e;
                    have_exit = true;
                }
            }
            for (std::uint32_t succ : successors(b)) {
                const FpMode merged = reached.count(succ) ? merge(block_mode_[succ], m) : m;
                if (!reached.count(succ) || merged != block_mode_[succ]) {
                    block_mode_[succ] = merged;
                    reached.insert(succ);
                    work.push_back(succ);
                }
            }
        }
        summary_ = have_exit ? exit_state : FpMode{kEntry, kEntry};
    }

    void flush_cycles(std::ostringstream& out) {
        if (pending_cycles_) {
            // The charge at the CPU's speed (dream/runtime/sh4/cost.h); the SH-4's own estimate at
            // speed 1.
            out << "    c.cycles += cost(" << pending_cycles_ << ");\n";
            pending_cycles_ = 0;
        }
    }

    // Same alias rule as discovery: a constant target through another RAM alias of the image is
    // the function at the corresponding image address.
    std::uint32_t normalise(std::uint32_t a) const {
        const std::uint32_t phys = a & 0x1FFFFFFFu, base_phys = img_.base & 0x1FFFFFFFu;
        if ((a & 0x1C000000u) == 0x0C000000u && phys >= base_phys &&
            phys < base_phys + img_.bytes.size())
            return img_.base + (phys - base_phys);
        return a;
    }

    std::string call_target_expr(std::uint32_t target) const {
        auto it = known_.find(normalise(target));
        if (it != known_.end())
            return it->second + "__in(c, m);";
        if (const std::string* name = imported(target))
            return *name + "__in(c, m);";
        return "call_indirect_in(c, m, " + hexu(target) + ");";
    }

    // Another unit's function at `target` that this unit may call by name (EmitOptions::imports).
    const std::string* imported(std::uint32_t target) const {
        if (!opt_.imports || known_.count(normalise(target)))
            return nullptr;
        auto it = opt_.imports->find(target);
        return it == opt_.imports->end() ? nullptr : &it->second;
    }

    std::string irq_check_if_backedge(std::uint32_t target, std::uint32_t pc) const {
        // Braced: GCC's -Wmisleading-indentation rejects `if (x) f(); goto L;` on one line.
        return target <= pc ? "if (c.cycles >= c.next_event) { c.pc = " + hexu(target) +
                                  "; deliver_irq_in(c, m); } "
                            : "";
    }

    std::uint32_t emit_instruction(std::ostringstream& out, std::uint32_t pc) {
        const Instr ins = at(pc);
        out << "    // " << hex(pc) << ": " << sh4::format(ins, pc) << "\n";
        mode_ = transfer(mode_, current_block_start(pc), pc, ins);
        if (sh4::has_delay_slot(ins.op)) {
            emit_delayed(out, pc, ins);
            return pc + 4;
        }
        if (ins.op == Op::BT || ins.op == Op::BF) {
            ++pending_cycles_;
            flush_cycles(out);
            const std::uint32_t t = sh4::pcrel_target(ins, pc);
            const std::string cond = ins.op == Op::BT ? "c.t" : "!c.t";
            if (in_range(t)) {
                out << "    if (" << cond << ") { c.cycles += 1; " << irq_check_if_backedge(t, pc)
                    << "goto " << label(t) << "; }\n";
            } else {
                out << "    if (" << cond << ") { c.cycles += 1; " << call_target_expr(t)
                    << " return; }\n";
            }
            return pc + 2;
        }
        // A run of `ROTCL Ra; DIV1 Rm,Rn` pairs, the compilers' unrolled division, as one call
        // (ops.h rotcl_div1_run): the same registers, T, Q and M as the single steps. Within one
        // block only.
        if (ins.op == Op::ROTCL) {
            const Instr d = at(pc + 2);
            const unsigned a = ins.n, rm = d.m, rn = d.n;
            unsigned pairs = 0;
            for (std::uint32_t p = pc; in_range(p + 2); p += 4) {
                const Instr r = at(p), v = at(p + 2);
                if (r.op != Op::ROTCL || r.n != a || v.op != Op::DIV1 || v.m != rm || v.n != rn ||
                    (p != pc && labels_.count(p)) || labels_.count(p + 2))
                    break;
                ++pairs;
            }
            if (pairs >= 2 && a != rm && a != rn && rm != rn) {
                for (unsigned i = 1; i < 2 * pairs; ++i) {
                    out << "    // " << hex(pc + 2 * i) << ": " << sh4::format(at(pc + 2 * i), pc + 2 * i) << "\n";
                }
                pending_cycles_ += 2 * pairs;
                out << "    rotcl_div1_run(c, " << R(a) << ", " << R(rn) << ", " << R(rm) << ", " << pairs
                    << "u);\n";
                return pc + 4 * pairs;
            }
        }
        ++pending_cycles_;
        out << "    " << lower(pc, ins) << "\n";
        return pc + 2;
    }

    void emit_delayed(std::ostringstream& out, std::uint32_t pc, const Instr& ins) {
        const Instr slot = at(pc + 2);
        const bool illegal_slot =
            sh4::has_delay_slot(slot.op) || slot.op == Op::BT || slot.op == Op::BF;
        const std::string slot_code = illegal_slot
                                          ? "unimplemented(c, " + hexu(slot.raw) + ", " +
                                                hexu(pc + 2) + "); // illegal slot instruction"
                                          : lower(pc + 2, slot);
        auto emit_slot = [&] {
            out << "        // " << hex(pc + 2) << ": " << sh4::format(slot, pc + 2)
                << " (delay slot)\n";
            out << "        " << slot_code << "\n";
        };
        pending_cycles_ += 2;
        flush_cycles(out);
        out << "    {\n";
        switch (ins.op) {
            case Op::BT_S:
            case Op::BF_S: {
                const std::uint32_t t = sh4::pcrel_target(ins, pc);
                out << "        const bool taken = " << (ins.op == Op::BT_S ? "c.t" : "!c.t")
                    << ";\n";
                emit_slot();
                if (in_range(t)) {
                    out << "        if (taken) { " << irq_check_if_backedge(t, pc) << "goto "
                        << label(t) << "; }\n";
                } else {
                    out << "        if (taken) { " << call_target_expr(t) << " return; }\n";
                }
                break;
            }
            case Op::BRA: {
                const std::uint32_t t = sh4::pcrel_target(ins, pc);
                emit_slot();
                if (in_range(t)) {
                    out << "        " << irq_check_if_backedge(t, pc) << "goto " << label(t)
                        << ";\n";
                } else {
                    out << "        " << call_target_expr(t) << " return;\n";
                }
                break;
            }
            case Op::BSR: {
                const std::uint32_t t = sh4::pcrel_target(ins, pc);
                out << "        c.pr = " << hexu(pc + 4) << ";\n";
                emit_slot();
                out << "        c.pc = " << hexu(pc) << "; " << call_target_expr(t) << "\n";
                break;
            }
            case Op::BSRF:
            case Op::JSR: {
                // A target loaded from a literal (or immediate) in this block is called directly
                // when it is a function of this unit, or one it imports; otherwise the register
                // value goes through the function table at run time.
                std::uint32_t k = 0;
                const bool constant = constant_in_register(current_block_start(pc), pc, ins.m, k);
                const std::uint32_t direct = normalise(ins.op == Op::BSRF ? pc + 4 + k : k);
                if (constant && (known_.count(direct) || imported(direct))) {
                    out << "        c.pr = " << hexu(pc + 4) << ";\n";
                    emit_slot();
                    out << "        c.pc = " << hexu(pc) << "; " << call_target_expr(direct)
                        << "\n";
                    break;
                }
                if (ins.op == Op::BSRF)
                    out << "        const std::uint32_t target = " << hexu(pc + 4) << " + "
                        << R(ins.m) << ";\n";
                else
                    out << "        const std::uint32_t target = " << R(ins.m) << ";\n";
                out << "        c.pr = " << hexu(pc + 4) << ";\n";
                emit_slot();
                out << "        c.pc = " << hexu(pc) << "; call_indirect_in(c, m, target);\n";
                break;
            }
            case Op::BRAF:
            case Op::JMP: {
                if (ins.op == Op::BRAF)
                    out << "        const std::uint32_t target = " << hexu(pc + 4) << " + "
                        << R(ins.m) << ";\n";
                else
                    out << "        const std::uint32_t target = " << R(ins.m) << ";\n";
                emit_slot();
                auto it = switches_.find(pc);
                if (it != switches_.end()) {
                    out << "        switch (target) {\n";
                    std::set<std::uint32_t> seen;
                    for (std::uint32_t t : it->second.targets) {
                        if (!seen.insert(t).second)
                            continue;
                        out << "            case " << hexu(t) << ": "
                            << irq_check_if_backedge(t, pc) << "goto " << label(t) << ";\n";
                    }
                    out << "            default: c.pc = " << hexu(pc)
                        << "; call_indirect_in(c, m, target); return;\n";
                    out << "        }\n";
                } else {
                    out << "        c.pc = " << hexu(pc)
                        << "; call_indirect_in(c, m, target); return;\n";
                }
                break;
            }
            case Op::RTS:
                // PR is read before the slot. A return to anywhere but the entry PR is a
                // non-local return (setjmp/longjmp, a task switch): the host frames are dropped
                // and run_guest re-enters the target. A resumed body returns to run_guest anyway.
                out << "        const std::uint32_t rts_target = c.pr;\n";
                emit_slot();
                out << "        if (!resume_pc && rts_target != entry_pr) "
                       "nonlocal_return(c, m, rts_target);\n";
                out << "        return;\n";
                break;
            case Op::RTE:
                emit_slot();
                // Closing a delivered exception: unwind to the delivery point. Otherwise the RTE
                // is a jump and control continues at SPC.
                out << "        c.pc = " << hexu(pc)
                    << "; if (rte(c, m)) return; nonlocal_return(c, m, c.pc);\n";
                break;
            default:
                out << "        unimplemented(c, " << hexu(ins.raw) << ", " << hexu(pc) << ");\n";
                break;
        }
        out << "    }\n";
        if (slot_labels_.count(pc + 2) && blocks_.count(pc + 2)) {
            // The slot is also a block of its own: decoding reached it as a branch target before
            // this branch marked it a slot. That block already runs the slot instruction for
            // direct entries and carries on at pc+4, so no copy is emitted (it would repeat the
            // label). A path that falls through this branch (bt/s or bf/s not taken, a call that
            // returns) has run the slot already and must not run into that block again.
            const bool falls_through = ins.op == Op::BT_S || ins.op == Op::BF_S ||
                                       ins.op == Op::BSR || ins.op == Op::BSRF ||
                                       ins.op == Op::JSR;
            if (falls_through && blocks_.count(pc + 4))
                out << "    goto " << label(pc + 4) << ";\n";
        } else if (slot_labels_.count(pc + 2)) {
            // Another branch targets this slot: entering here executes the slot instruction as an
            // ordinary instruction and falls through to pc+4. The not-taken path of the owning
            // branch skips this copy.
            out << "    goto " << label(pc) << "_skip;\n";
            out << label(pc + 2) << ":\n";
            out << "    " << slot_code << "\n";
            out << label(pc) << "_skip:;\n";
        }
        if (call_returns_.count(pc + 4) && !blocks_.count(pc + 4))
            out << label(pc + 4) << ":;\n";  // resume point after the call
    }

    // A pool word some instruction in the unit stores to (a run-time patched slot, like the
    // program-entry address the Katana stub writes before jumping) must be read at run time.
    bool patched(std::uint32_t addr) const { return written_ && written_->count(addr & ~3u); }
    std::string literal32(std::uint32_t addr) const {
        if (opt_.fold_literals && img_.contains(addr, 4) && !patched(addr))
            return hexu(img_.read32(addr));
        return "m.read32(" + hexu(addr) + ")";
    }
    std::string literal16(std::uint32_t addr) const {
        if (opt_.fold_literals && img_.contains(addr, 2) && !patched(addr)) {
            const std::int32_t v = static_cast<std::int16_t>(img_.read16(addr));
            return U32(I(v));
        }
        return U32("static_cast<std::int16_t>(m.read16(" + hexu(addr) + "))");
    }

    static std::string ldsx(const std::string& r, const std::string& expr, unsigned bytes,
                            const std::string& cast) {
        // load with sign extension: r = (uint32)(int32)(castT)(expr)
        (void)bytes;
        return r + " = " + U32(S32("static_cast<" + cast + ">(" + expr + ")")) + ";";
    }

    std::uint32_t current_block_start(std::uint32_t pc) const {
        auto it = blocks_.upper_bound(pc);
        return it == blocks_.begin() ? spec_.entry : *std::prev(it);
    }

    // FP instruction under a known or unknown precision/size mode. When the mode is unknown the two
    // lowerings are emitted behind a runtime test of FPSCR.
    std::string lower_fp(const Instr& ins) {
        const bool needs_pr = pr_dependent(ins.op), needs_sz = sz_dependent(ins.op);
        const FpMode entry = entry_mode(false);
        const std::uint8_t pr = resolve_bit(mode_.pr, entry.pr),
                           sz = resolve_bit(mode_.sz, entry.sz);
        if ((needs_pr && pr == kUnknown) || (needs_sz && sz == kUnknown)) {
            ++result_.fp_runtime_branches;
            if (needs_pr) {
                return "if (c.fpscr & FPSCR_PR) { " +
                       lower_fp_mode(ins, 1, mode_.sz == kUnknown ? 0 : mode_.sz) + " } else { " +
                       lower_fp_mode(ins, 0, mode_.sz == kUnknown ? 0 : mode_.sz) + " }";
            }
            return "if (c.fpscr & FPSCR_SZ) { " + lower_fp_mode(ins, 0, 1) + " } else { " +
                   lower_fp_mode(ins, 0, 0) + " }";
        }
        return lower_fp_mode(ins, mode_.pr == kUnknown ? 0 : mode_.pr,
                             mode_.sz == kUnknown ? 0 : mode_.sz);
    }

    static bool pr_dependent(Op op) {
        switch (op) {
            case Op::FADD:
            case Op::FSUB:
            case Op::FMUL:
            case Op::FDIV:
            case Op::FCMP_EQ:
            case Op::FCMP_GT:
            case Op::FLOAT:
            case Op::FTRC:
            case Op::FSQRT:
                return true;
            default:
                return false;
        }
    }
    static bool sz_dependent(Op op) {
        switch (op) {
            case Op::FMOV:
            case Op::FMOV_LOAD:
            case Op::FMOV_STORE:
            case Op::FMOV_LOAD_INC:
            case Op::FMOV_STORE_DEC:
            case Op::FMOV_LOAD_R0:
            case Op::FMOV_STORE_R0:
                return true;
            default:
                return false;
        }
    }

    std::string lower_fp_mode(const Instr& ins, unsigned pr, unsigned sz) {
        const unsigned n = ins.n, m = ins.m;
        const std::string rn = R(n), rm = R(m), r0 = R(0);
        const std::string frn = "c.fr[" + std::to_string(n) + "]",
                          frm = "c.fr[" + std::to_string(m) + "]";
        const std::string drn = "get_dr(c, " + std::to_string(n & ~1u) + ")";
        const std::string drm = "get_dr(c, " + std::to_string(m & ~1u) + ")";
        auto set_drn = [&](const std::string& v) {
            return "set_dr(c, " + std::to_string(n & ~1u) + ", " + v + ");";
        };
        switch (ins.op) {
            case Op::FLDI0:
                return frn + " = 0.0f;";
            case Op::FLDI1:
                return frn + " = 1.0f;";
            case Op::FLDS:
                return "c.fpul = f2u(" + frn + ");";
            case Op::FSTS:
                return frn + " = u2f(c.fpul);";
            case Op::FABS:
                return frn + " = u2f(f2u(" + frn + ") & 0x7fffffffu);";
            case Op::FNEG:
                return frn + " = u2f(f2u(" + frn + ") ^ 0x80000000u);";
            case Op::FADD:
                return pr ? set_drn(drn + " + " + drm) : frn + " += " + frm + ";";
            case Op::FSUB:
                return pr ? set_drn(drn + " - " + drm) : frn + " -= " + frm + ";";
            case Op::FMUL:
                return pr ? set_drn(drn + " * " + drm) : frn + " *= " + frm + ";";
            case Op::FDIV:
                return pr ? set_drn(drn + " / " + drm) : frn + " /= " + frm + ";";
            case Op::FCMP_EQ:
                return "c.t = " + (pr ? drn + " == " + drm : frn + " == " + frm) + ";";
            case Op::FCMP_GT:
                return "c.t = " + (pr ? drn + " > " + drm : frn + " > " + frm) + ";";
            case Op::FMAC:
                return frn + " = fmac(c.fr[0], " + frm + ", " + frn + ");";
            case Op::FLOAT:
                return pr ? set_drn("static_cast<double>(static_cast<std::int32_t>(c.fpul))")
                          : frn + " = static_cast<float>(static_cast<std::int32_t>(c.fpul));";
            case Op::FTRC:
                return "c.fpul = ftrc(" + (pr ? drn : frn) + ");";
            case Op::FSQRT:
                return pr ? set_drn("std::sqrt(" + drn + ")") : frn + " = std::sqrt(" + frn + ");";
            case Op::FSRRA:
                return frn + " = 1.0f / std::sqrt(" + frn + ");";
            case Op::FCNVSD:
                return set_drn("static_cast<double>(u2f(c.fpul))");
            case Op::FCNVDS:
                return "c.fpul = f2u(static_cast<float>(" + drn + "));";
            case Op::FSCA:
                return "fsca(c, " + std::to_string(n & ~1u) + ");";
            case Op::FIPR:
                return "fipr(c, " + std::to_string(m) + ", " + std::to_string(n) + ");";
            case Op::FTRV:
                return "ftrv(c, " + std::to_string(n) + ");";
            case Op::FMOV:
                if (!sz && n == m)
                    return ";  // a move to itself";
                return sz ? "fmov_pair_load(c, " + std::to_string(n) + ", fmov_pair_store(c, " +
                                std::to_string(m) + "));"
                          : frn + " = " + frm + ";";
            case Op::FMOV_LOAD:
                return sz ? "fmov_pair_load(c, " + std::to_string(n) + ", read_pair(m, " + rm +
                                "));"
                          : frn + " = u2f(m.read32(" + rm + "));";
            case Op::FMOV_LOAD_INC:
                return sz ? "fmov_pair_load(c, " + std::to_string(n) + ", read_pair(m, " + rm +
                                ")); " + rm + " += 8;"
                          : frn + " = u2f(m.read32(" + rm + ")); " + rm + " += 4;";
            case Op::FMOV_LOAD_R0:
                return sz ? "fmov_pair_load(c, " + std::to_string(n) + ", read_pair(m, " + rm +
                                " + " + r0 + "));"
                          : frn + " = u2f(m.read32(" + rm + " + " + r0 + "));";
            case Op::FMOV_STORE:
                return sz ? "write_pair(m, " + rn + ", fmov_pair_store(c, " + std::to_string(m) +
                                "));"
                          : "m.write32(" + rn + ", f2u(" + frm + "));";
            case Op::FMOV_STORE_DEC:
                return sz ? rn + " -= 8; write_pair(m, " + rn + ", fmov_pair_store(c, " +
                                std::to_string(m) + "));"
                          : rn + " -= 4; m.write32(" + rn + ", f2u(" + frm + "));";
            case Op::FMOV_STORE_R0:
                return sz ? "write_pair(m, " + rn + " + " + r0 + ", fmov_pair_store(c, " +
                                std::to_string(m) + "));"
                          : "m.write32(" + rn + " + " + r0 + ", f2u(" + frm + "));";
            default:
                return "unimplemented(c, " + hexu(ins.raw) + ", 0u);";
        }
    }

    std::string lower(std::uint32_t pc, const Instr& ins) {
        const unsigned n = ins.n, m = ins.m;
        const std::int32_t imm = ins.imm;
        const std::string rn = R(n), rm = R(m), r0 = R(0);
        const std::string d = std::to_string(imm) + "u", d2 = std::to_string(imm * 2) + "u",
                          d4 = std::to_string(imm * 4) + "u";
        const std::string bank = "c.r_bank[" + std::to_string(ins.bank) + "]";
        switch (ins.op) {
            case Op::MOV_I:
                return rn + " = " + U32(I(imm)) + ";";
            case Op::MOV:
                if (ins.n == ins.m)
                    return ";  // a move to itself";
                return rn + " = " + rm + ";";
            case Op::MOV_W_PCREL:
                return rn + " = " + literal16(sh4::pcrel_target(ins, pc)) + ";";
            case Op::MOV_L_PCREL:
                return rn + " = " + literal32(sh4::pcrel_target(ins, pc)) + ";";
            case Op::MOVA:
                return r0 + " = " + hexu(sh4::pcrel_target(ins, pc)) + ";";
            case Op::MOVT:
                return rn + " = c.t;";
            case Op::MOV_B_S:
                return "m.write8(" + rn + ", static_cast<std::uint8_t>(" + rm + "));";
            case Op::MOV_W_S:
                return "m.write16(" + rn + ", static_cast<std::uint16_t>(" + rm + "));";
            case Op::MOV_L_S:
                return "m.write32(" + rn + ", " + rm + ");";
            case Op::MOV_B_L:
                return ldsx(rn, "m.read8(" + rm + ")", 1, "std::int8_t");
            case Op::MOV_W_L:
                return ldsx(rn, "m.read16(" + rm + ")", 2, "std::int16_t");
            case Op::MOV_L_L:
                return rn + " = m.read32(" + rm + ");";
            case Op::MOV_B_SD:
                return rn + " -= 1; m.write8(" + rn + ", static_cast<std::uint8_t>(" + rm + "));";
            case Op::MOV_W_SD:
                return rn + " -= 2; m.write16(" + rn + ", static_cast<std::uint16_t>(" + rm + "));";
            case Op::MOV_L_SD:
                return rn + " -= 4; m.write32(" + rn + ", " + rm + ");";
            case Op::MOV_B_LI:
                return ldsx(rn, "m.read8(" + rm + ")", 1, "std::int8_t") +
                       (n != m ? " " + rm + " += 1;" : "");
            case Op::MOV_W_LI:
                return ldsx(rn, "m.read16(" + rm + ")", 2, "std::int16_t") +
                       (n != m ? " " + rm + " += 2;" : "");
            case Op::MOV_L_LI:
                return rn + " = m.read32(" + rm + ");" + (n != m ? " " + rm + " += 4;" : "");
            case Op::MOV_L_S_DISP:
                return "m.write32(" + rn + " + " + d4 + ", " + rm + ");";
            case Op::MOV_L_L_DISP:
                return rn + " = m.read32(" + rm + " + " + d4 + ");";
            case Op::MOV_B_S_DISP0:
                return "m.write8(" + rm + " + " + d + ", static_cast<std::uint8_t>(" + r0 + "));";
            case Op::MOV_W_S_DISP0:
                return "m.write16(" + rm + " + " + d2 + ", static_cast<std::uint16_t>(" + r0 +
                       "));";
            case Op::MOV_B_L_DISP0:
                return ldsx(r0, "m.read8(" + rm + " + " + d + ")", 1, "std::int8_t");
            case Op::MOV_W_L_DISP0:
                return ldsx(r0, "m.read16(" + rm + " + " + d2 + ")", 2, "std::int16_t");
            case Op::MOV_B_S_R0:
                return "m.write8(" + rn + " + " + r0 + ", static_cast<std::uint8_t>(" + rm + "));";
            case Op::MOV_W_S_R0:
                return "m.write16(" + rn + " + " + r0 + ", static_cast<std::uint16_t>(" + rm +
                       "));";
            case Op::MOV_L_S_R0:
                return "m.write32(" + rn + " + " + r0 + ", " + rm + ");";
            case Op::MOV_B_L_R0:
                return ldsx(rn, "m.read8(" + rm + " + " + r0 + ")", 1, "std::int8_t");
            case Op::MOV_W_L_R0:
                return ldsx(rn, "m.read16(" + rm + " + " + r0 + ")", 2, "std::int16_t");
            case Op::MOV_L_L_R0:
                return rn + " = m.read32(" + rm + " + " + r0 + ");";
            case Op::MOV_B_S_GBR:
                return "m.write8(c.gbr + " + d + ", static_cast<std::uint8_t>(" + r0 + "));";
            case Op::MOV_W_S_GBR:
                return "m.write16(c.gbr + " + d2 + ", static_cast<std::uint16_t>(" + r0 + "));";
            case Op::MOV_L_S_GBR:
                return "m.write32(c.gbr + " + d4 + ", " + r0 + ");";
            case Op::MOV_B_L_GBR:
                return ldsx(r0, "m.read8(c.gbr + " + d + ")", 1, "std::int8_t");
            case Op::MOV_W_L_GBR:
                return ldsx(r0, "m.read16(c.gbr + " + d2 + ")", 2, "std::int16_t");
            case Op::MOV_L_L_GBR:
                return r0 + " = m.read32(c.gbr + " + d4 + ");";
            case Op::SWAP_B:
                return rn + " = (" + rm + " & 0xffff0000u) | ((" + rm + " & 0xffu) << 8) | ((" +
                       rm + " >> 8) & 0xffu);";
            case Op::SWAP_W:
                return rn + " = (" + rm + " << 16) | (" + rm + " >> 16);";
            case Op::XTRCT:
                return rn + " = (" + rm + " << 16) | (" + rn + " >> 16);";
            case Op::ADD:
                return rn + " += " + rm + ";";
            case Op::ADD_I:
                return rn + " += " + U32(I(imm)) + ";";
            case Op::ADDC:
                return rn + " = addc(c, " + rn + ", " + rm + ");";
            case Op::ADDV:
                return rn + " = addv(c, " + rn + ", " + rm + ");";
            case Op::CMP_EQ_I:
                return "c.t = " + r0 + " == " + U32(I(imm)) + ";";
            // A register compared with itself is how SH-4 code sets or clears T in one
            // instruction (`cmp/hi r15,r15`). Emitted literally it is
            // `c.r[15] > c.r[15]`, which every compiler warns about and -Werror then rejects, so
            // a title using the idiom would not build at all. The answer the hardware gives is a
            // constant, so emit the constant.
            case Op::CMP_EQ:
                if (ins.n == ins.m)
                    return "c.t = true;  // a register compared with itself";
                return "c.t = " + rn + " == " + rm + ";";
            case Op::CMP_HS:
                if (ins.n == ins.m)
                    return "c.t = true;  // a register compared with itself";
                return "c.t = " + rn + " >= " + rm + ";";
            case Op::CMP_GE:
                if (ins.n == ins.m)
                    return "c.t = true;  // a register compared with itself";
                return "c.t = " + S32(rn) + " >= " + S32(rm) + ";";
            case Op::CMP_HI:
                if (ins.n == ins.m)
                    return "c.t = false;  // a register compared with itself";
                return "c.t = " + rn + " > " + rm + ";";
            case Op::CMP_GT:
                if (ins.n == ins.m)
                    return "c.t = false;  // a register compared with itself";
                return "c.t = " + S32(rn) + " > " + S32(rm) + ";";
            case Op::CMP_PZ:
                return "c.t = " + S32(rn) + " >= 0;";
            case Op::CMP_PL:
                return "c.t = " + S32(rn) + " > 0;";
            case Op::CMP_STR:
                return "c.t = cmp_str(" + rn + ", " + rm + ");";
            case Op::DIV1:
                return rn + " = div1(c, " + rn + ", " + rm + ");";
            case Op::DIV0S:
                return "div0s(c, " + rn + ", " + rm + ");";
            case Op::DIV0U:
                return "div0u(c);";
            case Op::DMULS_L:
                return "dmuls(c, " + rn + ", " + rm + ");";
            case Op::DMULU_L:
                return "dmulu(c, " + rn + ", " + rm + ");";
            case Op::DT:
                return rn + " -= 1; c.t = " + rn + " == 0;";
            case Op::EXTS_B:
                return rn + " = " + U32(S32("static_cast<std::int8_t>(" + rm + ")")) + ";";
            case Op::EXTS_W:
                return rn + " = " + U32(S32("static_cast<std::int16_t>(" + rm + ")")) + ";";
            case Op::EXTU_B:
                return rn + " = " + rm + " & 0xffu;";
            case Op::EXTU_W:
                return rn + " = " + rm + " & 0xffffu;";
            case Op::MAC_L:
                return "{ const std::int32_t a = " + S32("m.read32(" + rn + ")") + "; " + rn +
                       " += 4; const std::int32_t b = " + S32("m.read32(" + rm + ")") + "; " + rm +
                       " += 4; mac_l(c, a, b); }";
            case Op::MAC_W:
                return "{ const std::int16_t a = static_cast<std::int16_t>(m.read16(" + rn +
                       ")); " + rn +
                       " += 2; const std::int16_t b = static_cast<std::int16_t>(m.read16(" + rm +
                       ")); " + rm + " += 2; mac_w(c, a, b); }";
            case Op::MUL_L:
                return "c.macl = " + rn + " * " + rm + ";";
            case Op::MULS_W:
                return "c.macl = " +
                       U32(S32("static_cast<std::int16_t>(" + rn + ")") + " * " +
                           S32("static_cast<std::int16_t>(" + rm + ")")) +
                       ";";
            case Op::MULU_W:
                return "c.macl = (" + rn + " & 0xffffu) * (" + rm + " & 0xffffu);";
            case Op::NEG:
                return rn + " = 0u - " + rm + ";";
            case Op::NEGC:
                return rn + " = negc(c, " + rm + ");";
            case Op::SUB:
                return rn + " -= " + rm + ";";
            case Op::SUBC:
                return rn + " = subc(c, " + rn + ", " + rm + ");";
            case Op::SUBV:
                return rn + " = subv(c, " + rn + ", " + rm + ");";
            case Op::AND:
                if (ins.n == ins.m)
                    return ";  // a register combined with itself";
                return rn + " &= " + rm + ";";
            case Op::AND_I:
                return r0 + " &= " + d + ";";
            case Op::AND_B_GBR:
                return "{ const std::uint32_t a = c.gbr + " + r0 +
                       "; m.write8(a, static_cast<std::uint8_t>(m.read8(a) & " + d + ")); }";
            case Op::NOT:
                return rn + " = ~" + rm + ";";
            case Op::OR:
                if (ins.n == ins.m)
                    return ";  // a register combined with itself";
                return rn + " |= " + rm + ";";
            case Op::OR_I:
                return r0 + " |= " + d + ";";
            case Op::OR_B_GBR:
                return "{ const std::uint32_t a = c.gbr + " + r0 +
                       "; m.write8(a, static_cast<std::uint8_t>(m.read8(a) | " + d + ")); }";
            case Op::TAS_B:
                return "{ const std::uint8_t v = m.read8(" + rn + "); c.t = v == 0; m.write8(" +
                       rn + ", static_cast<std::uint8_t>(v | 0x80u)); }";
            case Op::TST:
                return "c.t = (" + rn + " & " + rm + ") == 0;";
            case Op::TST_I:
                return "c.t = (" + r0 + " & " + d + ") == 0;";
            case Op::TST_B_GBR:
                return "c.t = (m.read8(c.gbr + " + r0 + ") & " + d + ") == 0;";
            case Op::XOR:
                return rn + " ^= " + rm + ";";
            case Op::XOR_I:
                return r0 + " ^= " + d + ";";
            case Op::XOR_B_GBR:
                return "{ const std::uint32_t a = c.gbr + " + r0 +
                       "; m.write8(a, static_cast<std::uint8_t>(m.read8(a) ^ " + d + ")); }";
            case Op::ROTL:
                return "c.t = " + rn + " >> 31; " + rn + " = (" + rn + " << 1) | c.t;";
            case Op::ROTR:
                return "c.t = " + rn + " & 1u; " + rn + " = (" + rn + " >> 1) | (c.t << 31);";
            case Op::ROTCL:
                return rn + " = rotcl(c, " + rn + ");";
            case Op::ROTCR:
                return rn + " = rotcr(c, " + rn + ");";
            case Op::SHAD:
                return rn + " = shad(" + rn + ", " + rm + ");";
            case Op::SHLD:
                return rn + " = shld(" + rn + ", " + rm + ");";
            case Op::SHAL:
            case Op::SHLL:
                return "c.t = " + rn + " >> 31; " + rn + " <<= 1;";
            case Op::SHAR:
                return "c.t = " + rn + " & 1u; " + rn + " = " + U32(S32(rn) + " >> 1") + ";";
            case Op::SHLR:
                return "c.t = " + rn + " & 1u; " + rn + " >>= 1;";
            case Op::SHLL2:
                return rn + " <<= 2;";
            case Op::SHLL8:
                return rn + " <<= 8;";
            case Op::SHLL16:
                return rn + " <<= 16;";
            case Op::SHLR2:
                return rn + " >>= 2;";
            case Op::SHLR8:
                return rn + " >>= 8;";
            case Op::SHLR16:
                return rn + " >>= 16;";
            case Op::CLRMAC:
                return "c.mach = 0; c.macl = 0;";
            case Op::CLRS:
                return "c.sr &= ~SR_S;";
            case Op::CLRT:
                return "c.t = 0;";
            case Op::SETS:
                return "c.sr |= SR_S;";
            case Op::SETT:
                return "c.t = 1;";
            case Op::NOP:
                return "/* nop */";
            case Op::LDC_SR:
                return "write_sr(c, " + rm + ");";
            case Op::LDC_GBR:
                return "c.gbr = " + rm + ";";
            case Op::LDC_VBR:
                return "c.vbr = " + rm + ";";
            case Op::LDC_SSR:
                return "c.ssr = " + rm + ";";
            case Op::LDC_SPC:
                return "c.spc = " + rm + ";";
            case Op::LDC_DBR:
                return "c.dbr = " + rm + ";";
            case Op::LDC_BANK:
                return bank + " = " + rm + ";";
            case Op::LDC_L_SR:
                return "write_sr(c, m.read32(" + rm + ")); " + rm + " += 4;";
            case Op::LDC_L_GBR:
                return "c.gbr = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDC_L_VBR:
                return "c.vbr = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDC_L_SSR:
                return "c.ssr = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDC_L_SPC:
                return "c.spc = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDC_L_DBR:
                return "c.dbr = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDC_L_BANK:
                return bank + " = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDS_MACH:
                return "c.mach = " + rm + ";";
            case Op::LDS_MACL:
                return "c.macl = " + rm + ";";
            case Op::LDS_PR:
                return "c.pr = " + rm + ";";
            case Op::LDS_L_MACH:
                return "c.mach = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDS_L_MACL:
                return "c.macl = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDS_L_PR:
                return "c.pr = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::STC_SR:
                return rn + " = read_sr(c);";
            case Op::STC_GBR:
                return rn + " = c.gbr;";
            case Op::STC_VBR:
                return rn + " = c.vbr;";
            case Op::STC_SSR:
                return rn + " = c.ssr;";
            case Op::STC_SPC:
                return rn + " = c.spc;";
            case Op::STC_SGR:
                return rn + " = c.sgr;";
            case Op::STC_DBR:
                return rn + " = c.dbr;";
            case Op::STC_BANK:
                return rn + " = " + bank + ";";
            case Op::STC_L_SR:
                return rn + " -= 4; m.write32(" + rn + ", read_sr(c));";
            case Op::STC_L_GBR:
                return rn + " -= 4; m.write32(" + rn + ", c.gbr);";
            case Op::STC_L_VBR:
                return rn + " -= 4; m.write32(" + rn + ", c.vbr);";
            case Op::STC_L_SSR:
                return rn + " -= 4; m.write32(" + rn + ", c.ssr);";
            case Op::STC_L_SPC:
                return rn + " -= 4; m.write32(" + rn + ", c.spc);";
            case Op::STC_L_SGR:
                return rn + " -= 4; m.write32(" + rn + ", c.sgr);";
            case Op::STC_L_DBR:
                return rn + " -= 4; m.write32(" + rn + ", c.dbr);";
            case Op::STC_L_BANK:
                return rn + " -= 4; m.write32(" + rn + ", " + bank + ");";
            case Op::STS_MACH:
                return rn + " = c.mach;";
            case Op::STS_MACL:
                return rn + " = c.macl;";
            case Op::STS_PR:
                return rn + " = c.pr;";
            case Op::STS_L_MACH:
                return rn + " -= 4; m.write32(" + rn + ", c.mach);";
            case Op::STS_L_MACL:
                return rn + " -= 4; m.write32(" + rn + ", c.macl);";
            case Op::STS_L_PR:
                return rn + " -= 4; m.write32(" + rn + ", c.pr);";
            case Op::TRAPA:
                return "c.pc = " + hexu(pc) + "; trapa_in(c, m, " + d + ");";
            case Op::MOVCA_L:
                return "m.write32(" + rn + ", " + r0 + ");";
            case Op::PREF:
                // A store-queue flush, or a cache hint (ops.h, pref_sq).
                return "pref_sq(m, " + rn + ");";
            case Op::OCBI:
            case Op::OCBP:
            case Op::OCBWB:
                return "/* cache op: no-op on the host */";
            case Op::FRCHG:
                return "frchg(c);";
            case Op::FSCHG:
                return "c.fpscr ^= FPSCR_SZ;";
            case Op::LDS_FPUL:
                return "c.fpul = " + rm + ";";
            case Op::STS_FPUL:
                return rn + " = c.fpul;";
            case Op::LDS_L_FPUL:
                return "c.fpul = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::STS_L_FPUL:
                return rn + " -= 4; m.write32(" + rn + ", c.fpul);";
            case Op::STS_FPSCR:
                return rn + " = c.fpscr;";
            case Op::STS_L_FPSCR:
                return rn + " -= 4; m.write32(" + rn + ", c.fpscr);";
            case Op::LDS_FPSCR:
                return "write_fpscr(c, " + rm + ");";
            case Op::LDS_L_FPSCR:
                return "write_fpscr(c, m.read32(" + rm + ")); " + rm + " += 4;";
            case Op::FLDI0:
            case Op::FLDI1:
            case Op::FLDS:
            case Op::FSTS:
            case Op::FABS:
            case Op::FNEG:
            case Op::FADD:
            case Op::FSUB:
            case Op::FMUL:
            case Op::FDIV:
            case Op::FCMP_EQ:
            case Op::FCMP_GT:
            case Op::FMAC:
            case Op::FLOAT:
            case Op::FTRC:
            case Op::FSQRT:
            case Op::FSRRA:
            case Op::FCNVSD:
            case Op::FCNVDS:
            case Op::FSCA:
            case Op::FIPR:
            case Op::FTRV:
            case Op::FMOV:
            case Op::FMOV_LOAD:
            case Op::FMOV_LOAD_INC:
            case Op::FMOV_LOAD_R0:
            case Op::FMOV_STORE:
            case Op::FMOV_STORE_DEC:
            case Op::FMOV_STORE_R0:
                return lower_fp(ins);
            default:
                return "unimplemented(c, " + hexu(ins.raw) + ", " + hexu(pc) + ");";
        }
    }

    const Image& img_;
    const FunctionSpec& spec_;
    const EmitOptions& opt_;
    const std::map<std::uint32_t, std::string>& known_;
    const Summaries& summaries_;
    EmitResult& result_;
    FpMode summary_{kEntry, kEntry};
    bool saves_fpscr_ = false;
    const std::set<std::uint32_t>* written_ = nullptr;  // image words stored to by unit code
    std::set<std::uint32_t> blocks_;
    std::set<std::uint32_t> labels_;
    std::set<std::uint32_t> call_returns_;
    std::map<std::uint32_t, std::uint32_t> block_end_;
    std::map<std::uint32_t, bool> ends_in_transfer_;
    std::map<std::uint32_t, FpMode> block_mode_;
    std::set<std::uint32_t> slot_labels_;
    std::map<std::uint32_t, SwitchTable> switches_;
    FpMode mode_;
    unsigned pending_cycles_ = 0;
};

}  // namespace

EmitResult emit_unit(const Image& image, const std::vector<FunctionSpec>& functions,
                     const EmitOptions& options) {
    EmitResult result;
    std::map<std::uint32_t, std::string> known;
    std::vector<FunctionSpec> specs = functions;
    for (auto& f : specs) {
        if (f.name.empty())
            f.name = default_name(f.entry);
        known[f.entry] = f.name;
    }
    std::ostringstream src, hdr;
    hdr << "// Generated by dream-translate. Do not edit.\n#pragma once\n#include "
           "\"dream/runtime/memory.h\"\n"
        << "#include \"dream/runtime/sh4/ctx.h\"\n\nnamespace dream::gen {\n";
    src << "// Generated by dream-translate from image base " << hex(image.base)
        << ". Do not edit.\n"
        << "#include <cstdint>\n#include \"dream/runtime/memory.h\"\n#include "
           "\"dream/runtime/sh4/abi.h\"\n"
        << "#include \"dream/runtime/sh4/ctx.h\"\n#include \"dream/runtime/sh4/ops.h\"\n\n"
        << "namespace dream::gen {\nusing namespace dream::sh4;\nusing dream::Memory;\n\n";
    for (const auto& f : specs) {
        hdr << "void " << f.name << "(dream::sh4::Ctx& c, dream::Memory& m);\n";
        src << "void " << f.name << "(Ctx& c, Memory& m);\n";
        src << (options.shards > 1 ? "void " : "static void ") << f.name
            << "__resume(Ctx& c, Memory& m, std::uint32_t resume_pc);\n";
        src << (options.shards > 1 ? "void " : "static void ") << f.name
            << "__in(Ctx& c, Memory& m);\n";
        // [hooks]: defined by the port; a missing definition is a link error, never a silent no-op.
        if (!f.hook.empty() && f.hook_entry)
            src << "bool dream_hook_entry_" << f.hook << "(Ctx& c, Memory& m);\n";
        if (!f.hook.empty() && f.hook_exit)
            src << "void dream_hook_exit_" << f.hook << "(Ctx& c, Memory& m);\n";
    }
    if (options.imports)
        for (const auto& [entry, name] : *options.imports)
            if (!known.count(entry))
                src << "void " << name << "__in(Ctx& c, Memory& m);  // imported\n";
    src << "\n";
    // Sharded output: every part starts with the same includes and declarations.
    const std::string prologue = src.str();
    std::vector<std::string> bodies;
    // Pass 0: pool words the unit itself writes (run-time patched literals) are never folded.
    std::set<std::uint32_t> written;
    {
        Summaries none;
        for (const auto& f : specs) {
            EmitResult scratch;
            FunctionEmitter fe(image, f, options, known, none, scratch);
            fe.scan_stores(written);
        }
    }
    // Callee summaries: iterate to a fixpoint over the call graph. A summary that keeps changing
    // (recursion through a toggle) is widened to unknown after a few rounds.
    Summaries summaries;
    std::map<std::uint32_t, int> changes;
    for (int round = 0; round < 8; ++round) {
        bool changed = false;
        for (const auto& f : specs) {
            EmitResult scratch;
            FunctionEmitter fe(image, f, options, known, summaries, scratch);
            fe.set_written(&written);
            FpMode sm = fe.summarise();
            auto it = summaries.find(f.entry);
            if (it == summaries.end() || it->second != sm) {
                if (++changes[f.entry] > 4)
                    sm = {kUnknown, kUnknown};
                summaries[f.entry] = sm;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    // Entry modes: seed the functions nobody calls directly with their spec's default, then push
    // the mode at each call site into the callee and join (disagreement -> unknown) to a fixpoint.
    {
        std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, FpMode>>> sites;
        std::set<std::uint32_t> called;
        for (const auto& f : specs) {
            EmitResult scratch;
            FunctionEmitter fe(image, f, options, known, summaries, scratch);
            fe.set_written(&written);
            fe.call_site_modes(sites[f.entry]);
            for (const auto& [t, m] : sites[f.entry]) called.insert(t);
        }
        auto resolve_abs = [](std::uint8_t rel, std::uint8_t e) -> std::uint8_t {
            switch (rel) {
                case 0:
                case 1:
                    return rel;
                case kEntry:
                    return e;
                case kEntryToggled:
                    return e == kUnknown ? kUnknown : static_cast<std::uint8_t>(e ^ 1u);
                default:
                    return kUnknown;
            }
        };
        auto join = [](std::uint8_t a, std::uint8_t b) -> std::uint8_t {
            return a == b ? a : kUnknown;
        };
        std::map<std::uint32_t, FpMode> entry_abs;
        for (const auto& f : specs) {
            if (called.count(f.entry))
                continue;
            // Nothing in the image calls this one, so it is entered from outside the analysis: an
            // interrupt vector, the program's own entry, or an indirect call. An interrupt arrives
            // in whatever floating-point mode the code it interrupted was using, which is not a
            // property of the handler and cannot be deduced from the image. Assuming the reset mode
            // here is a guess, and a wrong guess compiles `fmov` to move the wrong number of bytes
            // and walk its pointer at the wrong rate; unknown costs a runtime branch and is always
            // right.
            entry_abs[f.entry] = FpMode{kUnknown, kUnknown};
        }
        for (int round = 0; round < 32; ++round) {
            bool changed = false;
            for (const auto& [caller, list] : sites) {
                auto ce = entry_abs.find(caller);
                if (ce == entry_abs.end())
                    continue;
                for (const auto& [callee, rel] : list) {
                    const FpMode abs{resolve_abs(rel.pr, ce->second.pr),
                                     resolve_abs(rel.sz, ce->second.sz)};
                    auto it = entry_abs.find(callee);
                    if (it == entry_abs.end()) {
                        entry_abs[callee] = abs;
                        changed = true;
                    } else {
                        const FpMode joined{join(it->second.pr, abs.pr),
                                            join(it->second.sz, abs.sz)};
                        if (joined != it->second) {
                            it->second = joined;
                            changed = true;
                        }
                    }
                }
            }
            if (!changed)
                break;
        }
        for (auto& f : specs) {
            auto it = entry_abs.find(f.entry);
            if (it == entry_abs.end())
                continue;  // only reachable through a cycle: keep the default
            const FpMode e = it->second;
            f.entry_pr_unknown = e.pr == kUnknown;
            f.entry_sz_unknown = e.sz == kUnknown;
            if (e.pr != kUnknown)
                f.fpscr_entry =
                    (f.fpscr_entry & ~(1u << 19)) | (static_cast<std::uint32_t>(e.pr) << 19);
            if (e.sz != kUnknown)
                f.fpscr_entry =
                    (f.fpscr_entry & ~(1u << 20)) | (static_cast<std::uint32_t>(e.sz) << 20);
        }
    }
    for (const auto& f : specs) {
        std::ostringstream fsrc;
        std::ostringstream& out = options.shards > 1 ? fsrc : src;
        const std::size_t before = result.fp_runtime_branches;
        FunctionEmitter fe(image, f, options, known, summaries, result);
        fe.set_written(&written);
        const std::string body = fe.run();
        // Entry-mode clone: only where the entry mode was unknown and that cost runtime tests.
        if (result.fp_runtime_branches > before && (f.entry_pr_unknown || f.entry_sz_unknown)) {
            FunctionSpec fast = f;
            fast.entry_pr_unknown = fast.entry_sz_unknown = false;  // the default bits in fpscr_entry
            EmitResult scratch;
            FunctionEmitter fc(image, fast, options, known, summaries, scratch);
            fc.set_written(&written);
            fc.set_clone_suffix("__fpd");
            out << "static void " << f.name
                << "__fpd(Ctx& c, Memory& m, std::uint32_t resume_pc);\n";
            out << fc.run() << "\n";
            fe.set_fast_clone(f.fpscr_entry & 0x00180000u);
        }
        out << body << fe.wrapper() << "\n";
        if (options.shards > 1)
            bodies.push_back(fsrc.str());
    }
    if (options.shards > 1) {
        // Contiguous runs of functions, balanced by text size: the parts compile in about the
        // same time, and neighbouring functions (which call each other most) stay together.
        std::size_t total = 0;
        for (const auto& b : bodies) total += b.size();
        const std::size_t n = options.shards;
        std::size_t i = 0, acc = 0;
        for (std::size_t k = 0; k < n; ++k) {
            std::string part = prologue;
            const std::size_t goal = total * (k + 1) / n;
            while (i < bodies.size() && (acc < goal || k + 1 == n)) {
                acc += bodies[i].size();
                part += bodies[i++];
            }
            part += "}  // namespace dream::gen\n";
            result.parts.push_back(std::move(part));
        }
    }
    src << "namespace {\n";
    if (options.overlay) {
        // Up to 32 instruction words from the function's own extent (never its literal pool,
        // which a no_fold copy patches at run time): the runtime dispatches to this translation
        // only while RAM still holds them.
        const std::uint32_t image_end = image.base + static_cast<std::uint32_t>(image.bytes.size());
        for (const auto& f : specs) {
            src << "const std::uint16_t kSig_" << f.name << "[] = {";
            const std::uint32_t stop = std::min({f.end, f.entry + 64u, image_end});
            for (std::uint32_t a = f.entry; a < stop; a += 2) src << hexu(image.read16(a)) << ", ";
            src << "};\n";
        }
    }
    src << "const FunctionEntry kTable[] = {\n";
    for (const auto& f : specs) {
        src << "    {" << hexu(f.entry) << ", &" << f.name << "__in";
        if (options.overlay)
            src << ", kSig_" << f.name << ", sizeof kSig_" << f.name << " / sizeof(std::uint16_t)";
        else
            src << ", nullptr, 0";
        src << ", " << hexu(f.end) << ", &" << f.name << "__resume},\n";
    }
    src << "};\nstruct Registrar {\n    Registrar() { register_functions(kTable, sizeof kTable / "
           "sizeof kTable[0]); }\n} registrar;\n"
        << "}  // namespace\n}  // namespace dream::gen\n";
    hdr << "}  // namespace dream::gen\n";
    result.source = src.str();
    result.header = hdr.str();
    return result;
}

}  // namespace dream::translator
