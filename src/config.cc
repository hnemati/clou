#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <getopt.h>
#include <vector>

#include <llvm/Support/raw_ostream.h>

#include "config.h"
#include "util.h"
#include "uhb.h"

/* TODO
 * [ ] Handle function names
 */

char prog[] = "lcm";
static std::vector<char *> args = {prog};

std::string output_dir;
unsigned verbose = 0;
bool dump_constraints = false;
bool include_expr_in_constraint_name = false;
std::unordered_set<std::string> function_names;
std::unordered_set<unsigned> include_edges;
unsigned spec_depth = 2;
unsigned num_jobs = 1;
unsigned rob_size = 10;
unsigned max_traceback = 1;
std::ofstream log_;
LeakageClass leakage_class = LeakageClass::INVALID;
std::optional<unsigned> max_transient_nodes;
AliasMode alias_mode = {
    .transient = false,
    .lax = false,
};
SpectreV1Mode spectre_v1_mode = {
    .mode = SpectreV1Mode::CLASSIC,
};
SpectreV4Mode spectre_v4_mode = {
    .psf = false,
    .stb_size = 0,
};
bool witness_executions = true;
bool partial_executions = false;
bool fast_mode = false;

// TODO: add automated way for describing default values

namespace {

void usage(FILE *f = stderr) {
    const char *s = R"=(usage: [option...]
Options:
--help, -h           show help
--output, -o <path>  output directory
--func, -f <name>[,<name>]...
only examine given functions
--verbose, -v        verbosity++
--constraints, -c    include constraints in AEG graph output
--expr, -e           include expression string in constraint name (for debugging)
--edges, -E          include edges in execution graph output
--depth, -d <n>      speculation depth
--speculation-primitives <primitive>[,<primitive>...]
                     use comma-separated speculation primitives (possibilities: "branch", "addr")
--leakage-sources <source>[,<source>...]
                     use comman-separated leakage sources (possibilities: "addr-dst", "taint-trans")
--max-transient <num>
                     set maximum number of transient nodes (default: no limit)
--aa <flag>[,<flag>...]
                     set alias analysis flags. Accepted flags: "transient", "lax"
--spectre-v1 <subopts>
                     set Spectre-v1 options. Suboptions:
    mode={classic|branch-predicate}
--spectre-v4 <subopts>
                     set Spectre-v4 options. Suboptions:
    stb-size=<uint>       store buffer size
--traceback <uint>   set max traceback via rf * (addr + data) edges.
--witnesses <bool>   enable/disable generation of witness executions (default: on)
--partial [<bool>]   model partial executions in AEG (default: false)
--fast <bool>        enable/disable fast mode (default: off)
)=";
    fprintf(f, s);
}

void initialize() {
    log_.open("log");
}

void check_config() {
    if (leakage_class == LeakageClass::INVALID) {
        error("missing leakage class option (--spectre-v1, --spectre-v4, etc.)");
    }
}

template <typename OutputIt, typename Handler>
OutputIt parse_list(char *s, OutputIt out, Handler handler) {
    char *tok;
    while ((tok = strsep(&s, ","))) {
        if (*tok != '\0') {
            *out++ = handler(tok);
        }
    }
    return out;
}

bool parse_bool(const std::string& s) {
    const std::unordered_set<std::string> yes = {"yes", "y", "on"};
    const std::unordered_set<std::string> no = {"no", "n", "off"};
    std::string lower;
    std::transform(s.begin(), s.end(), std::back_inserter(lower), tolower);
    if (yes.find(lower) != yes.end()) { return true; }
    if (no.find(lower) != no.end()) { return false; }
    error("invalid boolean flag '%s'", s.c_str());
}

bool parse_bool_opt(const char *s) {
    if (s) {
        return parse_bool(s);
    } else {
        return true;
    }
}

