#include <vector>
#include <unordered_map>
#include <deque>
#include <unordered_set>
#include <fstream>

#include "z3-util.h"
#include "aeg.h"
#include "config.h"
#include "fol.h"

/* TODO
 * [ ] Don't use seen when generating tfo constraints
 * [ ] Use Graph<>'s node iterator, since it skips over deleted nodes? Or improve node range
 *     to skip deleted nodes.
 */

void AEG::dump_graph(const std::string& path) const {
    std::error_code ec;
    llvm::raw_fd_ostream os {path, ec};
    if (ec) {
        llvm::errs() << ec.message() << "\n";
        std::exit(1);
    }
    dump_graph(os);
}

void AEG::dump_graph(llvm::raw_ostream& os) const {
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
        ss << node.inst.kind_tostr() << "\n";
        ss << node.inst << "\n";
        ss << "po: " << node.arch << "\n"
        << "tfo: " << node.trans << "\n"
        << "tfo_depth: " << node.trans_depth << "\n";
        
#if 1
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
#endif
        
        ss << "constraints: " << node.constraints << "\n";
        
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
    unsigned count = 0;
    std::for_each(nodes.begin(), nodes.end(), [&] (Node& node) {
        llvm::errs() << ++count << "\n";
        node.simplify();
    });
    constraints.simplify();
    graph.for_each_edge([] (NodeRef, NodeRef, Edge& edge) {
        edge.simplify();
    });
}


void AEG::test() {
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
        unsigned edge_clauses = 0;
        graph.for_each_edge([&] (NodeRef, NodeRef, const Edge& e) {
            edge_clauses += e.constraints.exprs.size();
        });
        os << edge_clauses << " edge constraints\n";
    }
    
    
    // add edge constraints
    std::unordered_map<std::string, unsigned> names;
    graph.for_each_edge([&] (NodeRef src, NodeRef dst, const Edge& edge) {
        edge.constraints.add_to(solver);
    });
    
    // add node constraints
    for (NodeRef ref : node_range()) {
        lookup(ref).constraints.add_to(solver);
    }
    
    // add main constraints
    constraints.add_to(solver);
    
    std::cerr << solver.statistics() << "\n";
    
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
    
    const auto top = fol::node_rel(*this, Inst::ENTRY);
    const auto bot = fol::node_rel(*this, Inst::EXIT);
    
    const auto fr_computed = fol::join(fol::inverse(rf), co);
    const auto exprs = std::make_tuple(std::make_pair(po, "po"),
                                       std::make_pair(fol::element<0>(po, context.context), "po[0]"),
                                       std::make_pair(fol::identity(fol::element<0>(po, context.context)), "po[0]"),
                                       std::make_pair(fol::irreflexive_transitive_closure(po), "^po"),
                                       std::make_pair(fol::irreflexive_transitive_closure(po) & fol::identity(fol::element<0>(po, context.context)), "^po & iden"),
                                       std::make_pair(rfx, "rfx"),
                                       std::make_pair(cox, "cox"),
                                       std::make_pair(frx, "frx")
                                       );
    
    solver.push();
    
    unsigned nexecs = 0;
    
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
    
    constexpr unsigned max_nexecs = 16;
    while (nexecs < max_nexecs) {
        switch (solver.check()) {
            case z3::unsat: {
                llvm::errs() << "unsat\n";
                const auto& core = solver.unsat_core();
                for (const auto& expr : core) {
                    llvm::errs() << util::to_string(expr) << "\n";
                }
                goto done;
            }
            case z3::sat: {
                llvm::errs() << "sat";
                const z3::model model = solver.get_model();
                output_execution(std::string("out/exec") + std::to_string(nexecs) + ".dot", model);
                dump_expressions(model);
                
                ++nexecs;
                
                // add constraints
                z3::expr same_sol = context.TRUE;
                for (unsigned i = 0; i < model.size(); ++i) {
                    const z3::func_decl decl = model[i];
                    if (decl.range().is_bool()) {
                        same_sol &= decl() == model.get_const_interp(decl);
                    }
                }
                solver.add(!same_sol);
                break;
            }
            case z3::unknown:
                llvm::errs() << "unknown";
                goto done;
        }
    }
    
