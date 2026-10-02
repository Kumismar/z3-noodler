#pragma once

#include "smt/theory_str_noodler/extended_regex/backend.h"
#include "smt/theory_str_noodler/extended_regex/extended_regex.h"
#include "util/zstring.h"
#include "util/zstring_view.h"

#include <string>
#include <vector>

// Frontend for ECMAScript regexes ((_ re.from_regex ecma2020), alias re.from_ecma2020), following ECMA-262 2020 (11th edition), section 21.2.
namespace smt::noodler::extended_regex::ecma {
    /**
     * @brief Convert utf-8 @p raw_input into a sanitized form where each Z3Char (uint32_t) represents a single Unicode
     * code point.
     *
     * Characters above 0xFF were already decoded by the SMT-LIB parser and are kept as they are. Invalid UTF-8
     * sequences are replaced by U+FFFD. ECMAScript escape sequences (such as `\u0041`) are kept untouched, they are
     * part of the pattern grammar and they are decoded by ECMAParser.
     *
     * @param raw_input The original ECMA regex pattern as a UTF-8 encoded string. May contain multi-byte
     *                  characters.
     * @return zstring The sanitized regex pattern.
     */
    zstring sanitize_ecma_regex_input(const zstring& raw_input);

    // =============== ECMA REGEX PARSER ===============

    /**
     * Recursive descent parser of ECMAScript regex patterns.
     *
     * The parser follows the grammar of ECMA-262 2020, 21.2.1 Patterns, with the goal symbol Pattern[+U, +N], i.e., as
     * if the `u` flag was always set (21.2.3.2.2 RegExpInitialize, step 8). Therefore, the Annex B extensions (B.1.4)
     * are not used, since they apply only to [~U] patterns. The other flags are not supported (they are considered
     * unset).
     *
     * The regex grammar is a lexical one -- the meaning of a character depends on its context (inside/outside of a
     * character class, after '\' or after '(?'), so the parser reads code points directly without a separate lexer.
     * Each nonterminal of the grammar has its own parse_* method. The early errors (21.2.1.1) are checked during
     * parsing, except for the ones concerning backreferences, which are checked after the whole pattern is parsed
     * (backreferences can point forward).
     *
     * All errors are reported via util::throw_error.
     *
     * The parser produces the common AST of the backend; the ECMAScript meaning of the predefined character classes
     * (., \d, \s, \w and their negations) is expressed by explicit sets of characters (CharSet).
     *
     * The AST keeps views (zstring_view) into the pattern (group names, Unicode property names), so the pattern must
     * outlive the AST.
     */
    class ECMAParser {
    public:
        explicit ECMAParser(const zstring_view pattern)
            : m_pattern(pattern) { }

        /**
         * @brief Parse the whole Pattern and check its early errors.
         *
         * @return ASTNodeRef The root of the AST.
         */
        ASTNodeRef parse();

        /**
         * @brief Return NcapturingParens, the number of capturing groups in the parsed pattern.
         */
        GroupID num_capturing_groups() const;

    private:
        // Names are compared by their StringValue (21.2.1.6) -- the escape sequences are replaced, so the name can
        // differ from the source text and it cannot be a view into the pattern.
        struct NamedGroup {
            zstring name;
            GroupID gid;
        };

        // A backreference whose group is checked/resolved after the whole pattern is parsed.
        struct PendingBackref {
            ASTNodeBackreference* node;
            std::size_t position;
            zstring name;  // StringValue of the GroupName, empty for numeric backreferences
        };

        // The result of the ClassAtom production -- a single character or a character class escape (a CharSet or
        // a UnicodeProperty in `char_set`).
        struct ClassAtom {
            bool is_class = false;
            Z3Char value = 0;
            ClassItem char_set {};
        };

        zstring_view m_pattern;
        std::size_t m_pos = 0;
        GroupID m_num_capturing_groups = 0;
        std::vector<NamedGroup> m_named_groups;
        std::vector<PendingBackref> m_pending_backrefs;

        // ---- reading the pattern ----
        bool at_end() const;
        Z3Char peek(std::size_t offset = 0) const;
        Z3Char advance();
        bool eat(Z3Char ch);
        void expect(Z3Char ch, const char* what);
        void syntax_error(const std::string& message, std::size_t position) const;
        void syntax_error(const std::string& message) const;
        // A view of the pattern from @p begin to the current position
        zstring_view source_view(std::size_t begin) const;

        // ---- grammar productions (21.2.1) ----
        ASTNodeRef parse_disjunction();
        ASTNodeRef parse_alternative();
        ASTNodeRef parse_term();
        ASTNodeRef try_parse_assertion();
        bool try_parse_quantifier(Quantifier& quantifier);
        uint64_t parse_decimal_digits(const char* what);
        ASTNodeRef parse_atom();
        ASTNodeRef parse_group();
        ASTNodeRef parse_atom_escape();
        Z3Char parse_character_escape();
        bool try_parse_character_class_escape(ClassItem& char_set);
        void parse_unicode_property_value_expression(UnicodeProperty& property);
        Z3Char parse_regexp_unicode_escape_sequence();
        bool try_parse_hex4_digits(std::size_t offset, Z3Char& value) const;
        zstring parse_group_name(zstring_view& source_text);
        Z3Char parse_regexp_identifier_char(bool is_start);
        ASTNodeRef parse_character_class();
        void parse_class_ranges(ASTNodeCharClass& char_class);
        ClassAtom parse_class_atom();
        ClassAtom parse_class_escape();

        // ---- capturing groups and backreferences ----
        GroupID create_capturing_group();
        void register_group_name(const zstring& name, GroupID gid, std::size_t position);
        void resolve_backreferences();
    };

    // =============== ECMA REGEX ===============

    /**
     * An ECMAScript regex pattern. The pattern is sanitized (see sanitize_ecma_regex_input) in the constructor and
     * parsed by ECMAParser into the common AST.
     */
    class ECMARegex : public ExtendedRegex {
    public:
        explicit ECMARegex(const zstring& pattern);

        RegexFlavor get_flavor() const override;
        ASTNodeRef parse() const override;
    };
}  // namespace smt::noodler::extended_regex::ecma