int parse_args() {
    if (char *line = getenv("LCM_ARGS")) {
        while (char *s = strsep(&line, " ")) {
            if (*s) {
                args.push_back(s);
            }
        }
    }
    
    char **argv = args.data();
    int argc = args.size();
    int optc;
    
    initialize();
    
    enum Option {
        SPECULATION_PRIMITIVES = 256,
        MAX_TRANSIENT,
        AA_FLAGS,
        SPECTRE_V1,
        SPECTRE_V4,
        TRACEBACK,
        WITNESSES,
        PARTIAL,
        FAST,
    };
    
    struct option opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"verbose", no_argument, nullptr, 'v'},
        {"output", required_argument, nullptr, 'o'},
        {"constraints", no_argument, nullptr, 'c'},
        {"expr", no_argument, nullptr, 'e'},
        {"function", required_argument, nullptr, 'f'},
        {"edges", required_argument, nullptr, 'E'},
        {"depth", required_argument, nullptr, 'd'},
        {"jobs", required_argument, nullptr, 'j'},
        {"speculation-primitives", required_argument, nullptr, SPECULATION_PRIMITIVES},
        {"max-transient", required_argument, nullptr, MAX_TRANSIENT},
        {"aa", optional_argument, nullptr, AA_FLAGS},
        {"spectre-v1", optional_argument, nullptr, SPECTRE_V1},
        {"spectre-v4", optional_argument, nullptr, SPECTRE_V4},
        {"traceback", required_argument, nullptr, TRACEBACK},
        {"witnesses", optional_argument, nullptr, WITNESSES},
        {"partial", optional_argument, nullptr, PARTIAL},
        {"fast", optional_argument, nullptr, FAST},
        {nullptr, 0, nullptr, 0}
    };
    
    while ((optc = getopt_long(argc, argv, "hvo:cef:E:d:j:", opts, nullptr)) >= 0) {
        switch (optc) {
            case 'h':
                usage(stdout);
                exit(0);
                
            case 'o':
                output_dir = optarg;
                break;
                
            case 'v':
                ++verbose;
                break;
                
            case 'c':
                dump_constraints = true;
                break;
                
            case 'e':
                include_expr_in_constraint_name = true;
                break;
                
            case 'f':
                function_names.insert(optarg);
                break;
                
            case 'E': {
                const char *token;
                while ((token = strsep(&optarg, ",")) != nullptr) {
                    include_edges.insert(aeg::Edge::kind_fromstr(token));
                }
                break;
            }
                
            case 'd':
                spec_depth = std::stoul(optarg);
                break;
                
            case 'j':
                num_jobs = std::stoul(optarg);
                break;
                
            case MAX_TRANSIENT:
                max_transient_nodes = std::stoul(optarg);
                break;
                
            case AA_FLAGS: {
                alias_mode = {
                    .transient = false,
                    .lax = false,
                };
                
                std::vector<std::string> flags;
                parse_list(optarg, std::back_inserter(flags), [] (const char *s) -> std::string { return s; });
                for (const std::string& flag : flags) {
                    if (flag == "transient") {
                        alias_mode.transient = true;
                    } else if (flag == "lax") {
                        alias_mode.lax = true;
                    } else {
                        error("bad alias analysis flag '%s", suboptarg);
                    }
                }
                break;
            }
            
            case SPECTRE_V1: {
                enum Key {
                    MODE,
                    COUNT
                };
                
                const char *keylist[COUNT + 1] = {
                    [MODE] = "mode",
                    [COUNT] = nullptr
                };
                
                bool args[COUNT] = {
                    [MODE] = true,
                };
                
                char *value;
                int idx;
                while ((idx = getsubopt(&optarg, (char **) keylist, &value)) >= 0) {
                    if (args[idx] && value == nullptr) {
                        error("spectre-v1: suboption '%s' missing value", suboptarg);
                    }
                    switch (idx) {
                        case MODE: {
                            static const std::unordered_map<std::string, SpectreV1Mode::Mode> map {
                                {"classic", SpectreV1Mode::CLASSIC},
                                {"branch-predicate", SpectreV1Mode::BRANCH_PREDICATE},
                            };
                            spectre_v1_mode.mode = map.at(value);
                            break;
                        }
                            
                        default: std::abort();
                    }
                }
                
                leakage_class = LeakageClass::SPECTRE_V1;
                
                break;
            }
                
                
            case SPECTRE_V4: {
                leakage_class = LeakageClass::SPECTRE_V4;
                
                enum Key {
                    PSF,
                    STB_SIZE,
                    COUNT
                };

                const char *keylist[COUNT + 1] = {
                    [PSF] = "psf",
                    [STB_SIZE] = "stb-size",
                    [COUNT] = nullptr
                };
                
                bool args[COUNT] = {
                    [PSF] = false,
                    [STB_SIZE] = true,
                };
                
                char *value;
                int idx;
                while ((idx = getsubopt(&optarg, (char **) keylist, &value)) >= 0) {
                    if (args[idx] && value == nullptr) {
                        error("spectre-v4: suboption '%s' missing value", suboptarg);
                    }
                    switch (idx) {
                        case PSF:
                            spectre_v4_mode.psf = true;
                            break;
                            
                        case STB_SIZE:
                            spectre_v4_mode.stb_size = std::stoul(value);
                            break;

                        default: std::abort();
                    }
                }
                if (suboptarg != nullptr) {
                    error("spectre-v4: invalid suboption '%s'", suboptarg);
                }
                
                break;
            }
                
                
            case TRACEBACK: {
                max_traceback = std::stoul(optarg);
                break;
            }
                
            case WITNESSES: {
                witness_executions = parse_bool_opt(optarg);
                break;
            }
                
            case PARTIAL: {
                partial_executions = parse_bool_opt(optarg);
                break;
            }
                
            case FAST: {
                fast_mode = parse_bool_opt(optarg);
                if (fast_mode) {
                    witness_executions = false;
                    partial_executions = true;
                }
                break;
            }
                
            default:
                usage();
                exit(1);
        }
    }
    
    
    // check
    check_config();
    
    return 0;
}

const int parse_args_force = parse_args();

}