done:
    std::cerr << "found " << nexecs << " executions\n";
    
    solver.pop();
    
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
            }
        } else {
            ++passes;
        }
        solver.pop();
    }
    
    std::cerr << "Passes: " << passes << "\n"
              << "Fails:  " << fails  << "\n";

}

/* Using alias analysis to construct address variables. 
 * - Must alias: S = T
 * - May alias:  (no constraint)
 * - No alias:   S != T
 * One important added rule to handle loops:
 *  - Self-alias checks always returns 'may alias' to generalize across loops.
 */

void AEG::find_upstream_def(NodeRef node, const llvm::Value *addr_ref,
                            std::unordered_set<NodeRef>& out) const {
    /* Use BFS */
    std::deque<NodeRef> queue;
    const auto& init_preds = po.po.rev.at(node);
    std::copy(init_preds.begin(), init_preds.end(), std::front_inserter(queue));
    
    while (!queue.empty()) {
        const NodeRef nr = queue.back();
        queue.pop_back();
        const Node& node = lookup(nr);
        if (node.inst.addr_def == addr_ref) {
            out.insert(nr);
        } else {
            const auto& preds = po.po.rev.at(nr);
            std::copy(preds.begin(), preds.end(), std::front_inserter(queue));
        }
    }
}

#if 0
std::optional<z3::expr> AEG::check_no_intervening_writes(NodeRef src, NodeRef dst) const {
    /* TODO:
     * Visit the nodes in postorder, so that we know the full constraints of each node before
     * visiting successors, in order to avoid path explosion.
     *
     */
    
    z3::expr acc = context.TRUE;
    const Node& node = lookup(src);
    if (node.inst.kind == Inst::WRITE) {
        acc = z3::implies(node.po, node.addr_refs.at(0).po != lookup(src).addr_refs.at(0).po)
    }
    for (NodeRef pred : po.po.rev.at(src)) {
        if (pred != dst) {
            acc &= check_no_intervening_writes(pred, dst);
        }
    }
    return acc;
}
#else
z3::expr AEG::check_no_intervening_writes(NodeRef read, NodeRef write) const {
    /* Approach:
     * Use the postorder to be able to efficiently generate subpredicates.
     * For nodes of which `read` is an ancestor, it should all be false.
     * For nodes that are an ancestor of `read`, it should be computed accordingly.
     * All nodes start out as nullopt. All children must be nullopt to propogate.
     */
    const Node& read_node = lookup(read);
    std::vector<NodeRef> order;
    po.postorder(std::back_inserter(order));
    std::unordered_map<NodeRef, std::optional<z3::expr>> fs;
    for (NodeRef ref : order) {
        const auto& succs = po.po.fwd.at(ref);
        std::optional<z3::expr> out;
        for (NodeRef succ : succs) {
            if (const auto& in = fs.at(succ)) {
                out = out ? (*in && *out) : *in;
            }
        }
        if (out) {
            const Node& ref_node = lookup(ref);
            if (ref_node.inst.kind == Inst::WRITE) {
                *out &= lookup(ref).addr_refs.at(0).second.arch != read_node.addr_refs.at(0).second.arch;
            }
        } else {
            if (ref == read) {
                out = context.TRUE;
            }
        }
        fs.emplace(ref, out);
    }
    const auto& write_succs = po.po.fwd.at(write);
    return util::all_of(write_succs.begin(), write_succs.end(), [&] (NodeRef succ) -> z3::expr {
        const auto x = fs.at(succ);
        return x ? *x : context.TRUE;
    }, context.TRUE);
}
#endif


