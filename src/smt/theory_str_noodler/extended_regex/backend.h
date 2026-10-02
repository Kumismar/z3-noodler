#pragma once

#include "ast/ast.h"
#include "ast/seq_decl_plugin.h"
#include "params/theory_str_noodler_params.h"
#include "util/zstring.h"
#include "util/zstring_view.h"

#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// Common backend of the extended regexes (ECMAScript, ...): the common AST produced by the frontends, its conversion
// into the Regex Constraint Graph (RCG), and the generation of string constraints by a DFS over the RCG.
namespace smt::noodler::extended_regex {
    class ExtendedRegex;

    // ======================= UTILS =======================
    using Z3Char = uint32_t;

    // Upper bound of an unbounded quantifier ('*', '+', '{n,}').
    constexpr uint64_t UNBOUNDED = std::numeric_limits<uint64_t>::max();

    /**
     * @brief Printable ASCII characters are printed quoted ('a'), all other characters as U+XXXX. Used for
     * serialization and error messages.
     */
    std::string char_to_string(Z3Char ch);

    // =============== REGEX CONSTRAINT GRAPH ===============

    /**
     * Payload for a regular match edge. During DFS, the variable assigned to this edge
     * must satisfy str.in_re(edge_var, regex).
     */
    struct MatchEdge {
        app_ref regex;
    };

    using VertexID = uint32_t;
    using EdgeID = uint32_t;
    using GroupID = uint32_t;

    /**
     * A connected subgraph of the RCG produced by a single AST node's get_subgraph() method.
     *
     * `v_in` and `v_out` are the entry and exit vertices of the fragment.
     *
     * `edges_pointing_to_vout` holds all edges whose target is `v_out`.
     * This list is needed when chaining fragments -- all edges going to `v_out` are redirected to the next fragment's
     * `v_in`. When alternating fragments, all edges in both fragments' `edges_pointing_to_vout` lists are redirected to
     * a new shared `v_out`.
     */
    struct GraphFragment {
        VertexID v_in = std::numeric_limits<VertexID>::max();
        VertexID v_out = std::numeric_limits<VertexID>::max();
        std::vector<EdgeID> edges_pointing_to_vout;
        bool is_initialized() const;
    };

    enum class LookaroundDirection {
        FORWARD,
        BACKWARD
    };

    /**
     * Represents a lookaround assertion (lookahead or lookbehind, positive or negative).
     *
     * `subregex` holds the inner pattern of the assertion.
     * If the inner pattern is fully regular it is stored as z3 regular expression via app_ref; the DFS evaluates it
     * with `str.to_re` constraint. If the inner pattern contains non-regular constructs it is stored as a GraphFragment
     * and evaluated by running a nested DFS on the fragment.
     */
    struct Lookaround {
        std::variant<app_ref, GraphFragment> subregex;
        LookaroundDirection direction;
        bool is_positive;
    };

    using Anchor = uint32_t;

    /**
     * Payload for an assertion edge.
     *
     * An Anchor is either '^' (start of string) or '$' (end of string).
     * The edge variable generated for assertions is always empty string "".
     */
    struct AssertionEdge {
        std::variant<Anchor, Lookaround> payload;
    };

    /**
     * Payload for a backreference edge. During constraint generation, the edge variable must equal the string captured
     * by that group on the current path. If the group has not been entered yet (forward reference), the edge variable
     * is constrained to be empty.
     */
    struct BackrefEdge {
        GroupID backref_id;
    };

    struct RCGVertex {
        VertexID id;
        std::vector<EdgeID> outgoing_edges;

        RCGVertex(const VertexID vid, std::vector<EdgeID> edges)
            : id(vid),
              outgoing_edges(std::move(edges)) { }
    };

    using RCGEdgePayload = std::variant<std::monostate, MatchEdge, AssertionEdge, BackrefEdge>;

    struct RCGEdge {
        EdgeID id;
        VertexID target;
        RCGEdgePayload payload;

        RCGEdge(const EdgeID eid, const VertexID target_id, RCGEdgePayload edge_payload)
            : id(eid),
              target(target_id),
              payload(std::move(edge_payload)) { }
    };

    constexpr VertexID UNKNOWN_VERTEX = std::numeric_limits<VertexID>::max();

    /**
     * The Regex Constraint Graph (RCG) encodes the structure of an extended regex.
     * Vertices represent split points where the regex transitions between regular and non-regular components, or other
     * structural boundaries (such as alternations).
     *
     * Edges carry one of three payloads:
     *   - MatchEdge:     the edge variable belongs to a standard Z3 regex.
     *   - AssertionEdge: zero-width assertion (anchors, lookarounds); variable is "".
     *   - BackrefEdge:   the edge variable must equal the string captured by a group.
     *
     * A valid match corresponds to a path from start_vertex to end_vertex, where
     * the concatenated edge variables equal the target string.
     *
     * The `group_starts` and `group_ends` maps track which edges open and close
     * specific capture groups, allowing the DFS to evaluate backreferences.
     */
    struct RegexConstraintGraph {
        std::vector<RCGVertex> vertices;
        std::vector<RCGEdge> edges;

