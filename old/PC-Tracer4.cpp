//-----------------------------------------------------------------
// PC-Tracer2.cpp
//
// Elaborates a trace produced by biriscv_pc_tracer2.v into a call
// stack history, in the same spirit as the original PC-Tracer.cpp
// but driven by *explicit* control-flow classification instead of
// PC-range guessing:
//
//   - CALL   : JAL/JALR that writes ra (x1)             -> push
//   - RETURN : JALR ra,0(ra)  (rs1==ra, rd==x0, imm==0)  -> pop
//   - xRET   : MRET/SRET (matches biriscv INST_ERET)     -> pop
//   - IRQ/EXC: an 'X' record from the tracer              -> push
//              (matched by the following xRET)
//   - anything else that lands in a different function
//     than the current top of stack (e.g. a tail-jump)
//     just relabels the current frame instead of pushing,
//     since no return address was ever saved for it.
//
// Because push/pop are derived from real retiring instructions
// (post branch-resolution, post flush), this is correct across
// mispredicted returns and interrupts, which is exactly what broke
// the original tool.
//-----------------------------------------------------------------
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <string>
#include <regex>
#include <vector>
#include <cstdint>
#include "CLI11.hpp"

using namespace std;

//-----------------------------------------------------------------
// hex2uint: parse a hex string (no 0x prefix) -> unsigned int
//-----------------------------------------------------------------
static unsigned int hex2uint(const string &inp)
{
    unsigned int tmp = 0;
    for (char c : inp)
    {
        tmp <<= 4;
        if (c >= '0' && c <= '9')      tmp += (c - '0');
        else if (c >= 'a' && c <= 'f') tmp += (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') tmp += (c - 'A' + 10);
    }
    return tmp;
}

//-----------------------------------------------------------------
// Symbol table (built from an objdump-style "code.txt" listing,
// same format as the original tool: lines like
//   00000130 <main_entry>:
//-----------------------------------------------------------------
struct func_info
{
    string       func_name;
    unsigned int SOFunc = 0;
    unsigned int EOFunc = 0;
};

static bool find_func(const vector<func_info> &list, unsigned int pc, func_info &info)
{
    if (list.empty())
        return false;
    for (long long i = (long long)list.size() - 1; i >= 0; i--)
    {
        if ((int)pc >= (int)list[i].SOFunc && (int)pc <= (int)list[i].EOFunc)
        {
            info = list[i];
            return true;
        }
    }
    return false;
}

//-----------------------------------------------------------------
// RISC-V field / opcode helpers (subset mirrored from
// biriscv_defs.v - only what's needed to classify control flow)
//-----------------------------------------------------------------
static inline unsigned rd_of(uint32_t op)  { return (op >> 7)  & 0x1F; }
static inline unsigned rs1_of(uint32_t op) { return (op >> 15) & 0x1F; }
static inline int32_t  itype_imm(uint32_t op)
{
    int32_t imm = (int32_t)op >> 20; // sign extended [31:20]
    return imm;
}

static const uint32_t INST_JAL        = 0x0000006f, INST_JAL_MASK        = 0x0000007f;
static const uint32_t INST_JALR       = 0x00000067, INST_JALR_MASK       = 0x0000707f;
static const uint32_t INST_ERET       = 0x00200073, INST_ERET_MASK       = 0xcfffffff; // mret/sret/uret

static inline bool is_jal (uint32_t op) { return (op & INST_JAL_MASK)  == INST_JAL;  }
static inline bool is_jalr(uint32_t op) { return (op & INST_JALR_MASK) == INST_JALR; }
static inline bool is_eret(uint32_t op) { return (op & INST_ERET_MASK) == INST_ERET; }

static inline bool is_call(uint32_t op)
{
    return (is_jal(op) || is_jalr(op)) && rd_of(op) == 1; // writes ra
}
static inline bool is_ret(uint32_t op)
{
    // jalr x0, 0(ra)
    return is_jalr(op) && rs1_of(op) == 1 && rd_of(op) == 0 && itype_imm(op) == 0;
}

//-----------------------------------------------------------------
// Call-stack context
//
// Each interrupt/exception gets its own independent call stack
// (a "Context"), separate from whatever the main flow (or an outer
// interrupt, for nested IRQs) was doing. The interrupted context is
// simply left untouched on a context stack while the handler's
// context runs; on xRET the handler's whole context is discarded
// and the previous one resumes exactly where it left off. Every
// frame belonging to an interrupt context - not just its top -
// carries the [IRQ] tag, since is_irq is a property of the context,
// not of an individual frame.
//-----------------------------------------------------------------
struct Context
{
    vector<func_info> stack;
    bool               is_irq = false;
};

static void emit_stack_state(ostream &out, const string &pc, const string &time,
                              const Context &ctx)
{
    out << "(" << pc << " @ " << setw(8) << time << ")" << "\t\t";
    for (size_t i = 1; i < ctx.stack.size(); ++i)
        out << "\t";
    if (ctx.is_irq)
        out << "[IRQ] ";
    if (!ctx.stack.empty())
        out << ctx.stack.back().func_name << '\n';
    else
        out << "<empty>\n";
}

int main(int argc, char **argv)
{
    bool verbose(false);
    std::filesystem::path Path2Code = "";
    std::filesystem::path Path2Loge = "";
    std::filesystem::path Path2Outp = "";

    CLI::App app{"biRISC-V retirement trace -> call stack elaborator (v2)"};
    app.add_flag  ("-v,--verbose,!--no-verbose", verbose,   "Enable verbose output");
    app.add_option("-i,--input,--code-dir",      Path2Code, "Path to code.txt (objdump listing)");
    app.add_option("-l,--log",                   Path2Loge, "Path to PC_trac_log.log produced by biriscv_pc_tracer2");
    app.add_option("-o,--output,--out-dir",      Path2Outp, "Output directory");
    CLI11_PARSE(app, argc, argv);

    std::filesystem::create_directory(Path2Outp);

    ifstream code_fil(Path2Code);
    ifstream trac_log(Path2Loge);
    ofstream trac_vis(Path2Outp / "Trace.txt");
    ofstream Erro_fil(Path2Outp / "Errors.txt");

    //---------------------------------------------------------
    // 1) Parse symbol table (same as original tool)
    //---------------------------------------------------------
    vector<func_info> List;
    {
        func_info info{"INT_VECTOR", 0, 0};
        smatch m;
        regex Code_pattern("^([0-9a-fA-F]{8}) <([0-9A-Za-z_]+)>:");
        string a_line;
        int cntr = 0;
        cout << "Parsing symbol table..." << endl;
        while (code_fil.is_open() && !code_fil.eof())
        {
            cntr++;
            getline(code_fil, a_line);
            if (regex_search(a_line, m, Code_pattern))
            {
                // NOTE: earlier versions rounded `tmp` to an 8-byte
                // boundary and additionally subtracted 8, which was a
                // workaround for a *fetch-stage* tracer whose PC was
                // only reported at 8-byte (2-instruction) granularity.
                // biriscv_pc_tracer2 logs the exact, writeback-stage
                // PC, which is already correct to 4 bytes, so no such
                // rounding is needed - and doing it anyway carved an
                // 8-byte dead zone right before every function's
                // start, silently discarding each *previous* function's
                // last instruction(s) (almost always its `ret`). Ranges
                // are now perfectly contiguous: function N ends exactly
                // one instruction before function N+1 starts.
                unsigned int tmp = hex2uint(m.str(1));
                info.EOFunc = tmp - 4;
                List.push_back(info);
                info.func_name = m.str(2);
                info.SOFunc = tmp;
            }
        }
        info.EOFunc = 0x7FFFFFFC;
        List.push_back(info);
        cout << "  " << List.size() << " symbols loaded." << endl;
    }

    //---------------------------------------------------------
    // 2) Walk the retirement trace, classifying each event
    //---------------------------------------------------------
    regex I_pattern("^I ([0-9a-fA-F]{8})@([0-9]+) ([0-9a-fA-F]{8})");
    regex X_pattern("^X ([0-9a-fA-F]{8})@([0-9]+) ([0-9a-fA-F]{2})");
    smatch m;
    string a_line;

    vector<Context> contexts;
    contexts.push_back(Context{});   // context[0] = main flow, is_irq=false

    // What the *next* committed instruction implies for the currently
    // active context:
    //   NONE - just a normal instruction, no stack action pending
    //   CALL - push a new frame onto the *current* context's stack
    //   IRQ  - push a brand-new, separate context (its own stack)
    enum class Pending { NONE, CALL, IRQ };
    Pending pending = Pending::NONE;

    int cntr = 0, ecntr = 0, correct = 0;

    cout << "Elaborating trace..." << endl;
    while (trac_log.is_open() && !trac_log.eof())
    {
        cntr++;
        getline(trac_log, a_line);
        if (a_line.empty())
            continue;

        if (regex_search(a_line, m, I_pattern))
        {
            unsigned int pc   = hex2uint(m.str(1));
            string       time = m.str(2);
            uint32_t     op   = (uint32_t)hex2uint(m.str(3));

            func_info info;
            bool found = find_func(List, pc, info);
            if (!found)
            {
                // IMPORTANT: do NOT `continue` here. Symbol-table gaps
                // are a display-only problem (the function name is
                // unknown) and must never be allowed to skip the
                // is_call/is_ret/is_eret classification below - doing
                // so silently drops pops, which is what previously
                // turned every unmapped `ret` into a permanent stack
                // leak (the "never-ending slope"). Worst case with
                // this fallback is a frame labelled "??".
                ecntr++;
                Erro_fil << a_line << "  (unmapped PC)\n";
                info.func_name = "??";
                info.SOFunc = pc;
                info.EOFunc = pc;
            }

            Context &cur = contexts.back();

            if (pending == Pending::IRQ)
            {
                // Freeze whatever context was running and start a
                // brand-new, independent one for the handler. Nested
                // interrupts just stack another fresh context on top
                // the same way.
                contexts.push_back(Context{ {info}, true });
                pending = Pending::NONE;
                emit_stack_state(trac_vis, m.str(1), time, contexts.back());
                if (verbose) emit_stack_state(cout, m.str(1), time, contexts.back());
            }
            else if (pending == Pending::CALL)
            {
                cur.stack.push_back(info);
                pending = Pending::NONE;
                emit_stack_state(trac_vis, m.str(1), time, cur);
                if (verbose) emit_stack_state(cout, m.str(1), time, cur);
            }
            else if (cur.stack.empty())
            {
                cur.stack.push_back(info);
                emit_stack_state(trac_vis, m.str(1), time, cur);
                if (verbose) emit_stack_state(cout, m.str(1), time, cur);
            }
            else if (info.func_name != cur.stack.back().func_name)
            {
                // Landed in a different function without a saved
                // return address (tail-jump / fallthrough into an
                // adjacent routine): relabel current frame in place,
                // depth unchanged.
                cur.stack.back() = info;
                emit_stack_state(trac_vis, m.str(1), time, cur);
                if (verbose) emit_stack_state(cout, m.str(1), time, cur);
            }
            // else: same function, same frame - normal sequential
            // execution, nothing to report.

            correct++;

            // Now decide what *this* instruction implies for what
            // will be active on the *next* commit. Re-fetch the
            // active context, since an IRQ push above may have
            // changed which one that is.
            Context &active = contexts.back();

            if (is_eret(op))
            {
                // Return from trap/interrupt: discard the *entire*
                // handler context (its call stack is not carried
                // over - it's gone, by design) and resume whichever
                // context was running before it, exactly as it was
                // left.
                if (contexts.size() > 1)
                    contexts.pop_back();
                else
                {
                    // xRET while not actually inside a tracked
                    // interrupt context - likely a spurious/boot-time
                    // mret. Don't crash the stack; note it and treat
                    // it like an ordinary return in the main context.
                    Erro_fil << a_line << "  (xRET with no matching interrupt context)\n";
                    if (!active.stack.empty())
                        active.stack.pop_back();
                }
            }
            else if (is_ret(op))
            {
                if (!active.stack.empty())
                    active.stack.pop_back();
            }
            else if (is_call(op))
            {
                pending = Pending::CALL;
            }
        }
        else if (regex_search(a_line, m, X_pattern))
        {
            // Interrupt/exception taken. The handler's identity is
            // only known once we see its first committed PC, so just
            // arm a pending context push; the interrupted PC is only
            // useful for diagnostics here.
            pending = Pending::IRQ;
            correct++;
        }
        else
        {
            ecntr++;
            Erro_fil << a_line << "  (malformed line)\n";
        }
    }

    cout << "line #" << cntr << endl;
    cout << "errors  = " << ecntr << endl;
    cout << "correct = " << correct << endl;
    cout << "Tracing is done!" << endl;

    trac_log.close();
    trac_vis.close();
    Erro_fil.close();

    return 0;
}