template <typename OutputIt>
void AEG::find_sourced_memops(Inst::Kind kind, NodeRef org, OutputIt out) const {
    std::unordered_map<NodeRef, std::optional<z3::expr>> nos;
    std::unordered_map<NodeRef, std::optional<z3::expr>> yesses;
    std::vector<NodeRef> order;
    po.postorder(std::back_inserter(order));
    
    const Node& org_node = lookup(org);
    for (NodeRef ref : order) {
        std::optional<z3::expr> out;
        for (NodeRef succ : po.po.fwd.at(ref)) {
            if (const auto& in = nos.at(succ)) {
                out = out ? (*out && *in) : *in;
            }
        }
        std::optional<z3::expr> yes;
        std::optional<z3::expr> no;
        if (out) {
            const Node& ref_node = lookup(ref);
            if (ref_node.inst.kind == kind) {
                const z3::expr same_addr =
                ref_node.addr_refs.at(0).second.arch == org_node.addr_refs.at(0).second.arch;
                yes = *out && ref_node.arch && same_addr;
                no = *out && z3::implies(ref_node.arch, !same_addr);
            } else if (ref == entry) {
                yes = out;
                no = out;
            } else {
                yes = std::nullopt;
                no = out;
            }
        } else if (ref == org) {
            yes = std::nullopt;
            no = context.TRUE;
        } else {
            yes = std::nullopt;
            no = std::nullopt;
        }
        nos.emplace(ref, no);
        yesses.emplace(ref, yes);
    }
    for (const auto& yes : yesses) {
        if (yes.second) {
            *out++ = {yes.first, *yes.second};
        }
    }
}

// TODO: Rewrite this using postorder.
template <typename OutputIt>
void AEG::find_preceding_memops(Inst::Kind kind, NodeRef write, OutputIt out) const {
    const Node& write_node = lookup(write);
    const z3::expr& write_addr = write_node.addr_refs.at(0).second.arch;
    
    std::deque<NodeRef> todo;
    std::unordered_set<NodeRef> seen;
    const auto& init_preds = po.po.rev.at(write);
    std::copy(init_preds.begin(), init_preds.end(), std::front_inserter(todo));
    
    while (!todo.empty()) {
        NodeRef ref = todo.back();
        todo.pop_back();
        
        if (!seen.insert(ref).second) {
            continue;
        }
        
        const Node& node = lookup(ref);
        if (node.inst.kind == kind) {
            const z3::expr same_addr = write_addr == node.addr_refs.at(0).second.arch;
            const z3::expr path_taken = node.arch;
            *out++ = CondNode {ref, same_addr && path_taken};
        }
        const auto& preds = po.po.rev.at(ref);
        std::copy(preds.begin(), preds.end(), std::front_inserter(todo));
    }
}




#if 0
template <typename OutputIt>
void AEG::find_comx_window(NodeRef ref, unsigned distance, unsigned spec_depth,
                           OutputIt out) const {
    /* Find predecessors exactly distance away */
    struct Entry {
        NodeRef ref;
        unsigned distance;
    };
    
    std::deque<Entry> po_todo {{ref, distance}};
    std::unordered_set<NodeRef> tfo_todo;
    std::unordered_set<NodeRef> po_set;
    std::unordered_set<NodeRef> tfo_set;
    
    // first explore po
    while (!todo.empty()) {
        const Entry& ent = todo.back();
        
        if (ent.distance == 0) {
            continue;
        }
        
        // output this node
        po_set.insert(ent.ref);
        
        const auto& preds = po.po.rev.at(ent.ref);
        std::transform(preds.begin(), preds.end(), std::front_inserter(todo),
                       [&] (NodeRef pred) -> Entry {
            return {pred, distance - 1};
        });
        
        todo.pop_back();
    }
    
    // now explore tfo
    
    for (NodeRef ref : po_set) {
        
    }
    
    std::copy(set.begin(), set.end(), out);
}
#endif

void AEG::add_bidir_edge(NodeRef a, NodeRef b, const UHBEdge& e) {
    UHBEdge e1 = e;
    UHBEdge e2 = e;
    const z3::expr dir = context.make_bool();
    e1.exists &=  dir;
    e2.exists &= !dir;
    add_unidir_edge(a, b, e1);
    add_unidir_edge(b, a, e2);
}