        std::unordered_map<EdgeID, std::vector<GroupID>> group_starts;
        std::unordered_map<EdgeID, std::vector<GroupID>> group_ends;

        VertexID start_vertex = UNKNOWN_VERTEX;
        VertexID end_vertex = UNKNOWN_VERTEX;

        void add_vertex(RCGVertex vtx);
        VertexID create_vertex();
        VertexID create_vertex(std::vector<EdgeID> edge_list);
        void add_edge(RCGEdge child);
        EdgeID create_edge();
        EdgeID create_edge(VertexID target, RCGEdgePayload payload);
    };

    /**
     * @brief Connect two fragments sequentially: first -> second.
     *
     * Optimization: redirects every edge in first.edges_pointing_to_vout to point to second.v_in instead
     * of first.v_out. The resulting fragment spans from first.v_in to second.v_out.
     *
     * @param graph  The RCG owning both fragments.
     * @param first  The fragment that runs first.
     * @param second The fragment that runs after first.
     * @return GraphFragment A fragment from first.v_in to second.v_out.
     */
    GraphFragment chain_fragments(RegexConstraintGraph& graph, const GraphFragment& first, const GraphFragment& second);

    /**
     * @brief Merge two fragments into one that accepts either branch.
     *
     * Optimization: creates new global v_in and v_out, steals edges pointing from both fragments' v_in and adds them to
     * the new v_in. Then, redirects all edges in both fragments' edges_pointing_to_vout to the new v_out. The resulting
     * fragment spans from the new v_in to the new v_out.
     *
     * @param graph  The RCG owning both fragments.
     * @param first  One branch of the alternation.
     * @param second The other branch of the alternation.
     * @return GraphFragment A fragment spanning from the new v_in to the new v_out with two alternating branches.
     */
    GraphFragment alternate_fragments(RegexConstraintGraph& graph, const GraphFragment& first,
                                      const GraphFragment& second);

    /**
     * @brief Create a fragment that matches the empty string (epsilon).
     *
     * @param graph  The RCG to add the fragment to.
     * @param util_s Z3 sequence/regex utilities.
     * @param m      The Z3 AST manager.
     * @return GraphFragment A two-vertex fragment with a single epsilon MatchEdge.
     */
    GraphFragment make_epsilon_fragment(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m);

    // ================== COMMON REGEX AST ==================
    //
    // The common AST is produced by the frontends of all regex flavors. It is close to the regex grammar of ECMA-262
    // 2020 (section 21.2.1 Patterns), but the flavor-specific meaning of predefined character classes (., \d, \s, ...)
    // is resolved by the frontends into explicit sets of characters (CharSet).

    /**
     * Quantifier of a term (21.2.1 Quantifier, semantics in 21.2.2.7).
     *
     * `max == UNBOUNDED` stands for infinity. `greedy` is false for the lazy forms (`*?`, `+?`, `??`, `{n,m}?`). The
     * laziness does not change the set of strings matched by the regex.
     */
    struct Quantifier {
        uint64_t min = 0;
        uint64_t max = UNBOUNDED;
        bool greedy = true;
    };

    /**
     * Alternatives of the Assertion production (21.2.1 Assertion, semantics in 21.2.2.6).
     */
    enum class AssertionKind {
        START,              // ^
        END,                // $
        WORD_BOUNDARY,      // \b
        NOT_WORD_BOUNDARY,  // \B
        LOOKAHEAD,          // (?=...)
        NEG_LOOKAHEAD,      // (?!...)
        LOOKBEHIND,         // (?<=...)
        NEG_LOOKBEHIND,     // (?<!...)
    };

    /**
     * An inclusive range of characters in a character class. A single character `c` is stored as {c, c}.
     */
    struct ClassRange {
        Z3Char lo = 0;
        Z3Char hi = 0;
    };

    /**
     * A predefined set of characters given by a frontend, e.g. \d, \S or . in ECMAScript.
     *
     * The set contains the characters of `ranges`, or all the other characters if `negated` is true. `source` is a view
     * of the source text in the pattern (e.g. \d), used only for serialization.
     */
    struct CharSet {
        std::vector<ClassRange> ranges;
        bool negated = false;
        zstring_view source;
    };

    /**
     * A set of characters given by a Unicode property, e.g. \p{Lu} or \P{Script=Greek}. Not supported by the backend
     * yet.
     *
     * `name` and `value` are views of the property name and value in the pattern. For the lone form (e.g. \p{Lu}),
     * `name` is empty. `source` is a view of the whole source text (e.g. \p{Lu}).
     */
    struct UnicodeProperty {
        bool negated = false;
        zstring_view name;
        zstring_view value;
        zstring_view source;
    };

