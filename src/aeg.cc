#include <vector>
#include <unordered_map>
#include <deque>
#include <unordered_set>
#include <fstream>

#include "util/z3.h"
#include "aeg.h"
#include "config.h"
#include "fol.h"
#include "progress.h"
#include "timer.h"
#include "taint.h"
#include "fork_work_queue.h"
#include "shm.h"
#include "taint_bv.h"

/* TODO
 * [ ] Don't use seen when generating tfo constraints
 * [ ] Use Graph<>'s node iterator, since it skips over deleted nodes? Or improve node range
 *     to skip deleted nodes.
 */

void AEG::dump_graph(const std::string& path) const {
    std::ofstream ofs {path};
    dump_graph(ofs);
}

void AEG::dump_graph(std::ostream& os) const {
    os << R"=(
    digraph G {
    overlap = scale;
    splines = true;
    
    )=";
    
    // define nodes
    unsigned next_id = 0;
    std::unordered_map<NodeRef, std::string> names;
    for (NodeRef ref : node_range()) {
        const Node& node = lookup(ref);
        const std::string name = std::string("n") + std::to_string(next_id);
        names.emplace(ref, name);
        ++next_id;
        
        os << name << " ";
        
        std::stringstream ss;
        ss << ref << " ";
        ss << *node.inst << "\n";
        ss << "po: " << node.arch << "\n"
        << "tfo: " << node.trans << "\n"
        << "tfo_depth: " << node.trans_depth << "\n";
        
        if (node.addr_def) {
            ss << "addr (def): " << *node.addr_def << "\n";
        }
        if (!node.addr_refs.empty()) {
            ss << "addr (refs):";
            for (const auto& ref : node.addr_refs) {
                ss << " " << ref.second;
            }
            ss << "\n";
        }
        
        if (dump_constraints) {
            ss << "constraints: " << node.constraints << "\n";
        }
        
        dot::emit_kvs(os, "label", ss.str());
        os << ";\n";
    }
    
    // define edges
    graph.for_each_edge([&] (NodeRef src, NodeRef dst, const Edge& edge) {
        os << names.at(src) << " -> " << names.at(dst) << " ";
        dot::emit_kvs(os, "label", util::to_string(edge));
        os << ";\n";
    });
    
    // graph labels (constraints)
    {
        os << "graph ";
        std::stringstream ss;
        ss << constraints;
        dot::emit_kvs(os, "label", ss.str());
        os << "\n";
    }
    
    os << "}\n";
}


void AEG::simplify() {
    Progress progress {nodes.size()};
    std::for_each(nodes.begin(), nodes.end(), [&] (Node& node) {
        node.simplify();
        ++progress;
    });
    progress.done();
    
    constraints.simplify();
    
    progress = Progress(nedges);
    graph.for_each_edge([&] (NodeRef, NodeRef, Edge& edge) {
        edge.simplify();
        ++progress;
    });
    progress.done();
}