void AEG::add_optional_edge(NodeRef src, NodeRef dst, const UHBEdge& e_, const std::string& name) {
    UHBEdge e = e_;
    const z3::expr constr = e.exists;
    e.exists = context.make_bool(name);
    e.constraints(z3::implies(e.exists, constr));
    graph.insert(src, dst, e);
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

void AEG::output_execution(std::ostream& os, const z3::model& model) const {
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
        if (model.eval(node.get_exec()).is_true()) {
            const std::string name = std::string("n") + std::to_string(next_id);
            names.emplace(ref, name);
            ++next_id;
            
            os << name << " ";
            std::stringstream ss;
            ss << ref << " " << node.inst << "\n";
            
            switch (node.inst.kind) {
                case Inst::WRITE:
                case Inst::READ:
                    ss << "{" << model.eval(node.get_addr_ref(0)) << "}"
                    << "\n";
                    break;
                default: break;
            }
            
            std::string color;
            if (model.eval(node.arch).bool_value() == Z3_L_TRUE) {
                color = "green";
            } else if (model.eval(node.trans).bool_value() == Z3_L_TRUE) {
                color = "red";
            }
            
            dot::emit_kvs(os, dot::kv_vec {{"label", ss.str()}, {"color", color}});
            os << ";\n";
        }
    }
    
    // get cycles
    using Tarjan = tarjan<NodeRef>;
    Tarjan::DAG dag;
    const auto is_cycle_edge = [] (const Edge& e) {
        switch (e.kind) {
            case Edge::RFX:
            case Edge::COX:
                return true;
            default:
                return false;
        }
    };
    for_each_edge([&] (const NodeRef src, const NodeRef dst, const Edge& edge) {
        if (is_cycle_edge(edge) && model.eval(edge.exists).is_true()) {
            dag[src].insert(dst);
        }
    });
    std::vector<Tarjan::Cycle> cycles;
    Tarjan(dag, std::back_inserter(cycles));
    std::unordered_set<std::pair<NodeRef, NodeRef>> cycle_edges;
    for (Tarjan::Cycle& cycle : cycles) {
        NodeRef prev = cycle.back();
        for (auto it = cycle.begin(); it != cycle.end(); ++it) {
            cycle_edges.emplace(prev, *it);
            prev = *it;
        }
    }
    
    graph.for_each_edge([&] (NodeRef src, NodeRef dst, const Edge& edge) {
        if (model.eval(edge.exists).is_true()) {
            os << names.at(src) << " -> " << names.at(dst) << " ";
            std::string color;
            if (!is_cycle_edge(edge) || cycle_edges.find(std::make_pair(src, dst)) == cycle_edges.end()) {
                color = "black";
            } else {
                color = "red";
            }
            dot::emit_kvs(os, dot::kv_vec {{"label", util::to_string(edge.kind)}, {"color", color}});
            os << ";\n";
        }
    });
    
    os << "}\n";
}

void AEG::output_execution(const std::string& path, const z3::model& model) const {
    std::ofstream ofs {path};
    output_execution(ofs, model);
}

bool AEG::is_ancestor(NodeRef parent, NodeRef child) const {
    return is_ancestor_a(parent, child);
}

bool AEG::is_ancestor_a(NodeRef parent, NodeRef child) const {
    if (parent == child) {
        return true;
    }
    bool acc = false;
    for (NodeRef pred : po.po.rev.at(child)) {
        acc = acc || is_ancestor_a(parent, pred);
    }
    return acc;
}


bool AEG::is_ancestor_b(NodeRef parent, NodeRef child) const {
    std::vector<NodeRef> order;
    po.postorder(std::back_inserter(order));
    const auto child_it = std::find(order.begin(), order.end(), child);
    const auto parent_it = std::find(order.begin(), order.end(), parent);
    if (parent_it >= child_it) {
        return is_ancestor_a(parent, child);
    } else {
        // parent comes before child in postorder, so can't possibly be an ancestor
        return false;
    }
}


