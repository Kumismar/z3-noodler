#include "ecma.h"

#include "smt/theory_str_noodler/util.h"
#include "util/debug.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

namespace smt::noodler::extended_regex::ecma {
    // ======================= UTILS =======================

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

    // Characters matched by \d (21.2.2.12 CharacterClassEscape)
    constexpr std::array<ClassRange, 1> DIGIT_RANGES {{{'0', '9'}}};

    // Characters matched by \w -- WordCharacters() with IgnoreCase = false (21.2.2.6.1)
    constexpr std::array<ClassRange, 4> WORD_RANGES {{{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}}};

    // Characters not matched by . -- LineTerminator (21.2.2.8 Atom with DotAll = false, 11.3 Table 33)
    constexpr std::array<ClassRange, 3> LINE_TERMINATOR_RANGES {{{CH_LF, CH_LF}, {CH_CR, CH_CR}, {CH_LS, CH_PS}}};

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

    template <std::size_t N>
    static std::vector<ClassRange> to_ranges(const std::array<ClassRange, N>& ranges) {
        return {ranges.begin(), ranges.end()};
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

    zstring_view ECMAParser::source_view(const std::size_t begin) const {
        SASSERT(begin <= m_pos);
        return {m_pattern + static_cast<uint32_t>(begin), static_cast<uint32_t>(m_pos - begin)};
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
            case '.': {
                // DotAll is false (the `s` flag is not supported) --> all characters except LineTerminator
                // (21.2.2.8 Atom)
                const std::size_t start = m_pos;
                advance();
                auto char_class = std::make_unique<ASTNodeCharClass>(false);
                char_class->add_item(CharSet {to_ranges(LINE_TERMINATOR_RANGES), true, source_view(start)});
                return char_class;
            }
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

        ClassItem char_set;
        if (try_parse_character_class_escape(char_set)) {
            auto char_class = std::make_unique<ASTNodeCharClass>(false);
            char_class->add_item(std::move(char_set));
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

    bool ECMAParser::try_parse_character_class_escape(ClassItem& char_set) {
        // CharacterClassEscape[U] :: d | D | s | S | w | W | [+U] p{ UnicodePropertyValueExpression } |
        //                            [+U] P{ UnicodePropertyValueExpression }
        // The backslash is already consumed. The sets of characters are given by 21.2.2.12.
        const std::size_t start = m_pos - 1;
        const Z3Char letter = peek();
        std::vector<ClassRange> ranges;
        switch (letter) {
            case 'd':
            case 'D':
                ranges = to_ranges(DIGIT_RANGES);
                break;
            case 's':
            case 'S':
                ranges = to_ranges(WHITESPACE_RANGES);
                break;
            case 'w':
            case 'W':
                ranges = to_ranges(WORD_RANGES);
                break;
            case 'p':
            case 'P': {
                advance();
                UnicodeProperty property;
                property.negated = letter == 'P';
                parse_unicode_property_value_expression(property);
                property.source = source_view(start);
                char_set = property;
                return true;
            }
            default:
                return false;
        }
        advance();
        // The upper-case escapes are the complements of the lower-case ones
        const bool negated = letter == 'D' || letter == 'S' || letter == 'W';
        char_set = CharSet {std::move(ranges), negated, source_view(start)};
        return true;
    }

    void ECMAParser::parse_unicode_property_value_expression(UnicodeProperty& property) {
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
            return source_view(begin);
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
            property.name = first;
            property.value = value;
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
        property.value = first;
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
        source_text = source_view(begin);
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
                    char_class.add_item(std::move(first.char_set));
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
        if (try_parse_character_class_escape(atom.char_set)) {
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

    // =============== ECMA REGEX ===============

    ECMARegex::ECMARegex(const zstring& pattern)
        : ExtendedRegex(sanitize_ecma_regex_input(pattern)) { }

    RegexFlavor ECMARegex::get_flavor() const {
        return RegexFlavor::ECMA2020;
    }

    ASTNodeRef ECMARegex::parse() const {
        ECMAParser parser(get_pattern());
        return parser.parse();
    }
}  // namespace smt::noodler::extended_regex::ecma