void AEG::test() {
    unsigned naddrs = 0;
    for_each_edge(Edge::ADDR, [&] (NodeRef, NodeRef, const Edge&) {
        ++naddrs;
    });
    std::cerr << "Address edges: " << naddrs << "\n";
    if (naddrs > 0) {
        std::ofstream ofs {"addrs.txt", std::ios_base::out | std::ofstream::app};
        ofs << lookup(1).inst->get_inst()->getFunction()->getName().str() << "\n";
    } else {
        return;
    }
    
    logv(3) << "testing...\n";
    
    z3::solver solver {context.context};
    
    simplify();
    
    /* display stats */
    if (verbose >= 3) {
        auto& os = llvm::errs();
        os << constraints.exprs.size() << " top level constraints\n";
        const unsigned node_clauses =
        std::transform_reduce(nodes.begin(), nodes.end(), 0, std::plus<unsigned>(),
                              [] (const Node& node) {
            return node.constraints.exprs.size();
        });
        os << node_clauses << " node constraints\n";
        std::unordered_map<Edge::Kind, unsigned> edge_constraints;
        graph.for_each_edge([&] (NodeRef, NodeRef, const Edge& e) {
            edge_constraints[e.kind] += e.constraints.exprs.size();
        });
        os << std::transform_reduce(edge_constraints.begin(), edge_constraints.end(), 0, std::plus<unsigned>(), [] (const auto& pair) -> unsigned { return pair.second; }) << " edge constraints (total\n";
        for (const auto& pair : edge_constraints) {
            os << pair.first << " " << pair.second << "\n";
        }
    }
    
    // add edge constraints
    {
    std::cerr << __FUNCTION__ << ": adding edge constraints...\n";
    Progress progress {nedges};
    std::unordered_map<std::string, unsigned> names;
    graph.for_each_edge([&] (NodeRef src, NodeRef dst, const Edge& edge) {
        edge.constraints.add_to(solver);
        ++progress;
    });
        progress.done();
    }
    
    // add node constraints
    {
        std::cerr << __FUNCTION__ << ": adding node constraints...\n";
        Progress progress {size()};
    for (NodeRef ref : node_range()) {
        lookup(ref).constraints.add_to(solver);
        ++progress;
    }
        progress.done();
    }
    
    // add main constraints
    constraints.add_to(solver);
    
    std::cerr << solver.statistics() << "\n";
    
#define CHECKING 0
#if CHECKING
    // list of FOL expressions to evaulate
    const auto po = fol::edge_rel(*this, Edge::PO);
    const auto tfo = fol::edge_rel(*this, Edge::TFO);
    const auto rf = fol::edge_rel(*this, Edge::RF);
    const auto co = fol::edge_rel(*this, Edge::CO);
    const auto fr = fol::edge_rel(*this, Edge::FR);
    const auto com = rf + co + fr;
    const auto reads = fol::node_rel(*this, Inst::READ);
    const auto writes = fol::node_rel(*this, Inst::WRITE);
    const auto arch = fol::node_rel(*this, [] (const auto& p) -> z3::expr {
        return p.node.arch;
    });
    const auto trans = fol::node_rel(*this, [] (const auto& p) -> z3::expr {
        return p.node.trans;
    });
    const auto exec = arch + trans;
    const auto rfx = fol::edge_rel(*this, Edge::RFX);
    const auto cox = fol::edge_rel(*this, Edge::COX);
    const auto frx = fol::edge_rel(*this, Edge::FRX);
    const auto comx = rfx + cox + frx;
    
    const auto top = fol::node_rel(*this, Inst::ENTRY) & exec;
    const auto bot = fol::node_rel(*this, Inst::EXIT) & exec;
    
    const auto fr_computed = fol::join(fol::inverse(rf), co);
    const auto exprs = std::make_tuple(std::make_pair(po, "po"),
                                       std::make_pair(rf, "rf"),
                                       std::make_pair(co, "co"),
                                       std::make_pair(fr, "fr"),
                                       std::make_pair(fol::join(fol::inverse(rf), co), "fr'"),
                                       std::make_pair(bot, "bot"),
                                       std::make_pair(rfx, "rfx"),
                                       std::make_pair(cox, "cox"),
                                       std::make_pair(frx, "frx")
                                       );
#endif

    {
    Timer timer;
    solver.push();
    }
    
#if 0
    // DEBUG: Find which memory accesses use tainted pointer.
    {
        solver.push();
        
        std::vector<NodeRef> accesses;
        get_nodes_if(std::back_inserter(accesses), [] (NodeRef, const Node& node) -> bool {
            return node.inst.kind == Inst::READ || node.inst.kind == Inst::WRITE;
        });
        std::cerr << accesses.size() << " accesses\n";
        
        // Progress progress {accesses.size()};
        std::vector<NodeRef> tainted_accesses, maybe_tainted_accesses;
        for (NodeRef ref : accesses) {
            solver.push();
            
            const Node& node = lookup(ref);
            const z3::expr flag = tainter->flag(ref);
            solver.add(node.trans && flag, "flag"); // TODO: This should actually be trans.
            
            z3::check_result res;
            {
                Timer timer;
                res = solver.check();
                std::cerr << res << " ";
            }
            
            switch (res) {
                case z3::sat: {
                    tainted_accesses.push_back(ref);
                    std::stringstream path;
                    path << output_dir << "/taint" << ref << ".dot";
                    output_execution(path.str(), solver.get_model());
                    break;
                }
                case z3::unsat:
                    break;
                    
                case z3::unknown:
                    maybe_tainted_accesses.push_back(ref);
                    break;
                    
                default: std::abort();
            }
            
            solver.pop();
            
            // ++progress;
        }
        // progress.done();
        
        std::cerr << tainted_accesses.size() << " tainted accesses\n";
        std::cerr << maybe_tainted_accesses.size() << " maybe tainted accesses\n";
        
        solver.pop();
    }
#endif

    {
        fol::Context<z3::expr, fol::SymEval> fol_ctx {fol::Logic<z3::expr>(context.context), fol::SymEval(context.context), *this};
        const auto addr_rel = fol_ctx.edge_rel(Edge::ADDR);
        const auto trans_rel = fol_ctx.node_rel_if([&] (NodeRef, const Node& node) -> z3::expr {
            return node.trans;
        });
        const auto addr_expr = fol::some(fol::join(addr_rel, trans_rel));
        solver.push();
        solver.add(addr_expr);
        if (solver.check() == z3::sat) {
            z3::eval eval {solver.get_model()};
            output_execution("addr.dot", eval);
        } else {
            const auto exprs = solver.unsat_core();
            std::cerr << exprs << "\n";
            throw util::resume("no addr edges");
        }
        solver.pop();
        
        Timer timer;
#if 1
        const auto nleaks = leakage2(solver, 32);
#else
        const auto nleaks = leakage3(solver, 32);
#endif
        std::cerr << "Detected " << nleaks << " leaks.\n";
        if (nleaks == 0) {
            return;
        }
    }
    
    unsigned nexecs = 0;
    
#if CHECKING
    const auto dump_expressions = [&] (const z3::model& model) {
        std::ofstream ofs {std::string("out/exec") + std::to_string(nexecs) + ".txt"};
        util::for_each_in_tuple(exprs, [&] (const auto& pair) {
            ofs << pair.second << ":\n";
            for (const auto& rel_pair : pair.first) {
                if (model.eval(rel_pair.second).is_true()) {
                    util::for_each_in_tuple(rel_pair.first, [&] (const auto& x) {
                        ofs << " " << x;
                    });
                    ofs << "\n";
                }
            }
            ofs << "\n";
        });
    };
#endif
    
    constexpr unsigned max_nexecs = 16;
    while (nexecs < max_nexecs) {
        Stopwatch timer;
        timer.start();
        const auto res = solver.check();
        timer.stop();
        std::cerr << res << " " << timer << "\n";
        
            switch (res) {
            case z3::unsat: {
                const auto& core = solver.unsat_core();
                for (const auto& expr : core) {
                    llvm::errs() << util::to_string(expr) << "\n";
                }
                goto done;
            }
            case z3::sat: {
                const z3::eval eval {solver.get_model()};
                output_execution(std::string("out/exec") + std::to_string(nexecs) + ".dot", eval);
#if CHECKING
                dump_expressions(model);
#endif
                
                ++nexecs;
                
                // add constraints
                std::cerr << "adding different solution constraints...\n";
                Stopwatch timer;
                timer.start();
                std::vector<z3::expr> exprs;
                auto it = std::back_inserter(exprs);
                for (const Node& node : nodes) {
                    *it++ = node.arch;
                    *it++ = node.trans;
                }
                
                for_each_edge([&] (NodeRef, NodeRef, const Edge& edge) {
                    *it++ = edge.exists;
                });
                
                const z3::expr same_sol = std::transform_reduce(exprs.begin(), exprs.end(), context.TRUE, util::logical_and<z3::expr>(), [&] (const z3::expr& e) -> z3::expr {
                    return e == eval(e);
                });
                
                solver.add(!same_sol);
                
                timer.stop();
                std::cerr << timer << "\n";
                
                break;
            }
            case z3::unknown:
                goto done;
        }
    }
    
done:
    std::cerr << "found " << nexecs << " executions\n";
    
    solver.pop();
    
#if CHECKING
    // check FOL assertions
    const auto po_closure = fol::irreflexive_transitive_closure(po);
    
    const std::vector<std::tuple<z3::expr, z3::check_result, std::string>> vec = {
        {!fol::for_all(exec - bot, fol::for_func<NodeRef> {[&] (const fol::relation<NodeRef>& node) -> z3::expr {
            return fol::one(fol::join(node, tfo), context.context);
        }}, context.context), z3::unsat, "tfo succ"},
        {!fol::for_all(exec - top, fol::for_func<NodeRef> {[&] (const fol::relation<NodeRef>& node) -> z3::expr {
            return fol::one(fol::join(tfo, node), context.context);
        }}, context.context), z3::unsat, "tfo pred"},
        {!fol::one(exec & top, context.context), z3::unsat, "one top"},
        {!fol::one(exec & bot, context.context), z3::unsat, "one bot"},
        {fol::no(rf, context.context), z3::unsat, "no rf"},
        {fol::no(reads, context.context), z3::unsat, "no reads"},
        {fol::some(arch, context.context), z3::sat, "some arch"},
        {!fol::subset(fol::join(co, co), co, context.context), z3::unsat, "co.co in co"},
        {!fol::equal(fr, fol::join(fol::inverse(rf), co), context.context), z3::unsat, "fr = ~rf.co"},
        {!fol::subset(po, po_closure, context.context), z3::unsat, "po in ^po"},
        {!fol::acyclic(po, context.context), z3::unsat, "acyclic[po]"},
        {!fol::acyclic(po + com, context.context), z3::unsat, "acyclic[po+com]"},
        {!fol::equal(rf, fol::inverse(fol::inverse(rf)), context.context), z3::unsat, "rf = ~~rf"},
        {!fol::acyclic(cox, context.context), z3::unsat, "acyclic[cox]"},
        {!fol::acyclic(rfx + cox, context.context), z3::unsat, "acyclic[rfx+cox]"},
        {!fol::acyclic(comx, context.context), z3::unsat, "acyclic[comx]"},
        {fol::some(trans, context.context), z3::sat, "some trans"},
    };
    
    unsigned passes = 0;
    unsigned fails = 0;
    for (const auto& task : vec) {
        solver.push();
        solver.add(std::get<0>(task));
        const auto res = solver.check();
        if (res != std::get<1>(task)) {
            std::cerr << "CHECK FAILED: " << std::get<2>(task) << "\n";
            ++fails;
            if (res == z3::sat) {
                // produce model
                const z3::model model = solver.get_model();
                output_execution(std::string("out/exec") + std::to_string(nexecs) + ".dot", model);
                dump_expressions(model);
                ++nexecs;
            } else if (res == z3::unsat) {
                const auto& core = solver.unsat_core();
                for (const auto& expr : core) {
                    llvm::errs() << util::to_string(expr) << "\n";
                }
            }
        } else {
            ++passes;
        }
        solver.pop();
    }
    
    std::cerr << "Passes: " << passes << "\n"
              << "Fails:  " << fails  << "\n";
#endif

}