template <typename OutputIt>
void AEG::find_sourced_xsaccesses(XSAccess kind, NodeRef org, OutputIt out) const {
    OptionalNodeExprMap nos_po, yesses_po, nos_tfo, yesses_tfo;
    find_sourced_xsaccesses_po(kind, org, entry, yesses_po, nos_po);
    find_sourced_xsaccesses_tfo(kind, org, entry, 0, nos_po, yesses_tfo, nos_tfo);
    
    // output unified map
    for (NodeRef ref : node_range()) {
        auto yes_po = yesses_po[ref];
        auto yes_tfo = yesses_tfo[ref];
        if (yes_po || yes_tfo) {
            z3::expr yes = context.TRUE;
            if (yes_po) {
                yes &= *yes_po;
            }
            if (yes_tfo) {
                yes &= *yes_tfo;
            }
            *out++ = {ref, yes};
        }
    }
}

std::optional<z3::expr> AEG::find_sourced_xsaccesses_po
(XSAccess kind, NodeRef org, NodeRef ref, OptionalNodeExprMap& yesses,
 OptionalNodeExprMap& nos) const {
    {
        const auto it = nos.find(ref);
        if (it != nos.end()) {
            return it->second;
        }
    }
    
    auto& yes = yesses[ref];
    auto& no = nos[ref];
    
    if (org == ref) {
        yes = context.FALSE;
        return no = context.TRUE;
    }
    
    const auto& succs = po.po.fwd.at(ref);
    std::optional<z3::expr> out;
    for (const NodeRef succ : succs) {
        if (const auto in = find_sourced_xsaccesses_po(kind, org, succ, yesses, nos)) {
            out = out ? (*out && *in) : *in;
        }
    }
    
    
    // we haven't seen org yet
    if (!out) {
        return no = yes = std::nullopt;
    }
    
    const Node& org_node = lookup(org);
    
    no = yes = out;
    
    // add constraints for current node
    {
        const Node& ref_node = lookup(ref);
        const z3::expr *xsaccess;
        switch (kind) {
            case XSREAD:
                xsaccess = &ref_node.xsread;
                break;
            case XSWRITE:
                xsaccess = &ref_node.xswrite;
                break;
        }
        if (!xsaccess->is_false()) {
            const z3::expr same_xstate = ref_node.same_xstate(org_node);
            const z3::expr precond = ref_node.arch && *xsaccess;
            *no &= z3::implies(precond, !same_xstate);
            *yes &= precond && same_xstate;
        } else if (ref != entry) {
            // doesn't access, so yes should be false
            yes = context.FALSE;
        }
    }
    
    // add constraints for tfo
    {
        std::vector<NodeRef> todo;
        std::vector<NodeRef> next;
        std::copy(succs.begin(), succs.end(), std::back_inserter(todo));
        for (unsigned i = 1; i < po.num_specs; ++i) { // start @ 1 because succs are 1 step away
            for (const NodeRef ref : todo) {
                const Node& ref_node = lookup(ref);
                
                // add constraint
                const z3::expr is_xsaccess = ref_node.get_xsaccess(kind);
                if (!is_xsaccess.is_false()) {
                    const z3::expr same_xstate = ref_node.same_xstate(org_node);
                    const z3::expr f = z3::implies(ref_node.trans && is_xsaccess, !same_xstate);
                    *no &= f;
                    *yes &= f;
                }
                
                const auto& succs = po.po.fwd.at(ref);
                std::copy(succs.begin(), succs.end(), std::back_inserter(next));
            }
            todo = std::move(next);
        }
    }
    
    return no;
}


