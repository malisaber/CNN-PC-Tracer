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




unsigned int hex2uint	(string inp)
{
    unsigned int tmp = 0;
    bool ridi(false);
    for (unsigned int i = 0; i < inp.size(); i++)
    {
        tmp = 16 * tmp;
        switch (inp[i])
        {
        case '0':
            break;
        case '1':
            tmp += 1;
            break;
        case '2':
            tmp += 2;
            break;
        case '3':
            tmp += 3;
            break;
        case '4':
            tmp += 4;
            break;
        case '5':
            tmp += 5;
            break;
        case '6':
            tmp += 6;
            break;
        case '7':
            tmp += 7;
            break;
        case '8':
            tmp += 8;
            break;
        case '9':
            tmp += 9;
            break;
        case 'a':
        case 'A':
            tmp += 10;
            break;
        case 'b':
        case 'B':
            tmp += 11;
            break;
        case 'c':
        case 'C':
            tmp += 12;
            break;
        case 'd':
        case'D':
            tmp += 13;
            break;
        case 'e':
        case 'E':
            tmp += 14;
            break;
        case 'f':
        case 'F':
            tmp += 15;
            break;
        default:
            ridi = true;
            break;
        }
    }
    if (ridi)
        cout << "ridi:\t" << inp << endl;
    return tmp;
}

struct func_info
{
    std::string func_name;
    unsigned int SOFunc;
    unsigned int EOFunc;
};

bool find_func			(const std::vector<func_info> &list, 
						 unsigned int pc, 
						 func_info &info)
{
    bool found(false);
    if (list.empty())
        return false;
    for (long long i = static_cast<long long>(list.size()) - 1; i >= 0; i--)
    {
        if (((int)pc >= (int)list[i].SOFunc) && ((int)pc <= (int)list[i].EOFunc))
        {
            info = list[i];
            found = true;
            break;
        }
    }
    return found;
}

bool is_interrupt_like	(const std::string &name)
{
    return (name == "INT_VECTOR") ||
           (name.rfind("EXT_INT_", 0) == 0) ||
           (name.rfind("external_interrupt_signal_handler_", 0) == 0);
}

void emit_stack_state	(std::ostream &out,
                    	 const std::string &pc,
                    	 const std::string &time,
                    	 const std::vector<func_info> &stack)
{
    out << "(" << pc << " @ " << std::setw(8) << time << ")" << "\t\t";
    for (std::size_t i = 1; i < stack.size(); ++i)
        out << "\t";
    if (!stack.empty() && is_interrupt_like(stack.back().func_name))
        out << "[IRQ] ";
    out << stack.back().func_name << '\n';
}