void AEG::add_unidir_edge(NodeRef src, NodeRef dst, const UHBEdge& e) {
    if (e.possible()) {
        graph.insert(src, dst, e);
        ++nedges;
    }
}

void AEG::add_bidir_edge(NodeRef a, NodeRef b, const UHBEdge& e) {
    UHBEdge e1 = e;
    UHBEdge e2 = e;
    const z3::expr dir = context.make_bool();
    e1.exists &=  dir;
    e2.exists &= !dir;
    add_unidir_edge(a, b, e1);
    add_unidir_edge(b, a, e2);
}

z3::expr AEG::add_optional_edge(NodeRef src, NodeRef dst, const UHBEdge& e_, const std::string& name) {
    UHBEdge e = e_;
    const z3::expr constr = e.exists;
    e.exists = context.make_bool(name);
    e.constraints(z3::implies(e.exists, constr), name);
    add_unidir_edge(src, dst, e);
    return e.exists;
}

namespace {
std::ostream& operator<<(std::ostream& os, const std::pair<z3::expr, std::string>& p) {
    return os << p.second << ":" << p.first;
}
}

std::ostream& operator<<(std::ostream& os, const UHBConstraints& c) {
    for (const auto& p : c.exprs) {
        os << p << " && ";
    }
    return os;
}