    using ClassItem = std::variant<ClassRange, CharSet, UnicodeProperty>;

    /**
     * The result type of ASTNode::get_subgraph(). Each AST node converts itself into
     * one of two forms:
     *
     *   app_ref       -- the subtree is fully regular and collapses to a single Z3 regex expression.
     *
     *   GraphFragment -- the subtree contains non-regular constructs (backreferences, anchors, or lookarounds) and is
     *                    represented as a sub-portion of the RCG. The DFS will traverse this subgraph and generate
     *                    explicit string constraints for each path through it.
     *
     * The more subtrees are kept as app_refs, the smaller the RCG and the fewer constraints are generated.
     */
    using RegexComponent = std::variant<GraphFragment, app_ref>;
    class ASTNode;
    using ASTNodeRef = std::unique_ptr<ASTNode>;

    class ASTNode {
    public:
        virtual ~ASTNode() = default;
        virtual uint64_t print_dot(std::ostream& out, uint64_t& node_count) const = 0;
        virtual zstring serialize() const = 0;

        /**
         * @brief Convert this AST subtree into a RegexComponent.
         *
         * If the entire subtree is regular, returns an app_ref containing the combined Z3 regex expression for the
         * subtree. Otherwise, creates the necessary vertices and edges in @p graph and returns a GraphFragment
         * describing the subgraph.
         *
         * @param graph  The RCG being built. New vertices and edges are appended here.
         * @param util_s Z3 sequence/regex utility object.
         * @param m      The Z3 AST manager.
         * @return RegexComponent Either an app_ref (regular subtree) or a GraphFragment (non-regular subtree).
         */
        virtual RegexComponent get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const = 0;

        /**
         * @brief Return a deep copy of this node and all its children.
         *
         * Used by ASTNodeQuantified::unroll() to duplicate the quantified subpattern multiple times without sharing AST
         * nodes between copies.
         *
         * @return ASTNodeRef An independent copy of this node.
         */
        virtual ASTNodeRef clone() const = 0;

        /**
         * @brief Recursively convert all capturing groups in this subtree to non-capturing ones.
         */
        virtual void strip_captures() { }

        /**
         * @brief Recursively collect IDs of all backreferences in this subtree.
         *
         * Used as a pre-pass before `build_rcg()` to determine which capture groups are actually referenced. The result
         * is passed to `strip_unreferenced_captures()` to convert unreferenced capturing groups to non-capturing ones,
         * leading to fewer unsupported regex structure errors.
         *
         * @param refs Set to which every backreference ID found in the subtree is added.
         */
        virtual void collect_backrefs(std::unordered_set<GroupID>& refs) const { }

        /**
         * @brief Convert every capturing group whose ID is not in @p referenced to a non-capturing one.
         *
         * @param referenced IDs of the groups referenced by some backreference.
         */
        virtual void strip_unreferenced_captures(const std::unordered_set<GroupID>& referenced) { }
    };

    /**
     * Disjunction :: Alternative | Alternative '|' Disjunction
     *
     * Holds a list of alternative sub-patterns.
     */
    class ASTNodeDisjunction : public ASTNode {
    public:
        uint64_t print_dot(std::ostream& out, uint64_t& node_count) const override;
        zstring serialize() const override;
        void add_alternative(ASTNodeRef alt);

        /**
         * @brief Build the regex/subgraph for a set of alternatives.
         *
         * First, get_subgraph() is called on every alternative. Results are split into two buckets: regular ones
         * (app_ref) and non-regular ones (GraphFragment).
         *
         * If all alternatives are regular, they are combined with mk_union and returned as a single app_ref without any
         * graph nodes being created.
         *
         * If some of the alternatives are nonregular, all the regular alternatives are merged together with mk_union
         * into a single app_ref (because alternation is commutative), then the regular app_ref is alternated with all
         * the remaining non-regular fragments.
         *
         * @return RegexComponent app_ref if all alternatives are regular, GraphFragment otherwise.
         */
        RegexComponent get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const override;
        ASTNodeRef clone() const override;
        void strip_captures() override;
        void collect_backrefs(std::unordered_set<GroupID>& refs) const override;
        void strip_unreferenced_captures(const std::unordered_set<GroupID>& referenced) override;

    private:
        std::vector<ASTNodeRef> m_alternatives;
    };

    /**
     * Alternative :: [empty] | Alternative Term
     *
     * Holds an ordered list of terms (a concatenation).
     */
    class ASTNodeAlternative : public ASTNode {
    public:
        uint64_t print_dot(std::ostream& out, uint64_t& node_count) const override;
        zstring serialize() const override;
        void add_term(ASTNodeRef term);