std::optional<z3::expr> AEG::find_sourced_xsaccesses_tfo(XSAccess kind, NodeRef org, NodeRef ref,
                                                         unsigned spec_depth,
                                                         OptionalNodeExprMap& nos_po,
                                                         OptionalNodeExprMap& yesses_tfo,
                                                         OptionalNodeExprMap& nos_tfo) const {
    /* For a tfo node that is at the max speculative depth, look back to all possible speculative
     * forks -- it's the disjunction of those conditions.
     * Otherwise, a tfo node is simply the disjunction of its tfo successors.
     *
     */
    
    {
        const auto it = nos_tfo.find(ref);
        if (it != nos_tfo.end()) {
            return it->second;
        }
    }
    
    auto& yes = yesses_tfo[ref];
    auto& no = nos_tfo[ref];
    
    if (org == ref) {
        yes = context.FALSE;
        return no = context.TRUE;
    }
    
    std::optional<z3::expr> out;
    
    if (spec_depth == po.num_specs) {
        
        /* max speculative depth reached, so look at all possible source speculative forks,
         * which will be given in `forks` */
        std::unordered_set<NodeRef> todo;
        std::unordered_set<NodeRef> next;
        std::unordered_set<NodeRef> forks;
        const auto& init_preds = po.po.rev.at(ref);
        std::copy(init_preds.begin(), init_preds.end(), std::inserter(todo, todo.end()));
        
        // get speculative forks
        for (unsigned i = 1; i < po.num_specs; ++i) { // start @ 1 because preds are 1 step away
            for (const NodeRef ref : todo) {
                const auto& succs = po.po.fwd.at(ref);
                if (succs.size() > 1) {
                    // speculative fork
                    forks.insert(ref);
                }
                const auto& preds = po.po.rev.at(ref);
                std::copy(preds.begin(), preds.end(), std::inserter(next, next.end()));
            }
            todo = std::move(next);
        }
        
        // add po constraints for each speculative fork
        for (const NodeRef fork : forks) {
            if (const auto& in = nos_po.at(fork)) {
                out = out ? (*out && *in) : *in;
            }
        }
        
    } else {
        
        // recurse
        const auto& succs = po.po.fwd.at(ref);
        for (const NodeRef succ : succs) {
            if (const auto in = find_sourced_xsaccesses_tfo(kind, org, succ, spec_depth + 1,
                                                            nos_po, yesses_tfo, nos_tfo)) {
                out = out ? (*out && *in) : *in;
            }
        }
        
    }
    
    if (!out) {
        // we haven't seen org yet
        return no = yes = std::nullopt;
    }
    
    const Node& org_node = lookup(org);
    no = yes = out;
    
    // add constraints for current node
    {
        const Node& ref_node = lookup(ref);
        const z3::expr *xsaccess;
        switch (kind) {
            case XSREAD:
                xsaccess = &ref_node.xsread;
                break;
            case XSWRITE:
                xsaccess = &ref_node.xswrite;
                break;
        }
        if (!xsaccess->is_false()) {
            const z3::expr same_xstate = ref_node.same_xstate(org_node);
            *no &= z3::implies(ref_node.trans, !same_xstate);
            *yes &= ref_node.trans && same_xstate;
        }
    }
    
    return no;
}


template <typename OutputIt>
OutputIt AEG::get_edges(Direction dir, NodeRef ref, OutputIt out, Edge::Kind kind) {
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

NodeRefVec AEG::get_nodes(Direction dir, NodeRef ref, Edge::Kind kind) const {
    NodeRefVec res;
    get_nodes(dir, ref, std::back_inserter(res), kind);
    return res;
}


const AEG::Edge *AEG::find_edge(NodeRef src, NodeRef dst, Edge::Kind kind) const {
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
    auto& edges = graph.fwd[src][dst];
    const auto it = std::find_if(edges.begin(), edges.end(), [kind] (const auto& edgeptr) {
        return edgeptr->kind == kind;
    });
    return it == edges.end() ? nullptr : it->get();
}

z3::expr AEG::edge_exists(NodeRef src, NodeRef dst, Edge::Kind kind) const {
    if (const Edge *edge = find_edge(src, dst, kind)) {
        return edge->exists;
    } else {
        return context.FALSE;
    }
}

NodeRef AEG::add_node(const Node& node) {
    const NodeRef ref = size();
    nodes.push_back(node);
    graph.add_node(ref);
    return ref;
}

