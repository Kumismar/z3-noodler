#include "ecma_regex.h"

#include "ast/ast.h"
#include "ast/seq_decl_plugin.h"
#include "util.h"
#include "util/debug.h"
#include "util/zstring_view.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace smt::noodler::ecma {
    // ======================= UTILS =======================
    constexpr bool debug_mode = false;

    // Line terminators (ECMA-262 2020, 11.3 Line Terminators, Table 33)
    constexpr Z3Char CH_LF = 0x000A;  // Line Feed
    constexpr Z3Char CH_CR = 0x000D;  // Carriage Return
    constexpr Z3Char CH_LS = 0x2028;  // Line Separator
    constexpr Z3Char CH_PS = 0x2029;  // Paragraph Separator

    constexpr Z3Char CH_BACKSPACE = 0x0008;
    constexpr Z3Char CH_ZWNJ = 0x200C;  // Zero Width Non-Joiner
    constexpr Z3Char CH_ZWJ = 0x200D;   // Zero Width Joiner
    constexpr Z3Char CH_REPLACEMENT = 0xFFFD;
    constexpr Z3Char MAX_CODE_POINT = 0x10FFFF;

    // Characters matched by \s: WhiteSpace (11.2, Table 32, including the "Zs" category) and LineTerminator (11.3,
    // Table 33), see 21.2.2.12 CharacterClassEscape. Stored as inclusive ranges.
    constexpr std::array<ClassRange, 10> WHITESPACE_RANGES {{
        {0x0009, 0x000D},  // <TAB>, <LF>, <VT>, <FF>, <CR>
        {0x0020, 0x0020},  // <SP>
        {0x00A0, 0x00A0},  // <NBSP>
        {0x1680, 0x1680},  // Zs: OGHAM SPACE MARK
        {0x2000, 0x200A},  // Zs: EN QUAD .. HAIR SPACE
        {0x2028, 0x2029},  // <LS>, <PS>
        {0x202F, 0x202F},  // Zs: NARROW NO-BREAK SPACE
        {0x205F, 0x205F},  // Zs: MEDIUM MATHEMATICAL SPACE
        {0x3000, 0x3000},  // Zs: IDEOGRAPHIC SPACE
        {0xFEFF, 0xFEFF},  // <ZWNBSP>
    }};

    // Unicode property names and values allowed in \p{...} and \P{...} (ECMA-262 2020, 21.2.2.8.3 and 21.2.2.8.4).
    // Table 55: Non-binary Unicode property aliases
    constexpr std::array<const char*, 2> PROPERTY_GENERAL_CATEGORY {"General_Category", "gc"};
    constexpr std::array<const char*, 4> PROPERTY_SCRIPT {"Script", "sc", "Script_Extensions", "scx"};

    // Table 56: Binary Unicode property aliases
    constexpr std::array<const char*, 93> BINARY_PROPERTIES {
        "ASCII", "ASCII_Hex_Digit", "AHex", "Alphabetic", "Alpha", "Any", "Assigned", "Bidi_Control", "Bidi_C",
        "Bidi_Mirrored", "Bidi_M", "Case_Ignorable", "CI", "Cased", "Changes_When_Casefolded", "CWCF",
        "Changes_When_Casemapped", "CWCM", "Changes_When_Lowercased", "CWL", "Changes_When_NFKC_Casefolded", "CWKCF",
        "Changes_When_Titlecased", "CWT", "Changes_When_Uppercased", "CWU", "Dash", "Default_Ignorable_Code_Point",
        "DI", "Deprecated", "Dep", "Diacritic", "Dia", "Emoji", "Emoji_Component", "Emoji_Modifier",
        "Emoji_Modifier_Base", "Emoji_Presentation", "Extended_Pictographic", "Extender", "Ext", "Grapheme_Base",
        "Gr_Base", "Grapheme_Extend", "Gr_Ext", "Hex_Digit", "Hex", "IDS_Binary_Operator", "IDSB",
        "IDS_Trinary_Operator", "IDST", "ID_Continue", "IDC", "ID_Start", "IDS", "Ideographic", "Ideo",
        "Join_Control", "Join_C", "Logical_Order_Exception", "LOE", "Lowercase", "Lower", "Math",
        "Noncharacter_Code_Point", "NChar", "Pattern_Syntax", "Pat_Syn", "Pattern_White_Space", "Pat_WS",
        "Quotation_Mark", "QMark", "Radical", "Regional_Indicator", "RI", "Sentence_Terminal", "STerm", "Soft_Dotted",
        "SD", "Terminal_Punctuation", "Term", "Unified_Ideograph", "UIdeo", "Uppercase", "Upper", "Variation_Selector",
        "VS", "White_Space", "space", "XID_Continue", "XIDC", "XID_Start", "XIDS"};

    // Table 57: Value aliases and canonical values for the Unicode property General_Category
    constexpr std::array<const char*, 80> GENERAL_CATEGORY_VALUES {
        "Cased_Letter", "LC", "Close_Punctuation", "Pe", "Connector_Punctuation", "Pc", "Control", "Cc", "cntrl",
        "Currency_Symbol", "Sc", "Dash_Punctuation", "Pd", "Decimal_Number", "Nd", "digit", "Enclosing_Mark", "Me",
        "Final_Punctuation", "Pf", "Format", "Cf", "Initial_Punctuation", "Pi", "Letter", "L", "Letter_Number", "Nl",
        "Line_Separator", "Zl", "Lowercase_Letter", "Ll", "Mark", "M", "Combining_Mark", "Math_Symbol", "Sm",
        "Modifier_Letter", "Lm", "Modifier_Symbol", "Sk", "Nonspacing_Mark", "Mn", "Number", "N", "Open_Punctuation",
        "Ps", "Other", "C", "Other_Letter", "Lo", "Other_Number", "No", "Other_Punctuation", "Po", "Other_Symbol", "So",
        "Paragraph_Separator", "Zp", "Private_Use", "Co", "Punctuation", "P", "punct", "Separator", "Z",
        "Space_Separator", "Zs", "Spacing_Mark", "Mc", "Surrogate", "Cs", "Symbol", "S", "Titlecase_Letter", "Lt",
        "Unassigned", "Cn", "Uppercase_Letter", "Lu"};

    // Table 58: Value aliases and canonical values for the Unicode properties Script and Script_Extensions
    constexpr std::array<const char*, 300> SCRIPT_VALUES {
        "Adlam", "Adlm", "Ahom", "Anatolian_Hieroglyphs", "Hluw", "Arabic", "Arab", "Armenian", "Armn", "Avestan",
        "Avst", "Balinese", "Bali", "Bamum", "Bamu", "Bassa_Vah", "Bass", "Batak", "Batk", "Bengali", "Beng",
        "Bhaiksuki", "Bhks", "Bopomofo", "Bopo", "Brahmi", "Brah", "Braille", "Brai", "Buginese", "Bugi", "Buhid",
        "Buhd", "Canadian_Aboriginal", "Cans", "Carian", "Cari", "Caucasian_Albanian", "Aghb", "Chakma", "Cakm", "Cham",
        "Cherokee", "Cher", "Common", "Zyyy", "Coptic", "Copt", "Qaac", "Cuneiform", "Xsux", "Cypriot", "Cprt",
        "Cyrillic", "Cyrl", "Deseret", "Dsrt", "Devanagari", "Deva", "Dogra", "Dogr", "Duployan", "Dupl",
        "Egyptian_Hieroglyphs", "Egyp", "Elbasan", "Elba", "Elymaic", "Elym", "Ethiopic", "Ethi", "Georgian", "Geor",
        "Glagolitic", "Glag", "Gothic", "Goth", "Grantha", "Gran", "Greek", "Grek", "Gujarati", "Gujr", "Gunjala_Gondi",
        "Gong", "Gurmukhi", "Guru", "Han", "Hani", "Hangul", "Hang", "Hanifi_Rohingya", "Rohg", "Hanunoo", "Hano",
        "Hatran", "Hatr", "Hebrew", "Hebr", "Hiragana", "Hira", "Imperial_Aramaic", "Armi", "Inherited", "Zinh", "Qaai",
        "Inscriptional_Pahlavi", "Phli", "Inscriptional_Parthian", "Prti", "Javanese", "Java", "Kaithi", "Kthi",
        "Kannada", "Knda", "Katakana", "Kana", "Kayah_Li", "Kali", "Kharoshthi", "Khar", "Khmer", "Khmr", "Khojki",
        "Khoj", "Khudawadi", "Sind", "Lao", "Laoo", "Latin", "Latn", "Lepcha", "Lepc", "Limbu", "Limb", "Linear_A",
        "Lina", "Linear_B", "Linb", "Lisu", "Lycian", "Lyci", "Lydian", "Lydi", "Mahajani", "Mahj", "Makasar", "Maka",
        "Malayalam", "Mlym", "Mandaic", "Mand", "Manichaean", "Mani", "Marchen", "Marc", "Medefaidrin", "Medf",
        "Masaram_Gondi", "Gonm", "Meetei_Mayek", "Mtei", "Mende_Kikakui", "Mend", "Meroitic_Cursive", "Merc",
        "Meroitic_Hieroglyphs", "Mero", "Miao", "Plrd", "Modi", "Mongolian", "Mong", "Mro", "Mroo", "Multani", "Mult",
        "Myanmar", "Mymr", "Nabataean", "Nbat", "Nandinagari", "Nand", "New_Tai_Lue", "Talu", "Newa", "Nko", "Nkoo",
        "Nushu", "Nshu", "Nyiakeng_Puachue_Hmong", "Hmnp", "Ogham", "Ogam", "Ol_Chiki", "Olck", "Old_Hungarian", "Hung",
        "Old_Italic", "Ital", "Old_North_Arabian", "Narb", "Old_Permic", "Perm", "Old_Persian", "Xpeo", "Old_Sogdian",
        "Sogo", "Old_South_Arabian", "Sarb", "Old_Turkic", "Orkh", "Oriya", "Orya", "Osage", "Osge", "Osmanya", "Osma",
        "Pahawh_Hmong", "Hmng", "Palmyrene", "Palm", "Pau_Cin_Hau", "Pauc", "Phags_Pa", "Phag", "Phoenician", "Phnx",
        "Psalter_Pahlavi", "Phlp", "Rejang", "Rjng", "Runic", "Runr", "Samaritan", "Samr", "Saurashtra", "Saur",
        "Sharada", "Shrd", "Shavian", "Shaw", "Siddham", "Sidd", "SignWriting", "Sgnw", "Sinhala", "Sinh", "Sogdian",
        "Sogd", "Sora_Sompeng", "Sora", "Soyombo", "Soyo", "Sundanese", "Sund", "Syloti_Nagri", "Sylo", "Syriac", "Syrc",
        "Tagalog", "Tglg", "Tagbanwa", "Tagb", "Tai_Le", "Tale", "Tai_Tham", "Lana", "Tai_Viet", "Tavt", "Takri", "Takr",
        "Tamil", "Taml", "Tangut", "Tang", "Telugu", "Telu", "Thaana", "Thaa", "Thai", "Tibetan", "Tibt", "Tifinagh",
        "Tfng", "Tirhuta", "Tirh", "Ugaritic", "Ugar", "Vai", "Vaii", "Wancho", "Wcho", "Warang_Citi", "Wara", "Yi",
        "Yiii", "Zanabazar_Square", "Zanb"};

    // Compares @p str with the ASCII string @p ascii.
    static bool equals_ascii(const zstring_view str, const char* ascii) {
        uint32_t i = 0;
        for (; ascii[i] != '\0'; i++) {
            if (i >= str.length() || str[i] != static_cast<unsigned char>(ascii[i])) {
                return false;
            }
        }
        return i == str.length();
    }

    template <std::size_t N>
    static bool contains_name(const std::array<const char*, N>& names, const zstring_view name) {
        return std::any_of(names.begin(), names.end(), [&](const char* candidate) {
            return equals_ascii(name, candidate);
        });
    }

    static bool is_decimal_digit(const Z3Char ch) {
        return ch >= '0' && ch <= '9';
    }

    static bool is_hex_digit(const Z3Char ch) {
        return is_decimal_digit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    }

    static uint32_t hex_digit_value(const Z3Char ch) {
        if (is_decimal_digit(ch)) {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        return ch - 'A' + 10;
    }

    static bool is_ascii_letter(const Z3Char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    }

    // SyntaxCharacter :: one of ^ $ \ . * + ? ( ) [ ] { } |   (21.2.1)
    static bool is_syntax_character(const Z3Char ch) {
        switch (ch) {
            case '^':
            case '$':
            case '\\':
            case '.':
            case '*':
            case '+':
            case '?':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '|':
                return true;
            default:
                return false;
        }
    }

    static bool is_lead_surrogate(const Z3Char ch) {
        return ch >= 0xD800 && ch <= 0xDBFF;
    }

    static bool is_trail_surrogate(const Z3Char ch) {
        return ch >= 0xDC00 && ch <= 0xDFFF;
    }

    // Printable ASCII characters are printed quoted ('a'), all other characters as U+XXXX.
    static std::string char_to_string(const Z3Char ch) {
        if (ch >= 0x20 && ch <= 0x7E) {
            return std::string("'") + static_cast<char>(ch) + "'";
        }
        std::ostringstream out;
        out << "U+" << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << ch;
        return out.str();
    }

    static zstring char_to_zstring(const Z3Char ch) {
        return zstring(char_to_string(ch).c_str());
    }

    // Characters above the maximal character of the current string encoding cannot be represented in Z3.
    static void check_representable_char(const Z3Char ch, const seq_util& util_s) {
        if (ch > util_s.max_char()) {
            util::throw_error("ECMA regex unsupported: character " + char_to_string(ch) +
                              " is above the maximal character supported by the string encoding");
        }
    }

    // Sigma \ chars, i.e., all characters except the ones matched by @p chars.
    static app* mk_char_complement(seq_util& util_s, app* chars) {
        return util_s.re.mk_inter(util_s.re.mk_full_char(nullptr), util_s.re.mk_complement(chars));
    }

    zstring sanitize_ecma_regex_input(const zstring& raw_input) {
        std::vector<unsigned> sanitized;
        sanitized.reserve(raw_input.length());

        auto is_continuation = [&](const uint32_t idx) -> bool {
            if (idx >= raw_input.length()) {
                return false;
            }
            const bool is_raw_byte = raw_input[idx] <= 0xFF;
            const bool is_continuation_byte = (raw_input[idx] & 0xC0) == 0x80;  // top two bits must be 10xxxxxx
            return is_raw_byte && is_continuation_byte;
        };

        uint32_t i = 0;
        while (i < raw_input.length()) {
            const Z3Char raw_char = raw_input[i];

            // If the character value is > 0xFF, the zstring constructor already parsed it into a valid Unicode code
            // point. Originally, the character was in form \u{XXXX} or similar in the SMT-LIB string literal.
            if (raw_char > 0xFF) {
                sanitized.push_back(raw_char);
                i++;
                continue;
            }

            if (raw_char < 0x80) {
                // 0xxxxxxx (1 byte)
                sanitized.push_back(raw_char);
                i++;
            } else if ((raw_char & 0xE0) == 0xC0) {
                // 110xxxxx 10xxxxxx (2 bytes)
                if (!is_continuation(i + 1)) {
                    sanitized.push_back(CH_REPLACEMENT);
                    i++;
                    continue;
                }
                const Z3Char code_point = ((raw_char & 0x1F) << 6) | (raw_input[i + 1] & 0x3F);
                sanitized.push_back(code_point < 0x80 ? CH_REPLACEMENT : code_point);
                i += 2;
            } else if ((raw_char & 0xF0) == 0xE0) {
                // 1110xxxx 10xxxxxx 10xxxxxx (3 bytes)
                if (!is_continuation(i + 1) || !is_continuation(i + 2)) {
                    sanitized.push_back(CH_REPLACEMENT);
                    i++;
                    continue;
                }
                const Z3Char code_point =
                    ((raw_char & 0x0F) << 12) | ((raw_input[i + 1] & 0x3F) << 6) | (raw_input[i + 2] & 0x3F);
                const bool is_invalid = code_point < 0x800 || (code_point >= 0xD800 && code_point <= 0xDFFF);
                sanitized.push_back(is_invalid ? CH_REPLACEMENT : code_point);
                i += 3;
            } else if ((raw_char & 0xF8) == 0xF0) {
                // 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx (4 bytes)
                if (!is_continuation(i + 1) || !is_continuation(i + 2) || !is_continuation(i + 3)) {
                    sanitized.push_back(CH_REPLACEMENT);
                    i++;
                    continue;
                }
                const Z3Char code_point = ((raw_char & 0x07) << 18) | ((raw_input[i + 1] & 0x3F) << 12) |
                                          ((raw_input[i + 2] & 0x3F) << 6) | (raw_input[i + 3] & 0x3F);
                const bool is_invalid = code_point < 0x10000 || code_point > MAX_CODE_POINT;
                sanitized.push_back(is_invalid ? CH_REPLACEMENT : code_point);
                i += 4;
            } else {
                sanitized.push_back(CH_REPLACEMENT);
                i++;
            }

            if (sanitized.back() > zstring::max_char()) {
                util::throw_error("ECMA regex unsupported: character " + char_to_string(sanitized.back()) +
                                  " is above the maximal character supported by the string encoding");
            }
        }
        return {static_cast<unsigned>(sanitized.size()), sanitized.data()};
    }

    GraphFragment chain_fragments(RegexConstraintGraph& graph, const GraphFragment& first,
                                  const GraphFragment& second) {
        for (const EdgeID id : first.edges_pointing_to_vout) {
            graph.edges[id].target = second.v_in;
        }
        return GraphFragment {first.v_in, second.v_out, second.edges_pointing_to_vout};
    }

    GraphFragment alternate_fragments(RegexConstraintGraph& graph, const GraphFragment& first,
                                      const GraphFragment& second) {
        std::vector<EdgeID> new_vin_outgoing;
        for (const EdgeID id : graph.vertices[first.v_in].outgoing_edges) {
            new_vin_outgoing.push_back(id);
        }
        for (const EdgeID id : graph.vertices[second.v_in].outgoing_edges) {
            new_vin_outgoing.push_back(id);
        }

        graph.vertices[first.v_in].outgoing_edges.clear();
        graph.vertices[second.v_in].outgoing_edges.clear();

        const VertexID new_vin = graph.create_vertex(new_vin_outgoing);
        const VertexID new_vout = graph.create_vertex();

        std::vector<EdgeID> new_vout_incoming;
        for (const EdgeID id : first.edges_pointing_to_vout) {
            graph.edges[id].target = new_vout;
            new_vout_incoming.push_back(id);
        }
        for (const EdgeID id : second.edges_pointing_to_vout) {
            graph.edges[id].target = new_vout;
            new_vout_incoming.push_back(id);
        }

        return GraphFragment {new_vin, new_vout, new_vout_incoming};
    }

    GraphFragment make_epsilon_fragment(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) {
        const VertexID v_in = graph.create_vertex();
        const VertexID v_out = graph.create_vertex();
        app_ref eps(util_s.re.mk_epsilon(util_s.mk_string_sort()), m);
        const EdgeID eid = graph.create_edge(v_out, RCGEdgePayload {MatchEdge {eps}});
        graph.vertices[v_in].outgoing_edges.push_back(eid);
        return GraphFragment {v_in, v_out, {eid}};
    }

    // =============== REGEX CONSTRAINT GRAPH ==============

    void RegexConstraintGraph::add_vertex(RCGVertex vtx) {
        vertices.push_back(std::move(vtx));
    }

    VertexID RegexConstraintGraph::create_vertex() {
        VertexID new_id = vertices.size();
        vertices.emplace_back(new_id, std::vector<EdgeID> {});
        return new_id;
    }

    VertexID RegexConstraintGraph::create_vertex(std::vector<EdgeID> edge_list) {
        VertexID new_id = vertices.size();
        vertices.emplace_back(new_id, std::move(edge_list));
        return new_id;
    }

    void RegexConstraintGraph::add_edge(RCGEdge child) {
        edges.push_back(std::move(child));
    }

    EdgeID RegexConstraintGraph::create_edge() {
        EdgeID new_id = edges.size();
        edges.emplace_back(new_id, UNKNOWN_VERTEX, BackrefEdge {0u});
        return new_id;
    }

    EdgeID RegexConstraintGraph::create_edge(VertexID target_vertex, RCGEdgePayload payload) {
        EdgeID new_id = edges.size();
        edges.emplace_back(new_id, target_vertex, std::move(payload));
        return new_id;
    }

    // ================== ECMA REGEX AST ==================

    // ---------------- Disjunction ----------------

    uint64_t ASTNodeDisjunction::print_dot(std::ostream& out, uint64_t& node_count) const {
        const uint64_t id = ++node_count;
        out << "  node" << id << " [label=\"DISJUNCTION\"];\n";
        for (const ASTNodeRef& alt : m_alternatives) {
            const uint64_t child_id = alt->print_dot(out, node_count);
            out << "  node" << id << " -> node" << child_id << ";\n";
        }
        return id;
    }

    zstring ASTNodeDisjunction::serialize() const {
        zstring res("(DISJ");
        for (const auto& alt : m_alternatives) {
            res += zstring(" ");
            res += alt->serialize();
        }
        res += zstring(")");
        return res;
    }

    void ASTNodeDisjunction::add_alternative(ASTNodeRef alt) {
        m_alternatives.push_back(std::move(alt));
    }

    RegexComponent ASTNodeDisjunction::get_subgraph(RegexConstraintGraph& graph, seq_util& util_s,
                                                    ast_manager& m) const {
        std::vector<app_ref> regular_alternatives;
        std::vector<GraphFragment> nonregular_alternatives;

        // First pass -- sort regular and nonregular alternatives
        for (const ASTNodeRef& alternative : m_alternatives) {
            RegexComponent current_term = alternative->get_subgraph(graph, util_s, m);
            if (std::holds_alternative<app_ref>(current_term)) {
                regular_alternatives.push_back(std::get<app_ref>(current_term));
                continue;
            }
            nonregular_alternatives.push_back(std::get<GraphFragment>(current_term));
        }

        // All alternatives regular -- create a single regular fragment
        if (!regular_alternatives.empty() && nonregular_alternatives.empty()) {
            app_ref final_regular_segment = regular_alternatives[0];
            for (std::size_t i = 1; i < regular_alternatives.size(); i++) {
                final_regular_segment = util_s.re.mk_union(final_regular_segment, regular_alternatives[i]);
            }
            return final_regular_segment;
        }

        // Since alternation is commutative, unite all nonregular fragments first
        GraphFragment result_fragment = nonregular_alternatives[0];
        for (std::size_t i = 1; i < nonregular_alternatives.size(); i++) {
            result_fragment = alternate_fragments(graph, result_fragment, nonregular_alternatives[i]);
        }

        if (!regular_alternatives.empty()) {
            // Then, merge all regular alternatives into one regular fragment
            app_ref merged_regular = regular_alternatives[0];
            for (std::size_t i = 1; i < regular_alternatives.size(); i++) {
                merged_regular = {util_s.re.mk_union(merged_regular, regular_alternatives[i]), m};
            }

            // Lastly, unite the created regular fragment with all the non-regular ones
            VertexID reg_vout = graph.create_vertex();
            EdgeID reg_eid = graph.create_edge(reg_vout, RCGEdgePayload {MatchEdge {merged_regular}});
            VertexID reg_vin = graph.create_vertex(std::vector<EdgeID> {reg_eid});
            GraphFragment reg_fragment {reg_vin, reg_vout, {reg_eid}};

            result_fragment = alternate_fragments(graph, result_fragment, reg_fragment);
        }

        return result_fragment;
    }

    void ASTNodeDisjunction::strip_captures() {
        for (const ASTNodeRef& alternative : m_alternatives) {
            alternative->strip_captures();
        }
    }

    void ASTNodeDisjunction::collect_backrefs(std::unordered_set<GroupID>& refs) const {
        for (const ASTNodeRef& alternative : m_alternatives) {
            alternative->collect_backrefs(refs);
        }
    }

    void ASTNodeDisjunction::strip_unreferenced_captures(const std::unordered_set<GroupID>& referenced) {
        for (const ASTNodeRef& alternative : m_alternatives) {
            alternative->strip_unreferenced_captures(referenced);
        }
    }

    ASTNodeRef ASTNodeDisjunction::clone() const {
        auto cloned = std::make_unique<ASTNodeDisjunction>();
        for (const auto& alt : m_alternatives) {
            cloned->add_alternative(alt->clone());
        }
        return cloned;
    }

    // ---------------- Alternative ----------------

    uint64_t ASTNodeAlternative::print_dot(std::ostream& out, uint64_t& node_count) const {
        const uint64_t id = ++node_count;
        out << "  node" << id << " [label=\"ALTERNATIVE\"];\n";
        for (const ASTNodeRef& term : m_terms) {
            const uint64_t child_id = term->print_dot(out, node_count);
            out << "  node" << id << " -> node" << child_id << ";\n";
        }
        return id;
    }

    zstring ASTNodeAlternative::serialize() const {
        zstring res("(SEQ");
        for (const auto& term : m_terms) {
            res += zstring(" ");
            res += term->serialize();
        }
        res += zstring(")");
        return res;
    }

    void ASTNodeAlternative::add_term(ASTNodeRef term) {
        m_terms.push_back(std::move(term));
    }

    RegexComponent ASTNodeAlternative::get_subgraph(RegexConstraintGraph& graph, seq_util& util_s,
                                                    ast_manager& m) const {
        if (m_terms.empty()) {
            return app_ref(util_s.re.mk_epsilon(util_s.mk_string_sort()), m);
        }

        // First pass: merge adjacent characters into one string
        std::vector<RegexComponent> components;
        zstring literal_buf;
        auto flush_literals = [&]() {
            if (!literal_buf.empty()) {
                components.emplace_back(app_ref(util_s.re.mk_to_re(util_s.str.mk_string(literal_buf)), m));
                literal_buf = zstring();
            }
        };

        for (const ASTNodeRef& term : m_terms) {
            const auto* character = dynamic_cast<const ASTNodeCharacter*>(term.get());
            if (character != nullptr) {
                check_representable_char(character->get_char(), util_s);
                literal_buf += zstring(character->get_char());
            } else {
                flush_literals();
                components.emplace_back(term->get_subgraph(graph, util_s, m));
            }
        }
        flush_literals();

        if (components.size() == 1) {
            return components[0];
        }

        // Second pass: merge adjacent regular components into one regex
        std::vector<RegexComponent> simplified;
        for (RegexComponent& comp : components) {
            if (!simplified.empty() && std::holds_alternative<app_ref>(simplified.back()) &&
                std::holds_alternative<app_ref>(comp)) {
                simplified.back() =
                    app_ref(util_s.re.mk_concat(std::get<app_ref>(simplified.back()), std::get<app_ref>(comp)), m);
            } else {
                simplified.emplace_back(std::move(comp));
            }
        }

        if (simplified.size() == 1) {
            return simplified[0];
        }

        // Helper lambda for fragment creation
        auto to_fragment = [&](RegexComponent& component) -> GraphFragment {
            if (std::holds_alternative<app_ref>(component)) {
                const VertexID v_in = graph.create_vertex();
                const VertexID v_out = graph.create_vertex();
                EdgeID eid = graph.create_edge(v_out, RCGEdgePayload {MatchEdge {std::get<app_ref>(component)}});
                graph.vertices[v_in].outgoing_edges.push_back(eid);
                return GraphFragment {v_in, v_out, {eid}};
            }
            return std::get<GraphFragment>(component);
        };

        // Chain all the regular and non-regular fragments together in order
        GraphFragment result = to_fragment(simplified[0]);
        for (std::size_t i = 1; i < simplified.size(); i++) {
            result = chain_fragments(graph, result, to_fragment(simplified[i]));
        }
        return result;
    }

    void ASTNodeAlternative::strip_captures() {
        for (const ASTNodeRef& term : m_terms) {
            term->strip_captures();
        }
    }

    void ASTNodeAlternative::collect_backrefs(std::unordered_set<GroupID>& refs) const {
        for (const ASTNodeRef& term : m_terms) {
            term->collect_backrefs(refs);
        }
    }

    void ASTNodeAlternative::strip_unreferenced_captures(const std::unordered_set<GroupID>& referenced) {
        for (const ASTNodeRef& term : m_terms) {
            term->strip_unreferenced_captures(referenced);
        }
    }

    ASTNodeRef ASTNodeAlternative::clone() const {
        auto cloned = std::make_unique<ASTNodeAlternative>();
        for (const auto& term : m_terms) {
            cloned->add_term(term->clone());
        }
        return cloned;
    }

    // ---------------- Assertion ----------------

    static const char* assertion_label(const AssertionKind kind) {
        switch (kind) {
            case AssertionKind::START:
                return "'^'";
            case AssertionKind::END:
                return "'$'";
            case AssertionKind::WORD_BOUNDARY:
                return "'b'";
            case AssertionKind::NOT_WORD_BOUNDARY:
                return "'B'";
            case AssertionKind::LOOKAHEAD:
                return "?=";
            case AssertionKind::NEG_LOOKAHEAD:
                return "?!";
            case AssertionKind::LOOKBEHIND:
                return "?<=";
            case AssertionKind::NEG_LOOKBEHIND:
                return "?<!";
        }
        return "??";
    }

    ASTNodeAssertion::ASTNodeAssertion(const AssertionKind kind, ASTNodeRef subpattern)
        : m_kind(kind),
          m_subpattern(std::move(subpattern)) {
        SASSERT(is_lookaround() == (m_subpattern != nullptr));
    }

    uint64_t ASTNodeAssertion::print_dot(std::ostream& out, uint64_t& node_count) const {
        const uint64_t id = ++node_count;
        std::string label = std::string("ASSERTION (") + assertion_label(m_kind) + ")";
        std::erase(label, '\'');
        out << "  node" << id << " [label=\"" << label << "\"];\n";
        if (m_subpattern) {
            const uint64_t child_id = m_subpattern->print_dot(out, node_count);
            out << "  node" << id << " -> node" << child_id << ";\n";
        }
        return id;
    }

    zstring ASTNodeAssertion::serialize() const {
        zstring res = zstring("(ASSERT ") + zstring(assertion_label(m_kind));
        if (m_subpattern) {
            res += zstring(" ") + m_subpattern->serialize();
        }
        return res + zstring(")");
    }

    AssertionKind ASTNodeAssertion::get_kind() const {
        return m_kind;
    }

    bool ASTNodeAssertion::is_lookaround() const {
        return m_kind == AssertionKind::LOOKAHEAD || m_kind == AssertionKind::NEG_LOOKAHEAD ||
               m_kind == AssertionKind::LOOKBEHIND || m_kind == AssertionKind::NEG_LOOKBEHIND;
    }

    RegexComponent ASTNodeAssertion::get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const {
        // Anchors -- directly create AssertionEdge with the anchor as the payload
        if (m_kind == AssertionKind::START || m_kind == AssertionKind::END) {
            const Anchor anchor = m_kind == AssertionKind::START ? '^' : '$';
            VertexID v_in = graph.create_vertex();
            VertexID v_out = graph.create_vertex();
            EdgeID eid = graph.create_edge(v_out, RCGEdgePayload {AssertionEdge {anchor}});
            graph.vertices[v_in].outgoing_edges.push_back(eid);
            return GraphFragment {v_in, v_out, {eid}};
        }
        if (m_kind == AssertionKind::WORD_BOUNDARY || m_kind == AssertionKind::NOT_WORD_BOUNDARY) {
            return make_word_boundary_fragment(graph, util_s, m, m_kind == AssertionKind::WORD_BOUNDARY);
        }

        const RegexComponent inner_regex = m_subpattern->get_subgraph(graph, util_s, m);

        const bool is_forward = (m_kind == AssertionKind::LOOKAHEAD || m_kind == AssertionKind::NEG_LOOKAHEAD);
        const bool is_positive = (m_kind == AssertionKind::LOOKAHEAD || m_kind == AssertionKind::LOOKBEHIND);
        const LookaroundDirection dir = is_forward ? LookaroundDirection::FORWARD : LookaroundDirection::BACKWARD;

        // Lookarounds with regular content --> create AssertionEdge with regex as payload
        if (std::holds_alternative<app_ref>(inner_regex)) {
            return make_assertion_fragment(graph, m, std::get<app_ref>(inner_regex), dir, is_positive);
        }

        // Non-regular lookarounds with non-regular inner content are not supported, since they would require universal
        // quantification. Lookarounds with regular content can be expressed as a negation of regular language -->
        // supported.
        if (!is_positive) {
            util::throw_error("ECMA regex unsupported: negative lookaround with non-regular inner content "
                              "(would require universal quantifiers)");
        }

        GraphFragment inner_frag = std::get<GraphFragment>(inner_regex);

        // A non-regular lookaround needs to be wrapped on the open side with Sigma*,
        // otherwise it becomes a strict exact match up to the boundary.
        VertexID s_in = graph.create_vertex();
        VertexID s_out = graph.create_vertex();
        EdgeID s_eid =
            graph.create_edge(s_out, RCGEdgePayload {MatchEdge {app_ref(util_s.re.mk_full_seq(nullptr), m)}});
        graph.vertices[s_in].outgoing_edges.push_back(s_eid);
        GraphFragment sigma_frag {s_in, s_out, {s_eid}};

        // Chain Sigma* on the correct side of the inner fragment, depending on the lookaround direction
        // Lookaheads --> Pattern concat Sigma*
        // Lookbehinds --> Sigma* concat pattern
        if (dir == LookaroundDirection::FORWARD) {
            inner_frag = chain_fragments(graph, inner_frag, sigma_frag);
        } else {
            inner_frag = chain_fragments(graph, sigma_frag, inner_frag);
        }

        // Wrap the resulting fragment into an AssertionEdge
        VertexID v_in = graph.create_vertex();
        VertexID v_out = graph.create_vertex();
        EdgeID eid =
            graph.create_edge(v_out, RCGEdgePayload {AssertionEdge {Lookaround {std::move(inner_frag), dir, true}}});
        graph.vertices[v_in].outgoing_edges.push_back(eid);
        return GraphFragment {v_in, v_out, {eid}};
    }

    void ASTNodeAssertion::strip_captures() {
        if (m_subpattern != nullptr) {
            m_subpattern->strip_captures();
        }
    }

    void ASTNodeAssertion::collect_backrefs(std::unordered_set<GroupID>& refs) const {
        if (m_subpattern != nullptr) {
            m_subpattern->collect_backrefs(refs);
        }
    }

    void ASTNodeAssertion::strip_unreferenced_captures(const std::unordered_set<GroupID>& referenced) {
        if (m_subpattern != nullptr) {
            m_subpattern->strip_unreferenced_captures(referenced);
        }
    }

    ASTNodeRef ASTNodeAssertion::clone() const {
        return std::make_unique<ASTNodeAssertion>(m_kind, m_subpattern ? m_subpattern->clone() : nullptr);
    }

    GraphFragment ASTNodeAssertion::make_assertion_fragment(RegexConstraintGraph& graph, ast_manager& m,
                                                            app_ref assert_regex, const LookaroundDirection dir,
                                                            const bool is_positive) {
        const VertexID v_in = graph.create_vertex();
        const VertexID v_out = graph.create_vertex();
        EdgeID eid = graph.create_edge(
            v_out, RCGEdgePayload {AssertionEdge {Lookaround {std::move(assert_regex), dir, is_positive}}});
        graph.vertices[v_in].outgoing_edges.push_back(eid);
        return GraphFragment {v_in, v_out, {eid}};
    }

    GraphFragment ASTNodeAssertion::make_word_boundary_fragment(RegexConstraintGraph& graph, seq_util& util_s,
                                                                ast_manager& m, const bool is_word_boundary) {
        //  A word boundary matches at a position where one side is a word char (\w) and the other is not
        //  (21.2.2.6 Assertion, IsWordChar). This is modelled as two branches in alternation:
        //    branch1: lookbehind(\w) AND lookahead(\W) -- or the reverse for '\B'
        //    branch2: lookbehind(\W) AND lookahead(\w) -- or the reverse for '\B'
        //  Each branch is built as a chain of two assertion fragments.
        const app_ref word_characters = {util_s.re.mk_word_char(), m};

        const GraphFragment b1_lookbehind =
            make_assertion_fragment(graph, m, word_characters, LookaroundDirection::BACKWARD, true);
        const GraphFragment b1_lookahead =
            make_assertion_fragment(graph, m, word_characters, LookaroundDirection::FORWARD, !is_word_boundary);

        const GraphFragment b2_lookbehind =
            make_assertion_fragment(graph, m, word_characters, LookaroundDirection::BACKWARD, false);
        const GraphFragment b2_lookahead =
            make_assertion_fragment(graph, m, word_characters, LookaroundDirection::FORWARD, is_word_boundary);

        const GraphFragment branch1 = chain_fragments(graph, b1_lookbehind, b1_lookahead);
        const GraphFragment branch2 = chain_fragments(graph, b2_lookbehind, b2_lookahead);
        return alternate_fragments(graph, branch1, branch2);
    }

    // ---------------- Quantified atom ----------------

    static std::string quantifier_to_string(const Quantifier& quantifier) {
        std::string res = "{" + std::to_string(quantifier.min) + ",";
        res += quantifier.max == UNBOUNDED ? "inf" : std::to_string(quantifier.max);
        res += "}";
        if (!quantifier.greedy) {
            res += " lazy";
        }
        return res;
    }

    ASTNodeQuantified::ASTNodeQuantified(const Quantifier quantifier, ASTNodeRef atom)
        : m_quantifier(quantifier),
          m_atom(std::move(atom)) {
        SASSERT(m_quantifier.min <= m_quantifier.max);
    }

    uint64_t ASTNodeQuantified::print_dot(std::ostream& out, uint64_t& node_count) const {
        const uint64_t id = ++node_count;
        out << "  node" << id << " [label=\"QUANTIFIER " << quantifier_to_string(m_quantifier) << "\"];\n";
        const uint64_t child_id = m_atom->print_dot(out, node_count);
        out << "  node" << id << " -> node" << child_id << ";\n";
        return id;
    }

    zstring ASTNodeQuantified::serialize() const {
        return zstring("(QUANT ") + zstring(quantifier_to_string(m_quantifier).c_str()) + zstring(" ") +
               m_atom->serialize() + zstring(")");
    }

    const Quantifier& ASTNodeQuantified::get_quantifier() const {
        return m_quantifier;
    }

    void ASTNodeQuantified::strip_captures() {
        m_atom->strip_captures();
    }

    ASTNodeRef ASTNodeQuantified::clone() const {
        return std::make_unique<ASTNodeQuantified>(m_quantifier, m_atom->clone());
    }

    ASTNodeRef ASTNodeQuantified::unroll() const {
        auto disj = std::make_unique<ASTNodeDisjunction>();

        if (m_quantifier.min == 0) {
            disj->add_alternative(std::make_unique<ASTNodeAlternative>());
        }

        // Create chains with (min, min+1, ..., max) subtrees and alternate them all.
        const uint64_t start = std::max<uint64_t>(1, m_quantifier.min);
        for (uint64_t k = start; k <= m_quantifier.max; ++k) {
            auto alt = std::make_unique<ASTNodeAlternative>();
            for (uint64_t i = 0; i < k; ++i) {
                alt->add_term(m_atom->clone());
            }
            disj->add_alternative(std::move(alt));
        }

        return disj;
    }

    RegexComponent ASTNodeQuantified::get_subgraph(RegexConstraintGraph& graph, seq_util& util_s,
                                                   ast_manager& m) const {
        const RegexComponent child_subgraph = m_atom->get_subgraph(graph, util_s, m);
        const uint64_t min = m_quantifier.min;
        const uint64_t max = m_quantifier.max;

        if (std::holds_alternative<GraphFragment>(child_subgraph)) {
            // Nonregular fragments under unbounded quantifiers -- unsupported.
            if (max == UNBOUNDED) {
                util::throw_error("ECMA regex unsupported: non-regular constructs under an unbounded quantifier");
            }
            // Nonregular fragments under bounded quantifier -- statically unroll the AST and convert to graph fragment.
            return unroll()->get_subgraph(graph, util_s, m);
        }

        // Regular subregex --> directly create the corresponding regular expression
        SASSERT(std::holds_alternative<app_ref>(child_subgraph));
        const app_ref child_expr = std::get<app_ref>(child_subgraph);
        app* quant = nullptr;

        if (max == UNBOUNDED) {
            if (min == 0) {
                quant = util_s.re.mk_star(child_expr);
            } else if (min == 1) {
                quant = util_s.re.mk_plus(child_expr);
            } else {
                // Concatenation `min` times followed by kleene star
                quant = child_expr;
                for (uint64_t i = 1; i < min; i++) {
                    quant = util_s.re.mk_concat(quant, child_expr);
                }
                quant = util_s.re.mk_concat(quant, util_s.re.mk_star(child_expr));
            }
        } else {
            // For some reason, using mk_loop and mk_power directly leads to unsoudness of the solver, although the
            // semantics should be the same as concatenation.
            if (min == max) {
                if (min == 0) {
                    quant = util_s.re.mk_epsilon(util_s.mk_string_sort());
                } else {
                    // A concrete number of concatenations
                    quant = child_expr;
                    for (uint64_t i = 1; i < min; i++) {
                        quant = util_s.re.mk_concat(quant, child_expr);
                    }
                }
            } else {
                // Range [min, max]:
                // Obligatory part -- at least `min` times
                app* at_least_min = util_s.re.mk_epsilon(util_s.mk_string_sort());
                if (min > 0) {
                    at_least_min = child_expr;
                    for (uint64_t i = 1; i < min; i++) {
                        at_least_min = util_s.re.mk_concat(at_least_min, child_expr);
                    }
                }

                // Optional pattern -- union with epsilon
                app* eps = util_s.re.mk_epsilon(util_s.mk_string_sort());
                app* re_optional = util_s.re.mk_union(eps, child_expr);

                // Chain the optional pattern `max` - `min` times
                app* up_to_max = re_optional;
                for (uint64_t i = 1; i < (max - min); i++) {
                    up_to_max = util_s.re.mk_concat(up_to_max, re_optional);
                }

                quant = util_s.re.mk_concat(at_least_min, up_to_max);
            }
        }

        SASSERT(quant != nullptr);
        return app_ref(quant, m);
    }

    void ASTNodeQuantified::collect_backrefs(std::unordered_set<GroupID>& refs) const {
        m_atom->collect_backrefs(refs);
    }

    void ASTNodeQuantified::strip_unreferenced_captures(const std::unordered_set<GroupID>& referenced) {
        m_atom->strip_unreferenced_captures(referenced);
    }

    // ---------------- Character ----------------

    ASTNodeCharacter::ASTNodeCharacter(const Z3Char ch)
        : m_char(ch) { }

    uint64_t ASTNodeCharacter::print_dot(std::ostream& out, uint64_t& node_count) const {
        const uint64_t id = ++node_count;
        std::string label = char_to_string(m_char);
        std::erase(label, '"');
        std::erase(label, '\\');
        out << "  node" << id << " [label=\"LITERAL (" << label << ")\"];\n";
        return id;
    }

    zstring ASTNodeCharacter::serialize() const {
        return zstring("(LIT ") + char_to_zstring(m_char) + zstring(")");
    }

    Z3Char ASTNodeCharacter::get_char() const {
        return m_char;
    }

    RegexComponent ASTNodeCharacter::get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const {
        check_representable_char(m_char, util_s);
        return app_ref(util_s.re.mk_to_re(util_s.str.mk_string(zstring(m_char))), m);
    }

    ASTNodeRef ASTNodeCharacter::clone() const {
        return std::make_unique<ASTNodeCharacter>(*this);
    }

    // ---------------- Dot ----------------

    uint64_t ASTNodeDot::print_dot(std::ostream& out, uint64_t& node_count) const {
        const uint64_t id = ++node_count;
        out << "  node" << id << " [label=\"DOT\"];\n";
        return id;
    }

    zstring ASTNodeDot::serialize() const {
        return zstring("(DOT)");
    }

    RegexComponent ASTNodeDot::get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const {
        // DotAll is false (the `s` flag is not supported) --> all characters except LineTerminator (21.2.2.8 Atom)
        app* line_terminators = util_s.re.mk_to_re(util_s.str.mk_string(zstring(CH_LF)));
        for (const Z3Char ch : {CH_CR, CH_LS, CH_PS}) {
            line_terminators = util_s.re.mk_union(line_terminators, util_s.re.mk_to_re(util_s.str.mk_string(zstring(ch))));
        }
        return app_ref(mk_char_complement(util_s, line_terminators), m);
    }

    ASTNodeRef ASTNodeDot::clone() const {
        return std::make_unique<ASTNodeDot>(*this);
    }

    // ---------------- Backreference ----------------

    ASTNodeBackreference::ASTNodeBackreference(const GroupID group_id, const zstring_view group_name)
        : m_group_id(group_id),
          m_group_name(group_name) { }

    uint64_t ASTNodeBackreference::print_dot(std::ostream& out, uint64_t& node_count) const {
        const uint64_t id = ++node_count;
        out << "  node" << id << " [label=\"BACKREF " << m_group_id << "\"];\n";
        return id;
    }

    zstring ASTNodeBackreference::serialize() const {
        zstring res = zstring("(BACKREF ") + zstring(std::to_string(m_group_id).c_str());
        if (m_group_name.length() > 0) {
            res += zstring(" <") + m_group_name.to_zstring() + zstring(">");
        }
        return res + zstring(")");
    }

    GroupID ASTNodeBackreference::get_group_id() const {
        return m_group_id;
    }

    void ASTNodeBackreference::set_group_id(const GroupID group_id) {
        m_group_id = group_id;
    }

    zstring_view ASTNodeBackreference::get_group_name() const {
        return m_group_name;
    }

    RegexComponent ASTNodeBackreference::get_subgraph(RegexConstraintGraph& graph, seq_util& util_s,
                                                      ast_manager& m) const {
        const VertexID vin = graph.create_vertex();
        const VertexID vout = graph.create_vertex();
        const EdgeID backref_eid = graph.create_edge(vout, {BackrefEdge {m_group_id}});
        graph.vertices[vin].outgoing_edges.push_back(backref_eid);
        return GraphFragment {vin, vout, {backref_eid}};
    }

    ASTNodeRef ASTNodeBackreference::clone() const {
        return std::make_unique<ASTNodeBackreference>(*this);
    }

    void ASTNodeBackreference::collect_backrefs(std::unordered_set<GroupID>& refs) const {
        refs.insert(m_group_id);
    }

    // ---------------- Group ----------------

    ASTNodeGroup::ASTNodeGroup(ASTNodeRef child)
        : m_capturing(false),
          m_child(std::move(child)) { }

    ASTNodeGroup::ASTNodeGroup(const GroupID gid, ASTNodeRef child, const zstring_view name)
        : m_capturing(true),
          m_gid(gid),
          m_name(name),
          m_child(std::move(child)) { }

    uint64_t ASTNodeGroup::print_dot(std::ostream& out, uint64_t& node_count) const {
        const uint64_t id = ++node_count;
        std::string label = "GROUP";
        if (!m_capturing) {
            label += " (?:)";
        } else {
            label += " #" + std::to_string(m_gid);
        }

        out << "  node" << id << " [label=\"" << label << "\"];\n";
        const uint64_t child_id = m_child->print_dot(out, node_count);
        out << "  node" << id << " -> node" << child_id << ";\n";
        return id;
    }

    zstring ASTNodeGroup::serialize() const {
        zstring label;
        if (!m_capturing) {
            label = zstring("GROUP-NONCAP");
        } else {
            label = zstring("GROUP #") + zstring(std::to_string(m_gid).c_str());
            if (m_name.length() > 0) {
                label += zstring(" <") + m_name.to_zstring() + zstring(">");
            }
        }
        return zstring("(") + label + zstring(" ") + m_child->serialize() + zstring(")");
    }

    bool ASTNodeGroup::is_capturing() const {
        return m_capturing;
    }

    GroupID ASTNodeGroup::get_id() const {
        return m_gid;
    }

    zstring_view ASTNodeGroup::get_name() const {
        return m_name;
    }

    RegexComponent ASTNodeGroup::get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const {
        RegexComponent subregex = m_child->get_subgraph(graph, util_s, m);
        // Noncapturing groups --> no semantic meaning for the subregex
        if (!m_capturing) {
            return subregex;
        }

        GraphFragment fragment;
        if (std::holds_alternative<app_ref>(subregex)) {
            // Regular subregex in group -- create a MatchEdge and create a fragment
            const VertexID vin = graph.create_vertex();
            const VertexID vout = graph.create_vertex();
            const EdgeID eid = graph.create_edge(vout, RCGEdgePayload {MatchEdge {std::get<app_ref>(subregex)}});
            graph.vertices[vin].outgoing_edges.push_back(eid);
            fragment = GraphFragment {vin, vout, {eid}};
        } else {
            // Nonregular subregex in group -- take the fragment
            fragment = std::get<GraphFragment>(subregex);
        }

        // Mark all the outgoing edges from the v_in of the fragment as group starts
        for (const EdgeID eid : graph.vertices[fragment.v_in].outgoing_edges) {
            graph.group_starts[eid].push_back(m_gid);
        }

        // Mark all the incoming edges to the fragments' v_out as group ends
        for (const EdgeID eid : fragment.edges_pointing_to_vout) {
            graph.group_ends[eid].push_back(m_gid);
        }

        return fragment;
    }

    void ASTNodeGroup::strip_captures() {
        m_capturing = false;
        m_child->strip_captures();
    }

    void ASTNodeGroup::collect_backrefs(std::unordered_set<GroupID>& refs) const {
        m_child->collect_backrefs(refs);
    }

    void ASTNodeGroup::strip_unreferenced_captures(const std::unordered_set<GroupID>& referenced) {
        if (m_capturing && !referenced.contains(m_gid)) {
            m_capturing = false;
        }
        m_child->strip_unreferenced_captures(referenced);
    }

    ASTNodeRef ASTNodeGroup::clone() const {
        if (!m_capturing) {
            return std::make_unique<ASTNodeGroup>(m_child->clone());
        }
        return std::make_unique<ASTNodeGroup>(m_gid, m_child->clone(), m_name);
    }

    // ---------------- Character class ----------------

    static char class_escape_letter(const ClassEscapeKind kind) {
        switch (kind) {
            case ClassEscapeKind::DIGIT:
                return 'd';
            case ClassEscapeKind::NOT_DIGIT:
                return 'D';
            case ClassEscapeKind::SPACE:
                return 's';
            case ClassEscapeKind::NOT_SPACE:
                return 'S';
            case ClassEscapeKind::WORD:
                return 'w';
            case ClassEscapeKind::NOT_WORD:
                return 'W';
            case ClassEscapeKind::PROPERTY:
                return 'p';
            case ClassEscapeKind::NOT_PROPERTY:
                return 'P';
        }
        return '?';
    }

    // E.g. 'd' for \d, or 'p' {gc=Lu} for \p{gc=Lu}
    static std::string class_escape_to_string(const CharClassEscape& escape) {
        std::string res = std::string("'") + class_escape_letter(escape.kind) + "'";
        if (escape.kind == ClassEscapeKind::PROPERTY || escape.kind == ClassEscapeKind::NOT_PROPERTY) {
            res += " {";
            if (escape.property_name.length() > 0) {
                res += escape.property_name.to_zstring().encode() + "=";
            }
            res += escape.property_value.to_zstring().encode() + "}";
        }
        return res;
    }

    // The set of characters of a character class escape (21.2.2.12 CharacterClassEscape)
    static app* mk_class_escape_regex(const CharClassEscape& escape, seq_util& util_s) {
        switch (escape.kind) {
            case ClassEscapeKind::DIGIT:
            case ClassEscapeKind::NOT_DIGIT: {
                app* re_digit = util_s.re.mk_range(util_s.str.mk_string("0"), util_s.str.mk_string("9"));
                return escape.kind == ClassEscapeKind::DIGIT ? re_digit : mk_char_complement(util_s, re_digit);
            }
            case ClassEscapeKind::SPACE:
            case ClassEscapeKind::NOT_SPACE: {
                sort* re_sort = util_s.re.mk_re(util_s.mk_string_sort());
                app* re_whitespace = nullptr;
                for (const ClassRange& range : WHITESPACE_RANGES) {
                    app* re_range = util_s.re.mk_range(re_sort, range.lo, range.hi);
                    re_whitespace = re_whitespace == nullptr ? re_range : util_s.re.mk_union(re_whitespace, re_range);
                }
                return escape.kind == ClassEscapeKind::SPACE ? re_whitespace
                                                             : mk_char_complement(util_s, re_whitespace);
            }
            case ClassEscapeKind::WORD:
            case ClassEscapeKind::NOT_WORD: {
                // IgnoreCase is false --> WordCharacters() are exactly [a-zA-Z0-9_] (21.2.2.6.1)
                app* re_word = util_s.re.mk_word_char();
                return escape.kind == ClassEscapeKind::WORD ? re_word : mk_char_complement(util_s, re_word);
            }
            case ClassEscapeKind::PROPERTY:
            case ClassEscapeKind::NOT_PROPERTY:
                util::throw_error("ECMA regex unsupported: Unicode property escapes (\\p{...}, \\P{...})");
                break;
        }
        return nullptr;
    }

    ASTNodeCharClass::ASTNodeCharClass(const bool negated)
        : m_is_negated(negated) { }

    uint64_t ASTNodeCharClass::print_dot(std::ostream& out, uint64_t& node_count) const {
        const uint64_t id = ++node_count;
        std::string label = "CLASS [";
        if (m_is_negated) {
            label += "^";
        }

        for (const ClassItem& item : m_items) {
            if (std::holds_alternative<CharClassEscape>(item)) {
                label += " \\" + class_escape_to_string(std::get<CharClassEscape>(item));
            } else {
                const ClassRange& range = std::get<ClassRange>(item);
                label += " " + char_to_string(range.lo);
                if (range.lo != range.hi) {
                    label += "-" + char_to_string(range.hi);
                }
            }
        }
        label += " ]";
        std::erase(label, '"');
        std::erase(label, '\\');

        out << "  node" << id << " [label=\"" << label << "\"];\n";
        return id;
    }

    zstring ASTNodeCharClass::serialize() const {
        zstring res("(CLASS");
        if (m_is_negated) {
            res += zstring(" ^");
        }
        for (const ClassItem& item : m_items) {
            if (std::holds_alternative<CharClassEscape>(item)) {
                res += zstring(" (CHAR_CLASS ") +
                       zstring(class_escape_to_string(std::get<CharClassEscape>(item)).c_str()) + zstring(")");
            } else {
                const ClassRange& range = std::get<ClassRange>(item);
                if (range.lo == range.hi) {
                    res += zstring(" (LIT ") + char_to_zstring(range.lo) + zstring(")");
                } else {
                    res += zstring(" (RANGE ") + char_to_zstring(range.lo) + zstring(" ") + char_to_zstring(range.hi) +
                           zstring(")");
                }
            }
        }
        res += zstring(")");
        return res;
    }

    void ASTNodeCharClass::add_item(ClassItem item) {
        if (std::holds_alternative<ClassRange>(item) && std::get<ClassRange>(item).lo > std::get<ClassRange>(item).hi) {
            util::throw_error("ECMA regex syntax error: range out of order in character class");
        }
        m_items.push_back(std::move(item));
    }

    bool ASTNodeCharClass::is_negated() const {
        return m_is_negated;
    }

    const std::vector<ClassItem>& ASTNodeCharClass::get_items() const {
        return m_items;
    }

    RegexComponent ASTNodeCharClass::get_subgraph(RegexConstraintGraph& graph, seq_util& util_s, ast_manager& m) const {
        sort* re_sort = util_s.re.mk_re(util_s.mk_string_sort());
        const Z3Char max_char = util_s.max_char();

        // Unite all the class items (21.2.2.13 CharacterClass -- the class is the union of its ClassRanges)
        app* class_re = nullptr;
        for (const ClassItem& item : m_items) {
            app* item_re = nullptr;
            if (std::holds_alternative<CharClassEscape>(item)) {
                item_re = mk_class_escape_regex(std::get<CharClassEscape>(item), util_s);
            } else {
                const ClassRange& range = std::get<ClassRange>(item);
                // Characters above the maximal character of the string encoding cannot occur in any string
                if (range.lo > max_char) {
                    continue;
                }
                item_re = util_s.re.mk_range(re_sort, range.lo, std::min(range.hi, max_char));
            }
            class_re = class_re == nullptr ? item_re : util_s.re.mk_union(class_re, item_re);
        }

        // An empty class [] matches no character, [^] matches all characters (21.2.2.14 ClassRanges :: [empty])
        if (class_re == nullptr) {
            class_re = m_is_negated ? util_s.re.mk_full_char(nullptr) : util_s.re.mk_empty(re_sort);
        } else if (m_is_negated) {
            // Negated character class -- Sigma - class_re (CharacterSetMatcher with invert = true, 21.2.2.8.1)
            class_re = mk_char_complement(util_s, class_re);
        }

        return app_ref(class_re, m);
    }

    ASTNodeRef ASTNodeCharClass::clone() const {
        return std::make_unique<ASTNodeCharClass>(*this);
    }

    // =============== ECMA REGEX PARSER ===============
    //
    // Section and production names refer to ECMA-262 2020 (11th edition), 21.2.1 Patterns, with the grammar parameters
    // fixed to [+U, +N].

    // ---------------- Reading the pattern ----------------

    bool ECMAParser::at_end() const {
        return m_pos >= m_pattern.length();
    }

    Z3Char ECMAParser::peek(const std::size_t offset) const {
        // A value that is not a code point is returned behind the end of the pattern, so it never matches any character
        if (m_pos + offset >= m_pattern.length()) {
            return std::numeric_limits<Z3Char>::max();
        }
        return m_pattern[m_pos + offset];
    }

    Z3Char ECMAParser::advance() {
        SASSERT(!at_end());
        return m_pattern[m_pos++];
    }

    bool ECMAParser::eat(const Z3Char ch) {
        if (!at_end() && peek() == ch) {
            m_pos++;
            return true;
        }
        return false;
    }

    void ECMAParser::expect(const Z3Char ch, const char* what) {
        if (!eat(ch)) {
            syntax_error(std::string("expected ") + what);
        }
    }

    void ECMAParser::syntax_error(const std::string& message, const std::size_t position) const {
        const std::string full_message =
            "ECMA regex syntax error at position " + std::to_string(position) + ": " + message;
        util::throw_error(full_message);
    }

    void ECMAParser::syntax_error(const std::string& message) const {
        syntax_error(message, m_pos);
    }

    // ---------------- Pattern, Disjunction, Alternative, Term ----------------

    ASTNodeRef ECMAParser::parse() {
        m_pos = 0;
        m_num_capturing_groups = 0;
        m_named_groups.clear();
        m_pending_backrefs.clear();

        // Pattern :: Disjunction
        ASTNodeRef ast = parse_disjunction();
        if (!at_end()) {
            // Disjunction stops only at the end of the pattern or at ')'
            SASSERT(peek() == ')');
            syntax_error("unmatched ')'");
        }
        resolve_backreferences();

        if (debug_mode) {
            namespace fs = std::filesystem;
            fs::path project_root = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
            fs::path dot_file = project_root / "output.dot";
            std::ofstream out(dot_file);
            if (out.is_open()) {
                uint64_t node_count = 0;
                out << "digraph G {\n";
                ast->print_dot(out, node_count);
                out << "}" << std::endl;
                out.close();
            }
        }

        return ast;
    }

    GroupID ECMAParser::num_capturing_groups() const {
        return m_num_capturing_groups;
    }

    ASTNodeRef ECMAParser::parse_disjunction() {
        // Disjunction :: Alternative | Alternative '|' Disjunction
        ASTNodeRef first = parse_alternative();
        if (peek() != '|') {
            return first;
        }

        auto disjunction = std::make_unique<ASTNodeDisjunction>();
        disjunction->add_alternative(std::move(first));
        while (eat('|')) {
            disjunction->add_alternative(parse_alternative());
        }
        return disjunction;
    }

    ASTNodeRef ECMAParser::parse_alternative() {
        // Alternative :: [empty] | Alternative Term
        auto alternative = std::make_unique<ASTNodeAlternative>();
        while (!at_end() && peek() != '|' && peek() != ')') {
            alternative->add_term(parse_term());
        }
        return alternative;
    }

    ASTNodeRef ECMAParser::parse_term() {
        // Term :: Assertion | Atom | Atom Quantifier
        //
        // An assertion cannot be quantified for [+U] (QuantifiableAssertion from B.1.4 is only for [~U]). A quantifier
        // following an assertion is therefore reported as "nothing to repeat" by parse_atom() of the next term.
        if (ASTNodeRef assertion = try_parse_assertion()) {
            return assertion;
        }

        ASTNodeRef atom = parse_atom();
        Quantifier quantifier;
        if (!try_parse_quantifier(quantifier)) {
            return atom;
        }
        return std::make_unique<ASTNodeQuantified>(quantifier, std::move(atom));
    }

    ASTNodeRef ECMAParser::try_parse_assertion() {
        // Assertion :: ^ | $ | \b | \B | (?= Disjunction ) | (?! Disjunction ) | (?<= Disjunction ) | (?<! Disjunction )
        if (eat('^')) {
            return std::make_unique<ASTNodeAssertion>(AssertionKind::START);
        }
        if (eat('$')) {
            return std::make_unique<ASTNodeAssertion>(AssertionKind::END);
        }
        if (peek() == '\\' && (peek(1) == 'b' || peek(1) == 'B')) {
            const bool is_word_boundary = peek(1) == 'b';
            m_pos += 2;
            return std::make_unique<ASTNodeAssertion>(is_word_boundary ? AssertionKind::WORD_BOUNDARY
                                                                       : AssertionKind::NOT_WORD_BOUNDARY);
        }
        if (peek() != '(' || peek(1) != '?') {
            return nullptr;
        }

        AssertionKind kind;
        std::size_t prefix_length;
        if (peek(2) == '=' || peek(2) == '!') {
            kind = peek(2) == '=' ? AssertionKind::LOOKAHEAD : AssertionKind::NEG_LOOKAHEAD;
            prefix_length = 3;
        } else if (peek(2) == '<' && (peek(3) == '=' || peek(3) == '!')) {
            kind = peek(3) == '=' ? AssertionKind::LOOKBEHIND : AssertionKind::NEG_LOOKBEHIND;
            prefix_length = 4;
        } else {
            // a group (?: ...) or (?<name> ...)
            return nullptr;
        }

        m_pos += prefix_length;
        ASTNodeRef subpattern = parse_disjunction();
        expect(')', "')' closing the lookaround");
        return std::make_unique<ASTNodeAssertion>(kind, std::move(subpattern));
    }

    // ---------------- Quantifier ----------------

    bool ECMAParser::try_parse_quantifier(Quantifier& quantifier) {
        // Quantifier :: QuantifierPrefix | QuantifierPrefix ?
        // QuantifierPrefix :: * | + | ? | { DecimalDigits } | { DecimalDigits , } | { DecimalDigits , DecimalDigits }
        // (semantics in 21.2.2.7)
        if (eat('*')) {
            quantifier = {0, UNBOUNDED, true};
        } else if (eat('+')) {
            quantifier = {1, UNBOUNDED, true};
        } else if (eat('?')) {
            quantifier = {0, 1, true};
        } else if (peek() == '{') {
            // There is no fallback to a literal '{' for [+U] (ExtendedPatternCharacter from B.1.4 is only for [~U])
            const std::size_t start = m_pos;
            advance();
            quantifier.min = parse_decimal_digits("the minimum of the {} quantifier");
            if (eat('}')) {
                quantifier.max = quantifier.min;
            } else {
                expect(',', "',' or '}' in the {} quantifier");
                quantifier.max = peek() == '}' ? UNBOUNDED : parse_decimal_digits("the maximum of the {} quantifier");
                expect('}', "'}' closing the {} quantifier");
            }
            // Early error (21.2.1.1): the MV of the first DecimalDigits is larger than the MV of the second one
            if (quantifier.min > quantifier.max) {
                syntax_error("numbers out of order in {} quantifier", start);
            }
            quantifier.greedy = true;
        } else {
            return false;
        }

        if (eat('?')) {
            quantifier.greedy = false;
        }
        return true;
    }

    uint64_t ECMAParser::parse_decimal_digits(const char* what) {
        // DecimalDigits :: DecimalDigit | DecimalDigits DecimalDigit   (MV defined in 11.8.3)
        if (!is_decimal_digit(peek())) {
            syntax_error(std::string("expected ") + what);
        }
        uint64_t value = 0;
        while (is_decimal_digit(peek())) {
            const uint64_t digit = advance() - '0';
            // UNBOUNDED is reserved for infinity
            if (value > (UNBOUNDED - 1 - digit) / 10) {
                util::throw_error("ECMA regex unsupported: number in the {} quantifier is too large");
            }
            value = value * 10 + digit;
        }
        return value;
    }

    // ---------------- Atom ----------------

    ASTNodeRef ECMAParser::parse_atom() {
        // Atom :: PatternCharacter | . | \ AtomEscape | CharacterClass | ( GroupSpecifier Disjunction ) |
        //         (?: Disjunction )
        SASSERT(!at_end());
        switch (peek()) {
            case '.':
                advance();
                return std::make_unique<ASTNodeDot>();
            case '\\':
                advance();
                return parse_atom_escape();
            case '[':
                return parse_character_class();
            case '(':
                return parse_group();
            case '*':
            case '+':
            case '?':
            case '{':
                syntax_error("nothing to repeat");
            case ']':
            case '}':
                // SyntaxCharacter is not a PatternCharacter (Annex B allows lone ']' and '}' only for [~U])
                syntax_error(std::string("lone '") + static_cast<char>(peek()) + "' must be escaped");
            default:
                // PatternCharacter :: SourceCharacter but not SyntaxCharacter
                // ('^', '$', '|' and ')' were already handled by parse_term/parse_alternative)
                SASSERT(!is_syntax_character(peek()));
                return std::make_unique<ASTNodeCharacter>(advance());
        }
    }

    ASTNodeRef ECMAParser::parse_group() {
        // Atom :: ( GroupSpecifier Disjunction ) | (?: Disjunction )
        // GroupSpecifier :: [empty] | ? GroupName
        // (lookarounds starting with "(?" were already handled by try_parse_assertion)
        const std::size_t start = m_pos;
        advance();

        if (eat('?')) {
            if (eat(':')) {
                ASTNodeRef child = parse_disjunction();
                expect(')', "')' closing the group");
                return std::make_unique<ASTNodeGroup>(std::move(child));
            }
            if (peek() != '<') {
                syntax_error("invalid group, expected one of '(?:', '(?=', '(?!', '(?<=', '(?<!' or '(?<name>'", start);
            }
            const std::size_t name_position = m_pos;
            zstring_view name_source;
            const zstring name = parse_group_name(name_source);
            // The group number is the number of left-capturing parentheses to the left (parenIndex, 21.2.2.8)
            const GroupID gid = create_capturing_group();
            register_group_name(name, gid, name_position);
            ASTNodeRef child = parse_disjunction();
            expect(')', "')' closing the group");
            return std::make_unique<ASTNodeGroup>(gid, std::move(child), name_source);
        }

        const GroupID gid = create_capturing_group();
        ASTNodeRef child = parse_disjunction();
        expect(')', "')' closing the group");
        return std::make_unique<ASTNodeGroup>(gid, std::move(child));
    }

    // ---------------- AtomEscape, CharacterEscape ----------------

    ASTNodeRef ECMAParser::parse_atom_escape() {
        // AtomEscape :: DecimalEscape | CharacterClassEscape | CharacterEscape | k GroupName
        // (the backslash is already consumed; \b and \B were handled by try_parse_assertion)
        const std::size_t start = m_pos - 1;
        if (at_end()) {
            syntax_error("\\ at end of pattern", start);
        }

        // DecimalEscape :: NonZeroDigit DecimalDigits_opt [lookahead ∉ DecimalDigit]
        // For [+U], it is always a backreference (no legacy octal escapes from B.1.4).
        if (peek() >= '1' && peek() <= '9') {
            // CapturingGroupNumber (21.2.1.2), saturated -- a too large number is reported as an early error later
            GroupID number = 0;
            while (is_decimal_digit(peek())) {
                const GroupID digit = advance() - '0';
                number = number > (std::numeric_limits<GroupID>::max() - digit) / 10
                             ? std::numeric_limits<GroupID>::max()
                             : number * 10 + digit;
            }
            auto backref = std::make_unique<ASTNodeBackreference>(number);
            m_pending_backrefs.push_back({backref.get(), start, zstring()});
            return backref;
        }

        // [+N] k GroupName
        if (eat('k')) {
            if (peek() != '<') {
                syntax_error("invalid named reference, expected '\\k<name>'", start);
            }
            zstring_view name_source;
            zstring name = parse_group_name(name_source);
            auto backref = std::make_unique<ASTNodeBackreference>(0, name_source);
            m_pending_backrefs.push_back({backref.get(), start, std::move(name)});
            return backref;
        }

        CharClassEscape escape;
        if (try_parse_character_class_escape(escape)) {
            auto char_class = std::make_unique<ASTNodeCharClass>(false);
            char_class->add_item(std::move(escape));
            return char_class;
        }

        return std::make_unique<ASTNodeCharacter>(parse_character_escape());
    }

    Z3Char ECMAParser::parse_character_escape() {
        // CharacterEscape[U] :: ControlEscape | c ControlLetter | 0 [lookahead ∉ DecimalDigit] | HexEscapeSequence |
        //                       RegExpUnicodeEscapeSequence[?U] | IdentityEscape[?U]
        // Returns the CharacterValue (21.2.1.4). The backslash is already consumed.
        const std::size_t start = m_pos - 1;
        if (at_end()) {
            syntax_error("\\ at end of pattern", start);
        }

        const Z3Char ch = advance();
        switch (ch) {
            // ControlEscape :: one of f n r t v   (Table 54)
            case 't':
                return 0x0009;
            case 'n':
                return 0x000A;
            case 'v':
                return 0x000B;
            case 'f':
                return 0x000C;
            case 'r':
                return 0x000D;
            case 'c':
                // c ControlLetter -- the remainder of dividing the code point by 32
                if (!is_ascii_letter(peek())) {
                    syntax_error("invalid control escape, expected '\\c' followed by an ASCII letter", start);
                }
                return advance() % 32;
            case '0':
                // 0 [lookahead ∉ DecimalDigit] -- for [+U], legacy octal escapes (B.1.4) are not allowed
                if (is_decimal_digit(peek())) {
                    syntax_error("invalid decimal escape (octal escapes are not allowed)", start);
                }
                return 0x0000;
            case 'x': {
                // HexEscapeSequence :: x HexDigit HexDigit   (11.8.4)
                if (!is_hex_digit(peek()) || !is_hex_digit(peek(1))) {
                    syntax_error("invalid hexadecimal escape, expected '\\x' followed by two hex digits", start);
                }
                const Z3Char high = hex_digit_value(advance());
                return high * 16 + hex_digit_value(advance());
            }
            case 'u':
                return parse_regexp_unicode_escape_sequence();
            default:
                // IdentityEscape[+U] :: [+U] SyntaxCharacter | [+U] /
                if (is_syntax_character(ch) || ch == '/') {
                    return ch;
                }
                syntax_error("invalid escape", start);
        }
    }

    Z3Char ECMAParser::parse_regexp_unicode_escape_sequence() {
        // RegExpUnicodeEscapeSequence[+U] :: u LeadSurrogate \u TrailSurrogate | u LeadSurrogate | u TrailSurrogate |
        //                                    u NonSurrogate | u{ CodePoint }
        // The "\u" is already consumed.
        const std::size_t start = m_pos - 2;

        if (eat('{')) {
            // CodePoint :: HexDigits but only if MV of HexDigits ≤ 0x10FFFF   (11.8.6)
            if (!is_hex_digit(peek())) {
                syntax_error("invalid unicode escape, expected hex digits after '\\u{'", start);
            }
            uint32_t value = 0;
            while (is_hex_digit(peek())) {
                value = value * 16 + hex_digit_value(advance());
                if (value > MAX_CODE_POINT) {
                    syntax_error("invalid unicode escape, code point is larger than 0x10FFFF", start);
                }
            }
            expect('}', "'}' closing the unicode escape");
            return value;
        }

        Z3Char lead = 0;
        if (!try_parse_hex4_digits(0, lead)) {
            syntax_error("invalid unicode escape, expected '\\u' followed by four hex digits or '\\u{...}'", start);
        }
        m_pos += 4;

        // A lead surrogate followed by an escaped trail surrogate forms a single code point (UTF16DecodeSurrogatePair,
        // 10.1.3). Each \u TrailSurrogate is associated with the nearest preceding u LeadSurrogate.
        Z3Char trail = 0;
        if (is_lead_surrogate(lead) && peek() == '\\' && peek(1) == 'u' && try_parse_hex4_digits(2, trail) &&
            is_trail_surrogate(trail)) {
            m_pos += 6;
            return (lead - 0xD800) * 0x400 + (trail - 0xDC00) + 0x10000;
        }
        return lead;
    }

    bool ECMAParser::try_parse_hex4_digits(const std::size_t offset, Z3Char& value) const {
        // Hex4Digits :: HexDigit HexDigit HexDigit HexDigit   (11.8.4)
        value = 0;
        for (std::size_t i = 0; i < 4; i++) {
            if (!is_hex_digit(peek(offset + i))) {
                return false;
            }
            value = value * 16 + hex_digit_value(peek(offset + i));
        }
        return true;
    }

    // ---------------- CharacterClassEscape ----------------

    bool ECMAParser::try_parse_character_class_escape(CharClassEscape& escape) {
        // CharacterClassEscape[U] :: d | D | s | S | w | W | [+U] p{ UnicodePropertyValueExpression } |
        //                            [+U] P{ UnicodePropertyValueExpression }
        switch (peek()) {
            case 'd':
                escape.kind = ClassEscapeKind::DIGIT;
                break;
            case 'D':
                escape.kind = ClassEscapeKind::NOT_DIGIT;
                break;
            case 's':
                escape.kind = ClassEscapeKind::SPACE;
                break;
            case 'S':
                escape.kind = ClassEscapeKind::NOT_SPACE;
                break;
            case 'w':
                escape.kind = ClassEscapeKind::WORD;
                break;
            case 'W':
                escape.kind = ClassEscapeKind::NOT_WORD;
                break;
            case 'p':
            case 'P':
                escape.kind = peek() == 'p' ? ClassEscapeKind::PROPERTY : ClassEscapeKind::NOT_PROPERTY;
                advance();
                parse_unicode_property_value_expression(escape);
                return true;
            default:
                return false;
        }
        advance();
        return true;
    }

    void ECMAParser::parse_unicode_property_value_expression(CharClassEscape& escape) {
        // UnicodePropertyValueExpression :: UnicodePropertyName = UnicodePropertyValue |
        //                                   LoneUnicodePropertyNameOrValue
        // UnicodePropertyNameCharacter :: ControlLetter | _
        // UnicodePropertyValueCharacter :: UnicodePropertyNameCharacter | DecimalDigit
        const std::size_t start = m_pos - 2;
        expect('{', "'{' after '\\p' or '\\P'");

        auto is_name_char = [](const Z3Char ch) {
            return is_ascii_letter(ch) || ch == '_';
        };
        // The name and value are views into the pattern
        auto read_value_chars = [&]() {
            const std::size_t begin = m_pos;
            while (is_name_char(peek()) || is_decimal_digit(peek())) {
                advance();
            }
            return zstring_view(m_pattern + static_cast<uint32_t>(begin), static_cast<uint32_t>(m_pos - begin));
        };
        auto is_property_name = [&](const zstring_view chars) {
            for (uint32_t i = 0; i < chars.length(); i++) {
                if (!is_name_char(chars[i])) {
                    return false;
                }
            }
            return chars.length() > 0;
        };

        const zstring_view first = read_value_chars();
        if (eat('=')) {
            if (!is_property_name(first)) {
                syntax_error("invalid Unicode property name", start);
            }
            const zstring_view value = read_value_chars();
            if (value.length() == 0) {
                syntax_error("invalid Unicode property value", start);
            }
            expect('}', "'}' closing the Unicode property escape");

            // Early errors (21.2.1.1): the name must be listed in Table 55, the value in Table 57 or Table 58
            bool is_known_value;
            if (contains_name(PROPERTY_GENERAL_CATEGORY, first)) {
                is_known_value = contains_name(GENERAL_CATEGORY_VALUES, value);
            } else if (contains_name(PROPERTY_SCRIPT, first)) {
                is_known_value = contains_name(SCRIPT_VALUES, value);
            } else {
                syntax_error("unknown Unicode property name '" + first.to_zstring().encode() + "'", start);
                return;
            }
            if (!is_known_value) {
                syntax_error("unknown value '" + value.to_zstring().encode() + "' of the Unicode property '" +
                                 first.to_zstring().encode() + "'",
                             start);
            }
            escape.property_name = first;
            escape.property_value = value;
            return;
        }

        if (first.length() == 0) {
            syntax_error("invalid Unicode property escape", start);
        }
        expect('}', "'}' closing the Unicode property escape");
        // Early error (21.2.1.1): a lone name must be a General_Category value (Table 57) or a binary property
        // (Table 56)
        if (!contains_name(GENERAL_CATEGORY_VALUES, first) && !contains_name(BINARY_PROPERTIES, first)) {
            syntax_error("unknown Unicode property '" + first.to_zstring().encode() + "'", start);
        }
        escape.property_value = first;
    }

    // ---------------- GroupName ----------------

    zstring ECMAParser::parse_group_name(zstring_view& source_text) {
        // GroupName[U] :: < RegExpIdentifierName[?U] >
        // RegExpIdentifierName[U] :: RegExpIdentifierStart[?U] | RegExpIdentifierName[?U] RegExpIdentifierPart[?U]
        // Returns the StringValue of the name (21.2.1.6), i.e., with the escape sequences replaced. The source text of
        // RegExpIdentifierName (a view into the pattern) is stored to @p source_text.
        expect('<', "'<' starting the group name");
        if (peek() == '>') {
            syntax_error("empty group name");
        }

        const std::size_t begin = m_pos;
        zstring name(parse_regexp_identifier_char(true));
        while (peek() != '>') {
            if (at_end()) {
                syntax_error("unterminated group name, expected '>'");
            }
            name += zstring(parse_regexp_identifier_char(false));
        }
        source_text = zstring_view(m_pattern + static_cast<uint32_t>(begin), static_cast<uint32_t>(m_pos - begin));
        advance();  // '>'
        return name;
    }

    Z3Char ECMAParser::parse_regexp_identifier_char(const bool is_start) {
        // RegExpIdentifierStart[U] :: UnicodeIDStart | $ | _ | \ RegExpUnicodeEscapeSequence[+U]
        // RegExpIdentifierPart[U] :: UnicodeIDContinue | $ | \ RegExpUnicodeEscapeSequence[+U] | <ZWNJ> | <ZWJ>
        //
        // Only the ASCII part of UnicodeIDStart/UnicodeIDContinue is checked, non-ASCII characters are accepted.
        // The early errors (21.2.1.1) require the same restrictions on the escaped characters.
        const std::size_t start = m_pos;
        if (at_end()) {
            syntax_error("unterminated group name, expected '>'");
        }
        Z3Char ch;
        if (eat('\\')) {
            if (!eat('u')) {
                syntax_error("invalid escape in group name, only '\\u' escapes are allowed", start);
            }
            ch = parse_regexp_unicode_escape_sequence();
        } else {
            ch = advance();
        }

        bool is_valid = ch == '$' || ch == '_' || is_ascii_letter(ch) || ch >= 0x80;
        if (!is_start) {
            is_valid = is_valid || is_decimal_digit(ch) || ch == CH_ZWNJ || ch == CH_ZWJ;
        }
        if (!is_valid) {
            syntax_error("invalid character in group name", start);
        }
        return ch;
    }

    // ---------------- CharacterClass ----------------

    ASTNodeRef ECMAParser::parse_character_class() {
        // CharacterClass[U] :: [ [lookahead ≠ ^] ClassRanges[?U] ] | [ ^ ClassRanges[?U] ]
        advance();
        auto char_class = std::make_unique<ASTNodeCharClass>(eat('^'));
        parse_class_ranges(*char_class);
        expect(']', "']' closing the character class");
        return char_class;
    }

    void ECMAParser::parse_class_ranges(ASTNodeCharClass& char_class) {
        // ClassRanges :: [empty] | NonemptyClassRanges
        // NonemptyClassRanges :: ClassAtom | ClassAtom NonemptyClassRangesNoDash | ClassAtom - ClassAtom ClassRanges
        // NonemptyClassRangesNoDash :: ClassAtom | ClassAtomNoDash NonemptyClassRangesNoDash |
        //                              ClassAtomNoDash - ClassAtom ClassRanges
        //
        // The productions are equivalent to the loop below: a '-' between two atoms forms a range, otherwise it is
        // a literal '-' (at the beginning, at the end, or right after a range).
        while (!at_end() && peek() != ']') {
            const std::size_t start = m_pos;
            ClassAtom first = parse_class_atom();

            if (peek() != '-' || peek(1) == ']' || m_pos + 1 >= m_pattern.length()) {
                if (first.is_class) {
                    char_class.add_item(std::move(first.escape));
                } else {
                    char_class.add_item(ClassRange {first.value, first.value});
                }
                continue;
            }

            advance();  // '-'
            ClassAtom second = parse_class_atom();
            // Early errors (21.2.1.1, IsCharacterClass in 21.2.1.3, CharacterValue in 21.2.1.4)
            if (first.is_class || second.is_class) {
                syntax_error("invalid character class range, a class escape cannot be a bound of a range", start);
            }
            if (first.value > second.value) {
                syntax_error("range out of order in character class", start);
            }
            char_class.add_item(ClassRange {first.value, second.value});
        }
    }

    ECMAParser::ClassAtom ECMAParser::parse_class_atom() {
        // ClassAtom[U] :: - | ClassAtomNoDash[?U]
        // ClassAtomNoDash[U] :: SourceCharacter but not one of \ or ] or - | \ ClassEscape[?U]
        SASSERT(!at_end() && peek() != ']');
        if (eat('\\')) {
            return parse_class_escape();
        }
        ClassAtom atom;
        atom.value = advance();
        return atom;
    }

    ECMAParser::ClassAtom ECMAParser::parse_class_escape() {
        // ClassEscape[U] :: b | [+U] - | CharacterClassEscape[?U] | CharacterEscape[?U]
        // The backslash is already consumed.
        ClassAtom atom;
        if (eat('b')) {
            atom.value = CH_BACKSPACE;
            return atom;
        }
        if (eat('-')) {
            atom.value = '-';
            return atom;
        }
        if (try_parse_character_class_escape(atom.escape)) {
            atom.is_class = true;
            return atom;
        }
        atom.value = parse_character_escape();
        return atom;
    }

    // ---------------- Capturing groups and backreferences ----------------

    GroupID ECMAParser::create_capturing_group() {
        // Early error (21.2.1.1): NcapturingParens ≥ 2^32 - 1
        if (m_num_capturing_groups >= std::numeric_limits<GroupID>::max() - 1) {
            syntax_error("too many capturing groups");
        }
        return ++m_num_capturing_groups;
    }

    void ECMAParser::register_group_name(const zstring& name, const GroupID gid, const std::size_t position) {
        // Early error (21.2.1.1): multiple GroupSpecifiers with the same StringValue of their names
        for (const NamedGroup& group : m_named_groups) {
            if (group.name == name) {
                syntax_error("duplicate capture group name", position);
            }
        }
        m_named_groups.push_back({name, gid});
    }

    void ECMAParser::resolve_backreferences() {
        for (const auto& [backref, position, name] : m_pending_backrefs) {
            if (name.empty()) {
                // Early error (21.2.1.1): CapturingGroupNumber of DecimalEscape is larger than NcapturingParens
                if (backref->get_group_id() > m_num_capturing_groups) {
                    syntax_error("reference to non-existent group \\" + std::to_string(backref->get_group_id()),
                                 position);
                }
                continue;
            }

            // Early error (21.2.1.1): \k GroupName must refer to some GroupSpecifier of the pattern
            const zstring& group_name = name;
            const auto group = std::find_if(m_named_groups.begin(), m_named_groups.end(), [&](const NamedGroup& g) {
                return g.name == group_name;
            });
            if (group == m_named_groups.end()) {
                syntax_error("reference to undefined group name", position);
            }
            backref->set_group_id(group->gid);
        }
    }

    // ============= DFS CONTEXT =============

    void DFSContext::set_target(app* target) {
        m_target_string = target;
    }

    app* DFSContext::get_target() const {
        return m_target_string;
    }

    void DFSContext::set_end_vertex(VertexID v) {
        m_end_vertex = v;
    }

    VertexID DFSContext::get_end_vertex() const {
        return m_end_vertex;
    }

    void DFSContext::set_base_prefix(const expr_ref& p) {
        m_base_prefix = p;
    }

    expr_ref DFSContext::get_base_prefix() const {
        return m_base_prefix;
    }

    bool DFSContext::has_group(GroupID gid) const {
        return m_group_vars.contains(gid);
    }

    const std::vector<ActiveLookahead>& DFSContext::get_active_lookaheads() const {
        return m_active_lookaheads;
    }

    expr_ref_vector& DFSContext::get_unique_paths() {
        return m_unique_paths;
    }

    app_ref DFSContext::mk_fresh_string_var() {
        app_ref var(m_manager.mk_fresh_const("ecma_re", m_str_sort), m_manager);
        m_all_created_vars.push_back(var);
        return var;
    }

    const app_ref_vector& DFSContext::get_all_fresh_vars() const {
        return m_all_created_vars;
    }

    app_ref DFSContext::create_edge_var() {
        app_ref edge_var = mk_fresh_string_var();
        m_current_path_vars.push_back(edge_var);
        return edge_var;
    }

    void DFSContext::push_edge_var_to_groups(const app_ref& edge_var) {
        for (GroupID gid : m_active_groups) {
            m_group_vars.at(gid).push_back(edge_var);
        }
    }

    void DFSContext::add_path_constraint(const app_ref& constraint) {
        m_current_path_constraints.push_back(constraint);
    }

    void DFSContext::start_groups(const std::vector<GroupID>& gids) {
        for (GroupID gid : gids) {
            m_active_groups.push_back(gid);
            if (m_group_vars.contains(gid)) {
                m_group_vars.at(gid).reset();
            } else {
                m_group_vars.insert({gid, expr_ref_vector(m_manager)});
            }
        }
    }

    void DFSContext::end_groups(const std::vector<GroupID>& gids) {
        for (GroupID gid : gids) {
            auto it = std::ranges::find(m_active_groups, gid);
            if (it != m_active_groups.end()) {
                m_active_groups.erase(it);
            }
        }
    }

    void DFSContext::push_lookahead(const std::variant<app_ref, GraphFragment>& subregex, bool is_positive) {
        m_active_lookaheads.push_back({subregex, is_positive, m_current_path_vars.size() - 1});
    }

    void DFSContext::commit_current_path(const expr_ref_vector& additional_constraints) {
        expr_ref_vector final_constraints(m_manager);

        for (expr* c : m_current_path_constraints) {
            final_constraints.push_back(c);
        }
        for (expr* c : additional_constraints) {
            final_constraints.push_back(c);
        }

        expr_ref path_string = concat_vars();
        final_constraints.push_back(m_manager.mk_eq(m_target_string, path_string));
        expr_ref_vector conjunction(m_manager);
        for (expr* c : final_constraints) {
            conjunction.push_back(c);
        }
        m_unique_paths.push_back(m_manager.mk_and(conjunction));
    }

    expr_ref DFSContext::concat_expr_vector(const expr_ref_vector& vars, const uint32_t start_idx,
                                            const uint32_t end_idx) const {
        const uint32_t actual_end = std::min(end_idx, vars.size());
        expr* empty_str = m_util_s.str.mk_empty(m_str_sort);

        if (start_idx >= actual_end) {
            return {empty_str, m_manager};
        }

        expr_ref_vector apps_to_concat {m_manager};
        for (uint32_t i = start_idx; i < actual_end; i++) {
            if (vars.get(i) != empty_str) {
                apps_to_concat.push_back(vars.get(i));
            }
        }

        if (apps_to_concat.empty()) {
            return {empty_str, m_manager};
        }
        if (apps_to_concat.size() == 1) {
            return {apps_to_concat.get(0), m_manager};
        }

        expr* concat = m_util_s.str.mk_concat(apps_to_concat, m_str_sort);
        return {concat, m_manager};
    }

    expr_ref DFSContext::concat_vars(const uint32_t start_idx, const uint32_t end_idx) const {
        return concat_expr_vector(m_current_path_vars, start_idx, end_idx);
    }

    expr_ref DFSContext::concat_group_vars(GroupID gid) const {
        const auto& vars = m_group_vars.at(gid);
        return concat_expr_vector(vars, 0, vars.size());
    }

    expr_ref DFSContext::get_global_prefix() const {
        expr_ref local_prefix = concat_vars();
        expr* empty_str = m_util_s.str.mk_empty(m_str_sort);

        if (m_base_prefix.get() == empty_str) {
            return local_prefix;
        }
        if (local_prefix.get() == empty_str) {
            return m_base_prefix;
        }

        return expr_ref(m_util_s.str.mk_concat(m_base_prefix, local_prefix), m_manager);
    }

    DFSStateSnapshot DFSContext::save_snapshot() const {
        DFSStateSnapshot s;
        s.num_path_vars = m_current_path_vars.size();
        s.num_path_constraints = m_current_path_constraints.size();
        s.num_active_lookaheads = m_active_lookaheads.size();
        s.active_groups = m_active_groups;

        // Deep copy the group variables to the snapshot
        for (const auto& [group_id, vars] : m_group_vars) {
            auto copy = std::make_unique<expr_ref_vector>(m_manager);
            for (unsigned i = 0; i < vars.size(); i++) {
                copy->push_back(vars[i]);
            }
            s.group_vars.insert({group_id, std::move(copy)});
        }
        return s;
    }

    void DFSContext::restore_snapshot(const DFSStateSnapshot& s) {
        m_current_path_vars.resize(s.num_path_vars);
        m_current_path_constraints.resize(s.num_path_constraints);

        // Safely truncating active lookaheads using pop_back avoids the need for a default constructor
        // which std::variant<app_ref, GraphFragment> natively prevents from existing.
        while (m_active_lookaheads.size() > s.num_active_lookaheads) {
            m_active_lookaheads.pop_back();
        }

        m_active_groups = s.active_groups;

        // Clear existing expr_ref_vectors to remove items appended in the discarded path branch.
        // It's perfectly safe to do so because the expressions themselves are preserved
        // by the deep copies stored in the Snapshot object up until this point.
        m_group_vars.clear();
        for (const auto& kv : s.group_vars) {
            GroupID gid = kv.first;
            const auto& vec_ptr = kv.second;
            expr_ref_vector vec(m_manager);
            for (unsigned i = 0; i < vec_ptr->size(); ++i) {
                vec.push_back(vec_ptr->get(i));
            }
            m_group_vars.insert({gid, std::move(vec)});
        }
    }

    OuterSearchState DFSContext::suspend_for_inner_search() {
        OuterSearchState state;
        state.unique_paths = std::make_unique<expr_ref_vector>(m_manager);
        state.current_path_vars = std::make_unique<expr_ref_vector>(m_manager);
        state.current_path_constraints = std::make_unique<expr_ref_vector>(m_manager);
        state.active_groups = std::move(m_active_groups);
        state.active_lookaheads = std::move(m_active_lookaheads);
        state.end_vertex = m_end_vertex;
        state.target_string = m_target_string;

        // Safe shallow copies via Z3 ref vectors
        for (unsigned i = 0; i < m_unique_paths.size(); ++i) {
            state.unique_paths->push_back(m_unique_paths.get(i));
        }
        for (unsigned i = 0; i < m_current_path_vars.size(); ++i) {
            state.current_path_vars->push_back(m_current_path_vars.get(i));
        }
        for (unsigned i = 0; i < m_current_path_constraints.size(); ++i) {
            state.current_path_constraints->push_back(m_current_path_constraints.get(i));
        }

        for (const auto& kv : m_group_vars) {
            state.existing_group_ids.push_back(kv.first);
        }

        m_unique_paths.reset();
        m_current_path_vars.reset();
        m_current_path_constraints.reset();
        m_active_groups.clear();
        m_active_lookaheads.clear();

        return state;
    }

    void DFSContext::resume_from_inner_search(OuterSearchState& state) {
        for (auto it = m_group_vars.begin(); it != m_group_vars.end();) {
            if (std::ranges::find(state.existing_group_ids, it->first) != state.existing_group_ids.end()) {
                ++it;
            } else {
                it = m_group_vars.erase(it);
            }
        }

        m_unique_paths.reset();
        for (unsigned i = 0; i < state.unique_paths->size(); ++i) {
            m_unique_paths.push_back(state.unique_paths->get(i));
        }
        m_current_path_vars.reset();
        for (unsigned i = 0; i < state.current_path_vars->size(); ++i) {
            m_current_path_vars.push_back(state.current_path_vars->get(i));
        }
        m_current_path_constraints.reset();
        for (unsigned i = 0; i < state.current_path_constraints->size(); ++i) {
            m_current_path_constraints.push_back(state.current_path_constraints->get(i));
        }

        m_active_groups = std::move(state.active_groups);
        m_active_lookaheads = std::move(state.active_lookaheads);
        m_end_vertex = state.end_vertex;
        m_target_string = state.target_string;
    }

    // ============= REGEX CONSTRAINT BUILDER =============

    RegexConstraintGraph RegexConstraintBuilder::build_rcg() {
        const ASTNodeRef root = m_parser.parse();
        // Get the subgraph from AST

        std::unordered_set<GroupID> referenced_groups;
        root->collect_backrefs(referenced_groups);
        root->strip_unreferenced_captures(referenced_groups);

        const RegexComponent comp = root->get_subgraph(m_graph, m_util_s, m_manager);

        VertexID inner_start = UNKNOWN_VERTEX;
        VertexID inner_end = UNKNOWN_VERTEX;

        if (std::holds_alternative<app_ref>(comp)) {
            // If the entire regex is regular, create a match edge and wrap it in two vertices
            inner_start = m_graph.create_vertex();
            inner_end = m_graph.create_vertex();
            const EdgeID eid = m_graph.create_edge(inner_end, RCGEdgePayload {MatchEdge {std::get<app_ref>(comp)}});
            m_graph.vertices[inner_start].outgoing_edges.push_back(eid);
        } else {
            const GraphFragment frag = std::get<GraphFragment>(comp);
            inner_start = frag.v_in;
            inner_end = frag.v_out;
        }

        if (m_params.m_ecma_engine_semantics) {
            // Wrap the entire regex in Sigma* to mimic the regex engine matching semantics --> the solution is any
            // substring
            const app_ref sigma_star(m_util_s.re.mk_full_seq(nullptr), m_manager);

            m_graph.start_vertex = m_graph.create_vertex();
            const EdgeID prefix_eid = m_graph.create_edge(inner_start, RCGEdgePayload {MatchEdge {sigma_star}});
            m_graph.vertices[m_graph.start_vertex].outgoing_edges.push_back(prefix_eid);

            m_graph.end_vertex = m_graph.create_vertex();
            const EdgeID suffix_eid = m_graph.create_edge(m_graph.end_vertex, RCGEdgePayload {MatchEdge {sigma_star}});
            m_graph.vertices[inner_end].outgoing_edges.push_back(suffix_eid);
        } else {
            // No engine matching semantics --> the whole string must match the regex
            m_graph.start_vertex = inner_start;
            m_graph.end_vertex = inner_end;
        }

        return m_graph;
    }

    expr_ref RegexConstraintBuilder::generate_constraints(app* target_string) {
        // Just a little sanity check that the graph is (at least somehow) constructed
        SASSERT(m_graph.start_vertex != UNKNOWN_VERTEX);

        // Store the global target string in the context for access in nested DFS calls when evaluating anchors and
        // lookaheads
        m_global_target_string = target_string;

        // Initialize the base DFS context and start the traversal
        DFSContext ctx {m_manager, m_util_s};
        ctx.set_target(target_string);
        ctx.set_base_prefix(expr_ref(m_util_s.str.mk_empty(m_str_sort), m_manager));
        ctx.set_end_vertex(m_graph.end_vertex);
        rcg_dfs_visit(m_graph.start_vertex, ctx);

        // OR all the paths that were generated along the graph traversal
        expr_ref_vector& unique_paths = ctx.get_unique_paths();
        expr_ref result(m_manager);
        if (unique_paths.empty()) {
            result = {m_manager.mk_false(), m_manager};
        } else if (unique_paths.size() == 1) {
            SASSERT(is_expr(unique_paths.get(0)));
            result = {unique_paths.get(0), m_manager};
        } else {
            result = {m_manager.mk_or(unique_paths), m_manager};
        }

        // Snapshot the fresh Skolem variables for the caller (used to build the existentially quantified form
        // for the negative implication -- see handle_ecma_re in theory_str_noodler.cpp).
        m_fresh_vars.reset();
        for (unsigned i = 0; i < ctx.get_all_fresh_vars().size(); i++) {
            m_fresh_vars.push_back(ctx.get_all_fresh_vars().get(i));
        }

        return result;
    }

    const app_ref_vector& RegexConstraintBuilder::get_fresh_vars() const {
        return m_fresh_vars;
    }

    expr_ref RegexConstraintBuilder::run_inner_rcg_dfs(const GraphFragment& fragment, app* target_string,
                                                       DFSContext& ctx) {
        // Suspend the outer DFS context and save the state before proceeding with the inner DFS run
        auto suspended_state = ctx.suspend_for_inner_search();

        ctx.set_end_vertex(fragment.v_out);
        ctx.set_target(target_string);

        // Run the DFS
        rcg_dfs_visit(fragment.v_in, ctx);

        // Disjunct all the paths from the subgraph and add it to the current path constraints
        expr_ref inner_result(m_manager);
        auto& unique_paths = ctx.get_unique_paths();
        if (unique_paths.empty()) {
            inner_result = {m_manager.mk_false(), m_manager};
        } else if (unique_paths.size() == 1) {
            inner_result = {unique_paths.get(0), m_manager};
        } else {
            inner_result = {m_manager.mk_or(unique_paths), m_manager};
        }

        ctx.resume_from_inner_search(suspended_state);
        return inner_result;
    }

    void RegexConstraintBuilder::handle_lookaround_constraints(DFSContext& ctx, const AssertionEdge& assertion,
                                                               const expr_ref& global_prefix) {
        const auto& lookaround = std::get<Lookaround>(assertion.payload);

        if (lookaround.direction == LookaroundDirection::FORWARD) {
            // Lookaheads are postponed and evaluated at the end of each path in the graph because they depend on the
            // suffix of the matched string starting from current position.
            ctx.push_lookahead(lookaround.subregex, lookaround.is_positive);
        } else {
            // Lookbehinds are evaluated immediately since they depend on the prefix of the matched string up until the
            // current position Lookbehinds with regular subregexes are handled via automata operations
            if (std::holds_alternative<app_ref>(lookaround.subregex)) {
                // Create x = global_prefix (p1p2p3...)
                const app_ref lb_var(ctx.mk_fresh_string_var(), m_manager);
                ctx.add_path_constraint({m_manager.mk_eq(lb_var, global_prefix), m_manager});

                // The global prefix should end with a string that matches the lookbehind subregex, therefore generate
                // global_prefix \in (Sigma* concat lb.subregex)
                const app_ref& lb_regex_base = std::get<app_ref>(lookaround.subregex);
                const app_ref sigma_star = {m_util_s.re.mk_full_seq(nullptr), m_manager};
                app_ref lb_regex = {m_util_s.re.mk_concat(sigma_star, lb_regex_base), m_manager};
                if (!lookaround.is_positive) {
                    // negative regular lookbehind --> just complement the final lokbehind subregex
                    lb_regex = m_util_s.re.mk_complement(lb_regex);
                }
                const app_ref condition = {m_util_s.re.mk_in_re(lb_var, lb_regex), m_manager};
                ctx.add_path_constraint(condition);
            } else {
                // Non-regular lookbehind --> need to run a nested DFS to evaluate the inner graph fragment and generate
                // the corresponding constraints.
                if (!lookaround.is_positive) {
                    // Negative lookbehind with non-regular subregex leads to universal quantification
                    util::throw_error("Unsupported: negative lookbehind with non-regular subregex");
                }

                // Introduce a fresh variable x_lb for the subregex in lookbehind
                const GraphFragment& subregex_fragment = std::get<GraphFragment>(lookaround.subregex);
                const app_ref subregex_lb_var(ctx.mk_fresh_string_var(), m_manager);
                ctx.add_path_constraint({m_manager.mk_eq(global_prefix, subregex_lb_var), m_manager});

                // The lookbehind evaluates the subregex against the entire prefix --> the procedure starts from the
                // beginning of matched string, therefore the prefix is "".
                expr_ref old_base = ctx.get_base_prefix();
                ctx.set_base_prefix(expr_ref(m_util_s.str.mk_empty(m_str_sort), m_manager));

                // The subregex lookbehind is evaluated in a separate run of DFS on the subgraph.
                // We hand over x_lb which is the new global target for the inner DFS
                // run. The constraints for x_lb are generated in the same way.
                const expr_ref inner_result = run_inner_rcg_dfs(subregex_fragment, subregex_lb_var, ctx);

                // Restore the original base prefix for the outer DFS run and add the generated constraints for the
                // lookbehind.
                ctx.set_base_prefix(old_base);
                ctx.add_path_constraint({to_app(inner_result.get()), m_manager});
            }
        }
    }

    void RegexConstraintBuilder::generate_lookahead_constraints(DFSContext& ctx, expr_ref_vector& final_constraints) {
        for (const ActiveLookahead& la : ctx.get_active_lookaheads()) {
            expr_ref suffix = ctx.concat_vars(la.start_index);

            if (std::holds_alternative<app_ref>(la.subregex)) {
                app_ref la_var(ctx.mk_fresh_string_var(), m_manager);
                final_constraints.push_back(m_manager.mk_eq(la_var, suffix));
                const app_ref& la_regex_base = std::get<app_ref>(la.subregex);
                const app_ref sigma_star = {m_util_s.re.mk_full_seq(nullptr), m_manager};
                app_ref la_regex = {m_util_s.re.mk_concat(la_regex_base, sigma_star), m_manager};
                if (!la.is_positive) {
                    la_regex = m_util_s.re.mk_complement(la_regex);
                }
                const app_ref condition = {m_util_s.re.mk_in_re(la_var, la_regex), m_manager};
                final_constraints.push_back(condition);
            } else {
                SASSERT(std::holds_alternative<GraphFragment>(la.subregex));
                if (!la.is_positive) {
                    util::throw_error("Unsupported: negative lookaround with non-regular inner content "
                                      "(would require universal quantifiers)");
                }

                const GraphFragment& inner_frag = std::get<GraphFragment>(la.subregex);
                app_ref subregex_la_var(ctx.mk_fresh_string_var(), m_manager);
                final_constraints.push_back(m_manager.mk_eq(suffix, subregex_la_var));

                // The nested graph must inherently know its specific global position relative to the target string
                expr_ref old_base = ctx.get_base_prefix();
                expr_ref local_prefix_to_la = ctx.concat_vars(0, la.start_index);
                ctx.set_base_prefix(expr_ref(m_util_s.str.mk_concat(old_base, local_prefix_to_la), m_manager));

                expr_ref inner_result = run_inner_rcg_dfs(inner_frag, subregex_la_var, ctx);

                ctx.set_base_prefix(old_base);
                final_constraints.push_back(inner_result);
            }
        }
    }

    void RegexConstraintBuilder::generate_edge_constraints(DFSContext& ctx, const RCGEdge& edge,
                                                           const app_ref& edge_var) {
        // Get all the string variables preceding the currently processed edge.
        // This is important for lookaround and anchor evaluation, since they are dependent on the absolute position in
        // the text.
        expr_ref global_prefix = ctx.get_global_prefix();

        // The edge contains regular payload --> generate str.in_re(edge_var, payload)
        if (std::holds_alternative<MatchEdge>(edge.payload)) {
            const app_ref regex = std::get<MatchEdge>(edge.payload).regex;
            ctx.add_path_constraint({m_util_s.re.mk_in_re(edge_var, regex), m_manager});
        }
        // Assertion (or zero-width assertion) --> no text is consumed, therefore edge_var = ""
        else if (std::holds_alternative<AssertionEdge>(edge.payload)) {
            const AssertionEdge& assertion = std::get<AssertionEdge>(edge.payload);
            ctx.add_path_constraint({m_manager.mk_eq(edge_var, m_util_s.str.mk_empty(m_str_sort)), m_manager});
            if (std::holds_alternative<Anchor>(assertion.payload)) {
                const Z3Char anchor = std::get<Anchor>(assertion.payload);
                // '^' anchor means 'nothing is matched before this position' --> global prefix must be empty
                if (anchor == '^') {
                    ctx.add_path_constraint(
                        {m_manager.mk_eq(global_prefix, m_util_s.str.mk_empty(m_str_sort)), m_manager});
                }
                // '$' anchor means 'nothing is matched after this position' --> global prefix must be equal to the
                // entire target string
                else if (anchor == '$') {
                    ctx.add_path_constraint(
                        {m_manager.mk_eq(global_prefix, app_ref(m_global_target_string, m_manager)), m_manager});
                } else {
                    util::throw_error("Internal error: RegexConstraintBuilder::generate_edge_constraints: anchor != "
                                      "'$' && anchor != '^'");
                }
            } else if (std::holds_alternative<Lookaround>(assertion.payload)) {
                handle_lookaround_constraints(ctx, assertion, global_prefix);
            } else {
                util::throw_error("Internal error: RegexConstraintBuilder::generate_edge_constraints: Assertion is "
                                  "neither Anchor nor Lookaround");
            }
        }
        // Backreference --> edge_var must be equal to the concatenation of all variables in the referenced group on the
        // current path. If the referenced group is not active, it is a forward reference -- matches empty string.
        // https://tc39.es/ecma262/2020/#sec-backreferencematcher -- "e. If `s` is undefined, return c(x)."
        else if (std::holds_alternative<BackrefEdge>(edge.payload)) {
            const GroupID ref_id = std::get<BackrefEdge>(edge.payload).backref_id;
            if (ctx.has_group(ref_id)) {
                // Backreference -- concatenate all the variables accumulated in the capture group during the current
                // path.
                expr_ref captured_string = ctx.concat_group_vars(ref_id);
                ctx.add_path_constraint({m_manager.mk_eq(edge_var, captured_string), m_manager});
            } else {
                // Forward reference
                ctx.add_path_constraint({m_manager.mk_eq(edge_var, m_util_s.str.mk_empty(m_str_sort)), m_manager});
            }
        } else {
            util::throw_error("Internal error: RegexConstraintBuilder::generate_edge_constraints: edge payload is "
                              "neither BackrefEdge, AssertionEdge nor MatchEdge");
        }
    }

    void RegexConstraintBuilder::rcg_dfs_visit(const VertexID current_vertex, DFSContext& ctx) {
        // End of (sub)graph reached --> evaluate postponed lookaheads and commit current path
        if (current_vertex == ctx.get_end_vertex()) {
            expr_ref_vector final_constraints(m_manager);
            generate_lookahead_constraints(ctx, final_constraints);
            ctx.commit_current_path(final_constraints);
            return;
        }

        for (EdgeID eid : m_graph.vertices[current_vertex].outgoing_edges) {
            // Before continuing, save a snapshot of the previous edge in case of alternation
            const RCGEdge& edge = m_graph.edges[eid];
            DFSStateSnapshot snapshot = ctx.save_snapshot();

            // Mark all groups that begin on this edge as active
            if (m_graph.group_starts.contains(eid)) {
                ctx.start_groups(m_graph.group_starts.at(eid));
            }

            // Create a fresh variable (and add it to all the active capture groups) and generate constraints for the
            // current edge
            app_ref edge_var = ctx.create_edge_var();
            ctx.push_edge_var_to_groups(edge_var);
            generate_edge_constraints(ctx, edge, edge_var);

            // Mark all groups that end on this edge as inactive
            if (m_graph.group_ends.contains(eid)) {
                ctx.end_groups(m_graph.group_ends.at(eid));
            }

            // Continue DFS on the next edge, rollback to the snapshot after exploring the branch
            rcg_dfs_visit(edge.target, ctx);
            ctx.restore_snapshot(snapshot);
        }
    }

    bool GraphFragment::is_initialized() const {
        return v_in == std::numeric_limits<VertexID>::max() && v_out == std::numeric_limits<VertexID>::max();
    }
}  // namespace smt::noodler::ecma