        /**
         * @brief Build the regex/subgraph for a sequence of terms.
         *
         * Calls get_subgraph() on each term. Adjacent app_refs in the result list are merged with mk_concat. Once the
         * merged list is built, consecutive fragments are connected with chain_fragments.
         *
         * If the entire sequence reduces to a single app_ref (all terms are regular), that app_ref is returned directly
         * without creating any graph vertices.
         *
         * @return RegexComponent app_ref if all terms are regular, GraphFragment otherwise.
         */
        RegexComponent get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const override;
        ASTNodeRef clone() const override;
        void strip_captures() override;
        void collect_backrefs(std::unordered_set<GroupID>& refs) const override;
        void strip_unreferenced_captures(const std::unordered_set<GroupID>& referenced) override;

    private:
        std::vector<ASTNodeRef> m_terms;
    };

    /**
     * Term :: Assertion -- anchors (^, $), word boundaries (\b, \B), or lookarounds ((?=...), (?!...), (?<=...),
     * (?<!...)).
     *
     * All assertions always produce a GraphFragment, even if the inner pattern of an assertion is non-regular.
     * We handle the assertions during the constraint generation with special constraints.
     */
    class ASTNodeAssertion : public ASTNode {
    public:
        /**
         * @param kind       The kind of the assertion.
         * @param subpattern The inner Disjunction of a lookaround, must be nullptr for ^, $, \b and \B.
         */
        explicit ASTNodeAssertion(AssertionKind kind, ASTNodeRef subpattern = nullptr);

        uint64_t print_dot(std::ostream& out, uint64_t& node_count) const override;
        zstring serialize() const override;
        AssertionKind get_kind() const;
        bool is_lookaround() const;

        /**
         * @brief Builds the constraint graph fragment for zero-width assertions.
         *
         * This handles anchors (^, $), word boundaries (\b, \B), and both regular and non-regular lookarounds.
         *
         * @return RegexComponent Always returns a GraphFragment.
         */
        RegexComponent get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const override;
        ASTNodeRef clone() const override;
        void strip_captures() override;
        void collect_backrefs(std::unordered_set<GroupID>& refs) const override;
        void strip_unreferenced_captures(const std::unordered_set<GroupID>& referenced) override;

    private:
        AssertionKind m_kind;
        ASTNodeRef m_subpattern;  // only for lookarounds

        /**
         * @brief Create a two-vertex, fragment with a single AssertionEdge for a lookaround.
         *
         * @param graph       The RCG to add the fragment to.
         * @param m           The Z3 AST manager.
         * @param assert_regex The inner regex of the lookaround.
         * @param dir         FORWARD for lookahead, BACKWARD for lookbehind.
         * @param is_positive True for (?=...) or (?<=...), false for (?!...) or (?<!...).
         * @return GraphFragment The constructed assertion fragment.
         */
        static GraphFragment make_assertion_fragment(RegexConstraintGraph& graph, ast_manager& m, app_ref assert_regex,
                                                     LookaroundDirection dir, bool is_positive);

        /**
         * @brief Create the subgraph for '\b' or '\B' (word boundary / non-word boundary).
         *
         * @param graph           The RCG to add the fragment to.
         * @param util_s          Z3 sequence/regex utilities.
         * @param m               The Z3 AST manager.
         * @param is_word_boundary True for '\b', false for '\B'.
         * @return GraphFragment The word-boundary subgraph.
         */
        static GraphFragment make_word_boundary_fragment(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m,
                                                         bool is_word_boundary);
    };

    /**
     * Term :: Atom Quantifier -- an atom repeated according to the quantifier (*, +, ?, {n}, {n,}, {n,m}).
     *
     * If the atom is regular, get_subgraph() builds the corresponding Z3 regex. If the atom is non-regular and the
     * quantifier is bounded, the quantifier is unrolled into an explicit disjunction first.
     */
    class ASTNodeQuantified : public ASTNode {
    public:
        ASTNodeQuantified(Quantifier quantifier, ASTNodeRef atom);

        uint64_t print_dot(std::ostream& out, uint64_t& node_count) const override;
        zstring serialize() const override;
        const Quantifier& get_quantifier() const;
        ASTNodeRef clone() const override;
        void strip_captures() override;

        /**
         * @brief Expand a bounded quantifier {min,max} into an explicit disjunction.
         *
         * Produces an ASTNodeDisjunction equivalent to:
         *   (epsilon | atom^min | ... | atom^max)
         * where epsilon is included only when min == 0.
         *
         * @return ASTNodeRef A new ASTNodeDisjunction semantically equivalent to the quantified node.
         */
        ASTNodeRef unroll() const;

        /**
         * @brief Build the regex/subgraph for a quantified atom.
         *
         * Regular atom -- create a {min, max} loop which keeps it regular.
         * Nonregular atom:
         *      - Bounded quantifier -- unroll into an explicit disjunction of concatenations.
         *      - Unbounded quantifier -- unsupported, leads to dynamic number of string variables and constraints.
         *
         * @return RegexComponent app_ref if the atom is regular, GraphFragment otherwise.
         */
        RegexComponent get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const override;
        void collect_backrefs(std::unordered_set<GroupID>& refs) const override;
        void strip_unreferenced_captures(const std::unordered_set<GroupID>& referenced) override;