void AEG::output_execution(std::ostream& os, const z3::eval& eval, const EdgeSet& flag_edges) {
    os << R"=(
    digraph G {
    overlap = scale;
    splines = true;
    
    )=";
    
    // define nodes
    unsigned next_id = 0;
    std::unordered_map<NodeRef, std::string> names;
    for (NodeRef ref : node_range()) {
        const Node& node = lookup(ref);
        if (eval(node.exec())) {
            const std::string name = std::string("n") + std::to_string(next_id);
            names.emplace(ref, name);
            ++next_id;
            
            os << name << " ";
            std::stringstream ss;
            ss << ref << " " << *node.inst << "\n";

            if (node.inst->is_memory()) {
                ss << "{" << eval(node.get_memory_address()) << "} ";
            }
            
            const bool xsread = (bool) eval(node.xsread);
            const bool xswrite = (bool) eval(node.xswrite);
            if (xsread) {
                ss << "R";
            }
            if (xswrite) {
                ss << "W";
            }
            if ((xsread || xswrite) && node.xsaccess_order) {
                ss << "(" << eval(*node.xsaccess_order) << ") ";
            }
            
#if USE_TAINT
            // DEBUG: taint
            ss << " taint(" << eval(node.taint) << ")";
            if (node.inst->is_memory()) {
                const z3::expr flag = tainter->flag(ref);
                if (eval(flag)) {
                    ss << " FLAGGED";
                }
                
            }
            if (eval(node.taint_trans)) {
                ss << " taint_trans";
            }
#endif
            
            std::string color;
            if (eval(node.arch)) {
                color = "green";
            } else if (eval(node.trans)) {
                color = "red";
            }
            
            dot::emit_kvs(os, dot::kv_vec {{"label", ss.str()}, {"color", color}});
            os << ";\n";
        }
    }
    
    const auto output_edge = [&] (NodeRef src, NodeRef dst, Edge::Kind kind) {
        if (!include_edges.empty()) {
            if (include_edges.find(kind) == include_edges.end()) { return; }
        }
        
        os << names.at(src) << " -> " << names.at(dst) << " ";
        static const std::unordered_map<Edge::Kind, std::string> colors = {
            {Edge::TFO, "black"},
            {Edge::RF, "gray"},
            {Edge::CO, "blue"},
            {Edge::FR, "purple"},
            {Edge::RFX, "gray"},
            {Edge::COX, "blue"},
            {Edge::FRX, "purple"},
            {Edge::ADDR, "brown"},
            {Edge::CTRL, "purple"},
            {Edge::PO, "black"},
        };
        std::string color = colors.at(kind);
        if (flag_edges.find(std::make_tuple(src, dst, kind)) != flag_edges.end()) {
            color = "red";
        }
        dot::emit_kvs(os, dot::kv_vec {{"label", util::to_string(kind)}, {"color", color}});
        os << ";\n";
    };
    
    graph.for_each_edge([&] (NodeRef src, NodeRef dst, const Edge& edge) {
        if (eval(edge.exists)) {
            output_edge(src, dst, edge.kind);
        }
    });
    
    const fol::Context<bool, fol::ConEval> fol_ctx {fol::Logic<bool>(), fol::ConEval(eval), *this};
    
    const auto output_rel = [&] (const auto& rel, Edge::Kind kind) {
        for (const auto& p : rel) {
            output_edge(std::get<0>(p.first), std::get<1>(p.first), kind);
        }
    };
    
    static const Edge::Kind rels[] = {Edge::RF, Edge::CO, Edge::FR, Edge::RFX, Edge::COX, Edge::FRX};
    for (const Edge::Kind kind : rels) {
        output_rel(fol_ctx.edge_rel(kind), kind);
    }
    
    // output pseudo com and comx
    using EdgeSig = std::tuple<NodeRef, NodeRef, Edge::Kind>;
    using EdgeSigVec = std::vector<EdgeSig>;
    EdgeSigVec com, comx;
        // get_concrete_com(model, std::back_inserter(com));
        // get_concrete_comx(model, std::back_inserter(comx));
    for (const auto& edge : com) {
        std::apply(output_edge, edge);
    }
    for (const auto& edge : comx) {
        std::apply(output_edge, edge);
    }
    
#if 1
    // add tfo rollback edges
    for (const NodeRef ref : node_range()) {
        const Node& node = lookup(ref);
        if (!eval(node.arch)) { continue; }
        const auto next = [&] (NodeRef ref, Edge::Kind kind) -> std::optional<NodeRef> {
            const auto tfos = get_nodes(Direction::OUT, ref, kind);
            const auto tfo_it = std::find_if(tfos.begin(), tfos.end(), [&] (const auto& x) -> bool {
                return (bool) eval(x.second);
            });
            if (tfo_it == tfos.end()) {
                return std::nullopt;
            } else {
                return tfo_it->first;
            }
        };
        std::optional<NodeRef> cur;
        NodeRef prev = ref;
        while ((cur = next(prev, Edge::TFO))) {
            prev = *cur;
        }
        const auto arch_dst = next(ref, Edge::PO);
        if (prev != ref && arch_dst) {
            const auto trans_src = prev;
            output_edge(trans_src, *arch_dst, Edge::TFO);
        }
    }
#endif
    
    os << "}\n";
}