int main(int argc, char** argv)
{
    bool verbose(false);
	std::filesystem::path				Path2Code	=	"";	
	std::filesystem::path				Path2Loge	=	"";
	std::filesystem::path				Path2Outp	=	"";

    
	CLI::App app{"Assembly file to instruction memory file converter"};	
	app.add_flag	("-v,--verbose,!--no-verbose",	verbose,	"Enable verbose output");
	app.add_option	("-i,--input,--code-dir",		Path2Code,	"Path to code.txt");
	app.add_option	("-l,--log",					Path2Loge,	"Path to log file");
	app.add_option	("-o,--output,--out-dir",		Path2Outp,	"Output directory");
	CLI11_PARSE(app, argc, argv);

	
    

    string a_line;
	std::filesystem::create_directory(Path2Outp);
    ifstream code_fil;  code_fil.open(Path2Code);
    ifstream trac_log;  trac_log.open(Path2Loge);
    ofstream trac_vis;  trac_vis.open(Path2Outp / ("Trace.txt"));
    ofstream Erro_fil;  Erro_fil.open(Path2Outp / ("Errors.txt"));


    func_info info = { "INT_VECTOR", 0, 0 };
    //int func_cntr(0);
    std::vector<func_info> List;
    smatch m;



    cout << "Analizing text file ..." << endl;
    cout << "Extarcting function info ..." << endl;
    regex Code_pattern("^([0-9a-fA-F]{8}) <([0-9A-Za-z_]+)>:");
    int cntr(0);
    while (code_fil.is_open() && !code_fil.eof())
    {
        cntr++;
        if (cntr % 100 == 0)
        {
            cout << "\033[2K\r" << std::flush;
            cout << "line #" << cntr;
        }
        getline(code_fil, a_line);
        if (regex_search(a_line, m, Code_pattern))
        {
            unsigned int tmp = hex2uint(m.str(1));
            bool modif(false);
            if ((tmp % 8) != 0)
            {
                modif = true;
                tmp -= 4;
            }
            info.EOFunc = tmp - (!modif)*8;
            List.push_back(info);
            info.func_name = m.str(2);
            info.SOFunc = tmp;
        }
    }
    info.EOFunc = 0X7FFFFFFC;
    List.push_back(info);
    cout << "\033[2K\r" << std::flush;
    cout << "line #" << cntr << endl;
    cout << "Analizing text file is done!" << endl << endl;
    code_fil.close();

    if (verbose)
    {
        for (unsigned int i = 0; i < List.size(); i++)
        {
            std::ostringstream X1;
            std::ostringstream X2;
            X1 << std::hex << std::setw(8) << std::setfill('0') << List[i].SOFunc;
            X2 << std::hex << std::setw(8) << std::setfill('0') << List[i].EOFunc;
            cout << List[i].func_name << "\t<" << X1.str() << ", " << X2.str() << ">" << endl;
        }
    }


    cout << "Analizing Log file ..." << endl;
    cout << "Tracing Program Counter ..." << endl;
    regex Trac_pattern("^([0-9a-fA-F]{8})@([0-9]*)");
    cntr = 0;
    int ecntr(0);
    int correct(0);
    int PC(0);
    bool found(false);
    std::vector<func_info> call_stack;
    while (trac_log.is_open() && !trac_log.eof())
    {
        cntr++;
        if (cntr % 100 == 0)
        {
            cout << "\033[2K\r" << std::flush;
            cout << "line #" << cntr;
        }
        getline(trac_log, a_line);
        if (verbose)
            cout << "line #" << cntr << ":\t" << a_line << endl;

        if (a_line.empty())
        {
            correct++;
            continue;
        }

        if (!regex_search(a_line, m, Trac_pattern))
        {
            ecntr++;
            Erro_fil << a_line << endl;
            if (verbose)
                cout << "Skipping malformed log line: " << a_line << endl;
            continue;
        }

        PC = static_cast<int>(hex2uint(m.str(1)));
        string time = m.str(2);
        found = find_func(List, static_cast<unsigned int>(PC), info);
        if (!found)
        {
            ecntr++;
            Erro_fil << a_line << endl;
            if (verbose)
                cout << "Skipping unmapped PC: " << a_line << endl;
            continue;
        }

        if (call_stack.empty())
        {
            call_stack.push_back(info);
            emit_stack_state(trac_vis, m.str(1), time, call_stack);
            if (verbose)
                emit_stack_state(cout, m.str(1), time, call_stack);
            correct++;
            continue;
        }

        std::size_t matched = call_stack.size();
        for (std::size_t i = call_stack.size(); i-- > 0;)
        {
            if ((call_stack[i].SOFunc == info.SOFunc) && (call_stack[i].EOFunc == info.EOFunc) &&
                (call_stack[i].func_name == info.func_name))
            {
                matched = i;
                break;
            }
        }

        if (matched < call_stack.size())
        {
            if (matched + 1 == call_stack.size())
            {
                correct++;
                continue;
            }
            if (matched + 1 < call_stack.size())
                call_stack.erase(call_stack.begin() + matched + 1, call_stack.end());
            emit_stack_state(trac_vis, m.str(1), time, call_stack);
            if (verbose)
                emit_stack_state(cout, m.str(1), time, call_stack);
            correct++;
            continue;
        }

        call_stack.push_back(info);
        emit_stack_state(trac_vis, m.str(1), time, call_stack);
        if (verbose)
            emit_stack_state(cout, m.str(1), time, call_stack);
        correct++;
    }

    cout << "\033[2K\r" << std::flush;
    cout << "line #" << cntr << endl;
    cout << "error= " << ecntr << endl;
    cout << "correct= " << correct << endl;
    cout << "Tracing is done!" << endl << endl;
    trac_log.close();
    trac_vis.close();
    Erro_fil.close();

    cout << "ALL DONE!" << endl;

    return 0;
}