    private:
        Quantifier m_quantifier;
        ASTNodeRef m_atom;
    };

    /**
     * A single character: Atom :: PatternCharacter, or Atom :: \ CharacterEscape. The stored value is the
     * CharacterValue of the character (21.2.1.4).
     */
    class ASTNodeCharacter : public ASTNode {
    public:
        explicit ASTNodeCharacter(Z3Char ch);

        uint64_t print_dot(std::ostream& out, uint64_t& node_count) const override;
        zstring serialize() const override;
        Z3Char get_char() const;
        RegexComponent get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const override;
        ASTNodeRef clone() const override;

    private:
        Z3Char m_char;
    };

    /**
     * A backreference: Atom :: \ DecimalEscape (\1) or Atom :: \k GroupName (\k<name>). Named backreferences are
     * resolved to the group number by the parser. Always returns a GraphFragment with a single BackrefEdge.
     */
    class ASTNodeBackreference : public ASTNode {
    public:
        explicit ASTNodeBackreference(GroupID group_id, zstring_view group_name = zstring_view());

        uint64_t print_dot(std::ostream& out, uint64_t& node_count) const override;
        zstring serialize() const override;
        GroupID get_group_id() const;
        void set_group_id(GroupID group_id);
        zstring_view get_group_name() const;
        RegexComponent get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const override;
        ASTNodeRef clone() const override;
        void collect_backrefs(std::unordered_set<GroupID>& refs) const override;

    private:
        GroupID m_group_id;
        zstring_view m_group_name;  // source text of the GroupName, empty for numeric backreferences
    };

    /**
     * A group: Atom :: ( GroupSpecifier Disjunction ) for capturing groups (optionally named), or
     * Atom :: (?: Disjunction ) for non-capturing groups.
     */
    class ASTNodeGroup : public ASTNode {
    public:
        /**
         * @brief Create a non-capturing group.
         */
        explicit ASTNodeGroup(ASTNodeRef child);

        /**
         * @brief Create a capturing group with the number @p gid and optionally the @p name.
         */
        ASTNodeGroup(GroupID gid, ASTNodeRef child, zstring_view name = zstring_view());

        uint64_t print_dot(std::ostream& out, uint64_t& node_count) const override;
        zstring serialize() const override;
        bool is_capturing() const;
        GroupID get_id() const;
        zstring_view get_name() const;

        /**
         * @brief Build the subgraph for a group node.
         *
         * Non-capture groups return the child's subgraph directly.
         *
         * Capture groups ensure the subregex is represented as a GraphFragment, then annotate the fragment's boundary
         * edges with the group ID so the constraint generation can track which edge variables fall inside this group's
         * capture context.
         *
         * @return RegexComponent The child's subgraph, annotated with group boundaries
         *                        for capture groups.
         */
        RegexComponent get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const override;
        ASTNodeRef clone() const override;
        void strip_captures() override;
        void collect_backrefs(std::unordered_set<GroupID>& refs) const override;
        void strip_unreferenced_captures(const std::unordered_set<GroupID>& referenced) override;

    private:
        bool m_capturing;
        GroupID m_gid = 0;
        zstring_view m_name;  // source text of the GroupName, empty for unnamed groups
        ASTNodeRef m_child;
    };

    /**
     * A character class: Atom :: CharacterClass (like [abc], [^a-z\d]). A predefined set of characters outside of a
     * class (e.g. \d or .) is represented as a class with a single item.
     *
     * The items of the class are stored as a flat list of ranges (single characters are ranges {c, c}), predefined sets
     * of characters and Unicode properties. The class matches the union of its items, or its complement if negated.
     */
    class ASTNodeCharClass : public ASTNode {
    public:
        explicit ASTNodeCharClass(bool negated = false);

        uint64_t print_dot(std::ostream& out, uint64_t& node_count) const override;
        zstring serialize() const override;
        void add_item(ClassItem item);
        bool is_negated() const;
        const std::vector<ClassItem>& get_items() const;
        RegexComponent get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const override;
        ASTNodeRef clone() const override;

    private:
        bool m_is_negated;
        std::vector<ClassItem> m_items;
    };

    // ================= DFS CONTEXT CLASSES ==================

    /**
     * A postponed lookahead assertion waiting to be evaluated at the end of the current path.
     *
     * Lookaheads cannot be evaluated at the point where they appear in the graph because they constrain the suffix of
     * the matched string, which is not yet known at that point. Instead, they are pushed onto a list and evaluated when
     * the graph traversal reaches the end vertex.
     *
     * `start_index` is the index into `m_current_path_vars` at the moment the lookahead was encountered.
     * When the lookahead is evaluated at the end vertex, the suffix of the string is obtained by concatenating the
     * string variables from `start_index` to the end of `m_current_path_vars`.
     */
    struct ActiveLookahead {
        std::variant<app_ref, GraphFragment> subregex;
        bool is_positive;
        std::size_t start_index;
    };