void AEG::output_execution(const std::string& path, const z3::eval& eval, const EdgeSet& flag_edges) {
    std::ofstream ofs {path};
    output_execution(ofs, eval, flag_edges);
}

template <typename OutputIt>
OutputIt AEG::get_edges(Direction dir, NodeRef ref, OutputIt out, Edge::Kind kind) {
    assert(!is_pseudoedge(kind));
    const auto& map = graph(dir);
    for (const auto& p : map.at(ref)) {
        for (auto& edge : p.second) {
            if (edge->kind == kind) {
                *out++ = edge.get();
            }
        }
    }
    return out;
}

AEG::EdgePtrVec AEG::get_edges(Direction dir, NodeRef ref, UHBEdge::Kind kind) {
   EdgePtrVec es;
   get_edges(dir, ref, std::back_inserter(es), kind);
   return es;
}

std::vector<std::pair<NodeRef, z3::expr>> AEG::get_nodes(Direction dir, NodeRef ref, Edge::Kind kind) const {
    std::vector<std::pair<NodeRef, z3::expr>> res;
    get_nodes(dir, ref, std::back_inserter(res), kind);
    return res;
}


const AEG::Edge *AEG::find_edge(NodeRef src, NodeRef dst, Edge::Kind kind) const {
    assert(!is_pseudoedge(kind));
    const auto src_it = util::contains(graph.fwd, src);
    if (!src_it) { return nullptr; }
    const auto dst_it = util::contains((**src_it).second, dst);
    if (!dst_it) { return nullptr; }
    const auto& edges = (**dst_it).second;
    const auto it = std::find_if(edges.begin(), edges.end(), [=] (const auto& edgeptr) {
        return edgeptr->kind == kind;
    });
    return it == edges.end() ? nullptr : it->get();
}