    /**
     * A snapshot of the DFS context at a concrete point in the traversal.
     *
     * Saved before entering each outgoing edge of a vertex so that, after exploring one branch of an alternation, the
     * context can be fully restored before entering the next branch.
     *
     * Group variable vectors are deep-copied into this snapshot. This is necessary because restore_snapshot() must undo
     * any appends that occurred during the already explored branch.
     */
    struct DFSStateSnapshot {
    public:
        unsigned num_path_vars = 0;
        unsigned num_path_constraints = 0;
        unsigned num_active_lookaheads = 0;
        std::vector<GroupID> active_groups;

        std::unordered_map<GroupID, std::unique_ptr<expr_ref_vector>> group_vars;
    };

    /**
     * Holds the full outer DFS state while an inner DFS is running for a non-regular lookaround. The outer state is
     * moved into this struct by `suspend_for_inner_search()` and restored afterwards by `resume_from_inner_search()`.
     */
    struct OuterSearchState {
        std::unique_ptr<expr_ref_vector> unique_paths;
        std::unique_ptr<expr_ref_vector> current_path_vars;
        std::unique_ptr<expr_ref_vector> current_path_constraints;
        std::vector<GroupID> active_groups;
        std::vector<ActiveLookahead> active_lookaheads;
        VertexID end_vertex;
        app* target_string;
        std::vector<GroupID> existing_group_ids;
    };

    /**
     * Carries and manages all mutable state for a single DFS traversal of the RCG.
     *
     * The DFS explores every path from start_vertex to end_vertex. Along the way, it accumulates edge variables
     * (`m_current_path_vars`), constraints on those variables (`m_current_path_constraints`), and lists of variables
     * for each capture group (`m_group_vars`). When a complete path is found, commit_current_path() merges all
     * constraints into one conjunction and adds it to `m_unique_paths`.
     *
     * At vertices with multiple outgoing edges, `save_snapshot()` / `restore_snapshot()` are used to backtrack between
     * branches.
     *
     * For non-regular lookarounds, the outer DFS state is moved into an `OuterSearchState` via
     * `suspend_for_inner_search()`, a nested DFS runs with fresh state, and then `resume_from_inner_search()` restores
     * it.
     *
     * `m_base_prefix` holds all the string variables accumulated by outer DFS calls.
     * `get_global_prefix()` concatenates `m_base_prefix` with the current inner DFS path's variables to give the full
     * prefix at the current position in the original target string.
     */
    class DFSContext {
    public:
        DFSContext(ast_manager& manager, seq_util& util)
            : m_manager(manager),
              m_util_s(util),
              m_str_sort(util.mk_string_sort()),
              m_base_prefix(manager),
              m_unique_paths(manager),
              m_current_path_vars(manager),
              m_current_path_constraints(manager),
              m_all_created_vars(manager) { }

        void set_target(app* target);
        app* get_target() const;
        void set_end_vertex(VertexID v);
        VertexID get_end_vertex() const;
        void set_base_prefix(const expr_ref& p);
        expr_ref get_base_prefix() const;

        /**
         * @brief Save the current DFS state before exploring one outgoing edge.
         *
         * The snapshot captures the sizes of the path variable and constraint vectors, the active lookahead count, the
         * list of active groups, and deep copies of all group variable vectors. Used for backtracking between branches
         * in the DFS.
         *
         * @return DFSStateSnapshot The current state snapshot.
         */
        DFSStateSnapshot save_snapshot() const;

        /**
         * @brief Restore the DFS state to a previously saved @p snapshot after an edge visit.
         *
         * Fully undoes everything that was changed during the edge visit, including appends to path variable and
         * constraint vectors, modifications to active lookaheads and groups, and appends to group variable vectors.
         * @param snapshot The snapshot to restore from.
         */
        void restore_snapshot(const DFSStateSnapshot& snapshot);

        /**
         * @brief Suspend the current outer DFS so a DFS for a subgraph can start a fresh search.
         *
         * Moves the whole state into an `OuterSearchState`, then resets the context to a clean empty state ready for
         * the inner search.
         *
         * @return OuterSearchState The suspended outer state.
         */
        OuterSearchState suspend_for_inner_search();

        /**
         * @brief Restore the outer DFS @p state after an inner search has completed.
         *
         * @param state The OuterSearchState returned by suspend_for_inner_search().
         */
        void resume_from_inner_search(OuterSearchState& state);

        /**
         * @brief Create a fresh string variable without adding it to the path.
         *
         * The variable is also tracked in the internal list of all created fresh variables so it can later
         * be existentially quantified over in the generated formula.
         *
         * @return app_ref The new string variable.
         */
        app_ref mk_fresh_string_var();

        /**
         * @brief Return all fresh string variables created during the entire DFS (including inner DFS runs).
         *
         * Used to existentially quantify over all auxiliary variables introduced during constraint generation.
         *
         * @return app_ref_vector The list of all created fresh variables.
         */
        const app_ref_vector& get_all_fresh_vars() const;

        /**
         * @brief Create a fresh string variable for the current edge and append it to the path variable list.
         *
         * The variable is also registered with all currently open capture groups via
         * push_edge_var_to_groups (called separately by the caller after this).
         *
         * @return app_ref The new edge variable.
         */

        /**
         * @brief Create a fresh string variable for the current edge and append it to the path variable list.
         *
         * The variable is also registered with all currently open capture groups via
         * push_edge_var_to_groups (called separately by the caller after this).
         *
         * @return app_ref The new edge variable.
         */
        app_ref create_edge_var();

        /**
         * @brief Add the @p edge_var to variable list of every currently open capture group.
         *
         * @param edge_var The edge variable to add to the active groups.
         */
        void push_edge_var_to_groups(const app_ref& edge_var);

        /**
         * @brief Add a @p constraint to the current path's constraint list.
         *
         * @param constraint The constraint to add.
         */
        void add_path_constraint(const app_ref& constraint);

        /**
         * @brief Marks all the capture groups, that start on the current edge, as active.
         *
         * Therefore, from this point, any string variable created during the traversal will be added a variable
         * list of each of these groups until they are closed.
         *
         * @param gids The group IDs to open.
         */
        void start_groups(const std::vector<GroupID>& gids);

        /**
         * @brief Marks all the capture groups, that end on the current edge, as inactive.
         *
         * After this, new edge variables will no longer be added to the variable list of these groups.
         *
         * @param gids The group IDs to close.
         */
        void end_groups(const std::vector<GroupID>& gids);

        bool has_group(GroupID gid) const;

        /**
         * @brief Postpone a lookahead assertion for evaluation at the end of the current path.
         *
         * Saves the @p subregex, polarity and current position in the matched string.
         *
         * @param subregex    The lookahead subregex.
         * @param is_positive The lookahead polarity.
         */
        void push_lookahead(const std::variant<app_ref, GraphFragment>& subregex, bool is_positive);

        const std::vector<ActiveLookahead>& get_active_lookaheads() const;

        /**
         * @brief Take all the constraints accumulated during the current traversal and conjunct them.
         *
         * Takes `m_current_path_constraints` with @p additional_constraints (postponed lookaheads' constraints),
         * creates an equality `target_string` = concat(`edge_vars`), and stores the resulting conjunction in
         * `m_unique_paths`.
         *
         * @param additional_constraints Extra (lookahead) constraints to include.
         */
        void commit_current_path(const expr_ref_vector& additional_constraints);

        expr_ref_vector& get_unique_paths();

        /**
         * @brief Concatenate generated edge variables accumulated on the current path from @p start_idx to @p end_idx.
         *
         * @param start_idx Index of the first variable to include.
         * @param end_idx   One-past-last index (default: all variables).
         * @return expr_ref The concatenation of the selected edge variables.
         */
        expr_ref concat_vars(uint32_t start_idx = 0,
                             uint32_t end_idx = std::numeric_limits<uint32_t>::max()) const;

        /**
         * @brief Return the prefix of the target string matched from the beginning, across all DFS levels.
         *
         * @return expr_ref The global prefix expression.
         */
        expr_ref get_global_prefix() const;

        /**
         * @brief Concatenate all edge variables captured by a group @p gid on the current path.
         *
         * @param gid The capture group whose variable list to concatenate.
         * @return expr_ref The string captured by the group on the current path.
         */
        expr_ref concat_group_vars(GroupID gid) const;

    private:
        ast_manager& m_manager;
        seq_util& m_util_s;
        sort* m_str_sort;

        app* m_target_string = nullptr;
        VertexID m_end_vertex = UNKNOWN_VERTEX;
        expr_ref m_base_prefix;

        expr_ref_vector m_unique_paths;
        expr_ref_vector m_current_path_vars;
        expr_ref_vector m_current_path_constraints;

        std::vector<GroupID> m_active_groups;
        std::unordered_map<GroupID, expr_ref_vector> m_group_vars;
        std::vector<ActiveLookahead> m_active_lookaheads;

        // Accumulates every fresh variable created via mk_fresh_string_var() across the entire DFS (including inner
        // DFS runs for lookarounds). Used to existentially quantify over auxiliary variables in the final formula.
        app_ref_vector m_all_created_vars;

        expr_ref concat_expr_vector(const expr_ref_vector& vars, uint32_t start_idx, uint32_t end_idx) const;
    };

    // =============== REGEX CONSTRAINT BUILDER ===============