AEG::Edge *AEG::find_edge(NodeRef src, NodeRef dst, Edge::Kind kind) {
    assert(!is_pseudoedge(kind));
    auto& edges = graph.fwd[src][dst];
    const auto it = std::find_if(edges.begin(), edges.end(), [kind] (const auto& edgeptr) {
        return edgeptr->kind == kind;
    });
    return it == edges.end() ? nullptr : it->get();
}

NodeRef AEG::add_node(Node&& node) {
    const NodeRef ref = size();
    nodes.push_back(std::move(node));
    graph.add_node(ref);
    return ref;
}


z3::expr AEG::exists(Edge::Kind kind, NodeRef src, NodeRef dst) {
    switch (kind) {
        case Edge::CO: return co_exists(src, dst);
        case Edge::RF: return rf_exists(src, dst);
        case Edge::FR: return fr_exists(src, dst);
        case Edge::COX: return cox_exists(src, dst);
        case Edge::RFX: return rfx_exists(src, dst);
        case Edge::FRX: return frx_exists(src, dst);
            // case Edge::TFO: return tfo_exists(src, dst);
        case Edge::ADDR:
        case Edge::CTRL: {
            if (const Edge *edge = find_edge(src, dst, kind)) {
                return edge->exists;
            } else {
                return context.FALSE;
            }
        }
            
        default: std::abort();
    }
}

z3::expr AEG::exists_src(Edge::Kind kind, NodeRef src) const {
    const Node& node = lookup(src);
    switch (kind) {
        case Edge::PO: return node.arch;
        case Edge::TFO: return node.exec();
        case Edge::RF: return node.arch && node.write;
        case Edge::CO: return node.arch && node.write;
        case Edge::FR: return node.arch && node.read;
        case Edge::RFX: return node.exec() && node.xswrite;
        case Edge::COX: return node.exec() && node.xswrite;
        case Edge::FRX: return node.exec() && node.xsread;
        case Edge::ADDR: return node.exec() && node.read;
        case Edge::CTRL: return node.exec() && node.read;
        default: std::abort();
    }
}

z3::expr AEG::exists_dst(Edge::Kind kind, NodeRef dst) const {
    const Node& node = lookup(dst);
    switch (kind) {
        case Edge::PO: return node.arch;
        case Edge::TFO: return node.exec();
        case Edge::RF: return node.arch && node.read;
        case Edge::CO: return node.arch && node.write;
        case Edge::FR: return node.arch && node.write;
        case Edge::RFX: return node.exec() && node.xsread;
        case Edge::COX: return node.exec() && node.xswrite;
        case Edge::FRX: return node.exec() && node.xswrite;
        case Edge::ADDR: return node.exec() && node.access();
        case Edge::CTRL: return node.exec() && node.access();
        default: std::abort();
    }
}