    /**
     * Top-level class for translating an extended regex (of any flavor) into Z3 string constraints.
     *
     * Usage:
     *   1. Construct with the regex (a frontend of some flavor, see ExtendedRegex). The regex must outlive the builder.
     *   2. Call build_rcg() once to parse the regex and build the RCG.
     *   3. Call generate_constraints(target) to produce an SMT2 formula that is satisfiable
     *      iff target matches the regex.
     */
    class RegexConstraintBuilder {
    public:
        RegexConstraintBuilder(ast_manager& m, const ExtendedRegex& regex, const theory_str_noodler_params& params)
            : m_regex(regex),
              m_manager(m),
              m_params(params),
              m_util_s(m),
              m_str_sort(m_util_s.mk_string_sort()),
              m_fresh_vars(m) { }

        /**
         * @brief Build the Regex Constraint Graph (RCG) from the regex pattern.
         *
         * Parses the regex, calls get_subgraph() on the root AST node to obtain the core
         * subgraph (or a single app_ref for a fully regular regex). If the parameter `str.ecma_engine_semantics` is
         * set, the core subgraph is wrapped:
         *
         *   start_vertex --(Sigma*)--> core_v_in
         *                              ...core subgraph...
         *                              core_v_out --(Sigma*)--> end_vertex
         *
         * The Sigma* edges model the regex engine matching semantics -- any substring can be matched. Otherwise, the
         * whole string must match the regex.
         *
         * @return RegexConstraintGraph The fully assembled RCG.
         */
        RegexConstraintGraph build_rcg();

        /**
         * @brief Generate constraints for the given target string based on the RCG built from the regex pattern.
         *
         * Returns a ground formula whose auxiliary (fresh) string variables are Skolem constants. The list of those
         * constants is available via get_fresh_vars() after this call returns.
         *
         * @param target_string The string for which the regex constraints are to be generated.
         * @return expr_ref A ground Z3 expression that is satisfiable iff target matches the regex pattern.
         */
        expr_ref generate_constraints(app* target_string);

        /**
         * @brief Return the fresh Skolem variables introduced during the last generate_constraints() call.
         *
         * Used by the caller to build the existentially-quantified form needed for the negative implication.
         *
         * @return const app_ref_vector& All fresh variables created during DFS (including inner DFS for lookarounds).
         */
        const app_ref_vector& get_fresh_vars() const;

    private:
        const ExtendedRegex& m_regex;
        ast_manager& m_manager;
        const theory_str_noodler_params& m_params;
        seq_util m_util_s;
        RegexConstraintGraph m_graph;
        sort* m_str_sort = nullptr;

        // used by anchor evaluation -- ^ and $ compare against the full original target string
        app* m_global_target_string = nullptr;

        // Populated by generate_constraints(); holds every fresh Skolem constant introduced during DFS.
        app_ref_vector m_fresh_vars;

        /**
         * @brief Run an inner DFS on a subgraph to evaluate a non-regular lookaround.
         *
         * @param fragment      The sub-fragment whose paths are to be enumerated.
         * @param target_string The string the inner search must match against.
         * @param ctx           The outer DFS context (will be suspended and restored).
         * @return expr_ref     OR of all satisfying path formulas for the inner search,
         *                      or false if no matching path exists.
         */
        expr_ref run_inner_rcg_dfs(const GraphFragment& fragment, app* target_string, DFSContext& ctx);

        /**
         * @brief Handle the lookaround assertions postponed during DFS traversal by generating constraints based on the
         * lookaround type and its subregex.
         *
         * @param ctx           The DFS context.
         * @param assertion     The lookaround assertion edge for which constraints are to be generated.
         * @param global_prefix Current global prefix of the traversal path, used for evaluating lookbehind constraints.
         */
        void handle_lookaround_constraints(DFSContext& ctx, const AssertionEdge& assertion,
                                           const expr_ref& global_prefix);

        /**
         * @brief Generate lookahead constraints for the current path.
         *
         * @param ctx               The DFS context.
         * @param final_constraints The list of generated lookahead constraints.
         */
        void generate_lookahead_constraints(DFSContext& ctx, expr_ref_vector& final_constraints);

        /**
         * @brief Generate string constraints for a specific @p edge in the RCG based on the @p edge type.
         *
         * @param ctx The DFS context containing the current path state and accumulated constraints.
         * @param edge The edge for which constraints are to be generated.
         * @param edge_var The string variable representing the @p edge.
         */
        void generate_edge_constraints(DFSContext& ctx, const RCGEdge& edge, const app_ref& edge_var);

        /**
         * @brief One step of the DFS traversal of RCG.
         * Explores all outgoing edges of the @p current_vertex and generates constraints for each of them.
         *
         * @param current_vertex The vertex whose outgoing edges are currently being explored
         * @param ctx The context carrying DFS state accumulated on the curently explored path.
         */
        void rcg_dfs_visit(VertexID current_vertex, DFSContext& ctx);
    };
}  // namespace smt::noodler::extended_regex
