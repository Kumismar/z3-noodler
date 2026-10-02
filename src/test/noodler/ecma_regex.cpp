#include "smt/theory_str_noodler/ecma_regex.h"

#include "ast/ast.h"
#include "ast/ast_pp.h"
#include "ast/reg_decl_plugins.h"
#include "params/theory_str_noodler_params.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

// Section numbers in the comments refer to ECMA-262 2020.
namespace smt::noodler::ecma::test {
    // Converts ASCII @p text to a zstring character by character. Unlike zstring(const char*), it does not decode
    // the "\u" escape sequences, so they are kept for the ECMA regex parser.
    zstring raw(const std::string& text) {
        std::vector<unsigned> chars;
        for (const char ch : text) {
            chars.push_back(static_cast<unsigned char>(ch));
        }
        return {static_cast<unsigned>(chars.size()), chars.data()};
    }

    zstring from_code_points(const std::vector<unsigned>& code_points) {
        return {static_cast<unsigned>(code_points.size()), code_points.data()};
    }

    std::vector<unsigned> code_points(const zstring& str) {
        std::vector<unsigned> res;
        for (unsigned i = 0; i < str.length(); i++) {
            res.push_back(str[i]);
        }
        return res;
    }

    std::string parse_and_serialize(const zstring& pattern) {
        ECMAParser parser(pattern);
        const ASTNodeRef root = parser.parse();
        return root->serialize().encode();
    }

    std::string parse_and_serialize(const std::string& pattern) {
        return parse_and_serialize(raw(pattern));
    }

    std::string parse_and_serialize(const char* pattern) {
        return parse_and_serialize(raw(pattern));
    }
}  // namespace smt::noodler::ecma::test

// =====================================================================
// INPUT DECODING TESTS
// =====================================================================

TEST_CASE("ECMA Regex input decoding", "[noodler][ecma]") {
    using namespace smt::noodler::ecma;
    using namespace smt::noodler::ecma::test;

    SECTION("ASCII is kept") {
        REQUIRE(code_points(sanitize_ecma_regex_input(raw("a(b)*"))) ==
                std::vector<unsigned> {'a', '(', 'b', ')', '*'});
    }

    SECTION("Escape sequences are kept for the parser") {
        // "A" must stay 6 characters, otherwise "*" would become a quantifier
        REQUIRE(code_points(sanitize_ecma_regex_input(raw("\\u0041"))) ==
                std::vector<unsigned> {'\\', 'u', '0', '0', '4', '1'});
        REQUIRE(code_points(sanitize_ecma_regex_input(raw("\\u{41}"))) ==
                std::vector<unsigned> {'\\', 'u', '{', '4', '1', '}'});
    }

    SECTION("Multi-byte UTF-8 sequences are decoded") {
        // U+00E1 (2 bytes), U+20AC (3 bytes), U+1F600 (4 bytes)
        REQUIRE(code_points(sanitize_ecma_regex_input(from_code_points({0xC3, 0xA1}))) ==
                std::vector<unsigned> {0xE1});
        REQUIRE(code_points(sanitize_ecma_regex_input(from_code_points({0xE2, 0x82, 0xAC}))) ==
                std::vector<unsigned> {0x20AC});
        REQUIRE(code_points(sanitize_ecma_regex_input(from_code_points({'a', 0xF0, 0x9F, 0x98, 0x80, 'b'}))) ==
                std::vector<unsigned> {'a', 0x1F600, 'b'});
    }

    SECTION("Already decoded characters are kept") {
        REQUIRE(code_points(sanitize_ecma_regex_input(from_code_points({0x100, 'a', 0x20AC}))) ==
                std::vector<unsigned> {0x100, 'a', 0x20AC});
    }

    SECTION("Invalid UTF-8 sequences are replaced by U+FFFD") {
        // lone continuation byte
        REQUIRE(code_points(sanitize_ecma_regex_input(from_code_points({0x80, 'a'}))) ==
                std::vector<unsigned> {0xFFFD, 'a'});
        // truncated sequence at the end of the input
        REQUIRE(code_points(sanitize_ecma_regex_input(from_code_points({'a', 0xC3}))) ==
                std::vector<unsigned> {'a', 0xFFFD});
        REQUIRE(code_points(sanitize_ecma_regex_input(from_code_points({0xE2, 0x82}))) ==
                std::vector<unsigned> {0xFFFD, 0xFFFD});
        // overlong encoding of U+0000
        REQUIRE(code_points(sanitize_ecma_regex_input(from_code_points({0xC0, 0x80}))) ==
                std::vector<unsigned> {0xFFFD});
    }
}

// =====================================================================
// PARSER TESTS
// =====================================================================

TEST_CASE("ECMA Regex Parser", "[noodler][ecma]") {
    using namespace smt::noodler::ecma;
    using namespace smt::noodler::ecma::test;

    // ---- 21.2.1 Pattern, Disjunction, Alternative ----

    SECTION("Literals and Concatenation") {
        REQUIRE(parse_and_serialize("a") == "(SEQ (LIT 'a'))");
        REQUIRE(parse_and_serialize("ab") == "(SEQ (LIT 'a') (LIT 'b'))");
        REQUIRE(parse_and_serialize("abc") == "(SEQ (LIT 'a') (LIT 'b') (LIT 'c'))");
        REQUIRE(parse_and_serialize("") == "(SEQ)");
    }

    SECTION("Alternation (Disjunction)") {
        REQUIRE(parse_and_serialize("a|b") == "(DISJ (SEQ (LIT 'a')) (SEQ (LIT 'b')))");
        REQUIRE(parse_and_serialize("a|b|c") == "(DISJ (SEQ (LIT 'a')) (SEQ (LIT 'b')) (SEQ (LIT 'c')))");
        REQUIRE(parse_and_serialize("a|") == "(DISJ (SEQ (LIT 'a')) (SEQ))");
        REQUIRE(parse_and_serialize("|a") == "(DISJ (SEQ) (SEQ (LIT 'a')))");
        REQUIRE(parse_and_serialize("|") == "(DISJ (SEQ) (SEQ))");
    }

    SECTION("Operator Precedence") {
        REQUIRE(parse_and_serialize("ab*") == "(SEQ (LIT 'a') (QUANT {0,inf} (LIT 'b')))");
        REQUIRE(parse_and_serialize("a|bc") == "(DISJ (SEQ (LIT 'a')) (SEQ (LIT 'b') (LIT 'c')))");
        REQUIRE(parse_and_serialize("a|b*") == "(DISJ (SEQ (LIT 'a')) (SEQ (QUANT {0,inf} (LIT 'b'))))");
    }

    // ---- 21.2.1 PatternCharacter ----

    SECTION("Pattern characters") {
        REQUIRE(parse_and_serialize("/-,:=!'\"<>") ==
                "(SEQ (LIT '/') (LIT '-') (LIT ',') (LIT ':') (LIT '=') (LIT '!') (LIT ''') (LIT '\"') (LIT '<') "
                "(LIT '>'))");
        REQUIRE(parse_and_serialize(from_code_points({0xE1, 0x1F600})) == "(SEQ (LIT U+00E1) (LIT U+1F600))");
    }

    SECTION("Syntax characters must be escaped") {
        // SyntaxCharacter is not a PatternCharacter; the Annex B fallbacks are only for [~U]
        REQUIRE_THROWS(parse_and_serialize("]"));
        REQUIRE_THROWS(parse_and_serialize("a]"));
        REQUIRE_THROWS(parse_and_serialize("}"));
        REQUIRE_THROWS(parse_and_serialize("{"));
        REQUIRE_THROWS(parse_and_serialize("a{"));
    }

    SECTION("Dot") {
        REQUIRE(parse_and_serialize(".") == "(SEQ (DOT))");
        REQUIRE(parse_and_serialize(".*") == "(SEQ (QUANT {0,inf} (DOT)))");
    }

    // ---- 21.2.1 Quantifier ----

    SECTION("Quantifiers") {
        REQUIRE(parse_and_serialize("a*") == "(SEQ (QUANT {0,inf} (LIT 'a')))");
        REQUIRE(parse_and_serialize("a+") == "(SEQ (QUANT {1,inf} (LIT 'a')))");
        REQUIRE(parse_and_serialize("a?") == "(SEQ (QUANT {0,1} (LIT 'a')))");
        REQUIRE(parse_and_serialize("a{3}") == "(SEQ (QUANT {3,3} (LIT 'a')))");
        REQUIRE(parse_and_serialize("a{3,}") == "(SEQ (QUANT {3,inf} (LIT 'a')))");
        REQUIRE(parse_and_serialize("a{3,5}") == "(SEQ (QUANT {3,5} (LIT 'a')))");
        REQUIRE(parse_and_serialize("a{0}") == "(SEQ (QUANT {0,0} (LIT 'a')))");
        REQUIRE(parse_and_serialize("a{007,010}") == "(SEQ (QUANT {7,10} (LIT 'a')))");
        REQUIRE(parse_and_serialize("a{2,2}") == "(SEQ (QUANT {2,2} (LIT 'a')))");
    }

    SECTION("Lazy quantifiers") {
        REQUIRE(parse_and_serialize("a*?") == "(SEQ (QUANT {0,inf} lazy (LIT 'a')))");
        REQUIRE(parse_and_serialize("a+?") == "(SEQ (QUANT {1,inf} lazy (LIT 'a')))");
        REQUIRE(parse_and_serialize("a??") == "(SEQ (QUANT {0,1} lazy (LIT 'a')))");
        REQUIRE(parse_and_serialize("a{2,3}?") == "(SEQ (QUANT {2,3} lazy (LIT 'a')))");
        REQUIRE(parse_and_serialize("a{2,}?b") == "(SEQ (QUANT {2,inf} lazy (LIT 'a')) (LIT 'b'))");
    }

    SECTION("Quantifier without preceding atom throws") {
        REQUIRE_THROWS(parse_and_serialize("*"));
        REQUIRE_THROWS(parse_and_serialize("+"));
        REQUIRE_THROWS(parse_and_serialize("?"));
        REQUIRE_THROWS(parse_and_serialize("{1}"));
        REQUIRE_THROWS(parse_and_serialize("a|*"));
        REQUIRE_THROWS(parse_and_serialize("(*)"));
    }

    SECTION("Double quantifier throws") {
        REQUIRE_THROWS(parse_and_serialize("a**"));
        REQUIRE_THROWS(parse_and_serialize("a+*"));
        REQUIRE_THROWS(parse_and_serialize("a*??"));
        REQUIRE_THROWS(parse_and_serialize("a{2}{3}"));
    }

    SECTION("Incomplete {} quantifier throws") {
        REQUIRE_THROWS(parse_and_serialize("a{"));
        REQUIRE_THROWS(parse_and_serialize("a{1"));
        REQUIRE_THROWS(parse_and_serialize("a{1,"));
        REQUIRE_THROWS(parse_and_serialize("a{,2}"));
        REQUIRE_THROWS(parse_and_serialize("a{,}"));
        REQUIRE_THROWS(parse_and_serialize("a{1,2X"));
        REQUIRE_THROWS(parse_and_serialize("a{x}"));
        REQUIRE_THROWS(parse_and_serialize("a{-1}"));
    }

    SECTION("Quantifier bounds out of order throw (early error)") {
        REQUIRE_THROWS(parse_and_serialize("a{3,1}"));
        REQUIRE_THROWS(parse_and_serialize("a{1,0}"));
    }

    SECTION("Too large quantifier bound throws") {
        REQUIRE_THROWS(parse_and_serialize("a{99999999999999999999}"));
    }

    SECTION("Quantified assertion throws") {
        // QuantifiableAssertion (Annex B) is only for [~U]
        REQUIRE_THROWS(parse_and_serialize("^*"));
        REQUIRE_THROWS(parse_and_serialize("$+"));
        REQUIRE_THROWS(parse_and_serialize("\\b?"));
        REQUIRE_THROWS(parse_and_serialize("\\B{2}"));
        REQUIRE_THROWS(parse_and_serialize("(?=a)*"));
        REQUIRE_THROWS(parse_and_serialize("(?!a)+"));
        REQUIRE_THROWS(parse_and_serialize("(?<=a)?"));
        REQUIRE_THROWS(parse_and_serialize("(?<!a){1}"));
    }

    // ---- 21.2.1 Atom: groups ----

    SECTION("Groups (Normal, Non-capturing, Named)") {
        REQUIRE(parse_and_serialize("()") == "(SEQ (GROUP #1 (SEQ)))");
        REQUIRE(parse_and_serialize("(a)") == "(SEQ (GROUP #1 (SEQ (LIT 'a'))))");
        REQUIRE(parse_and_serialize("(ab)+") == "(SEQ (QUANT {1,inf} (GROUP #1 (SEQ (LIT 'a') (LIT 'b')))))");
        REQUIRE(parse_and_serialize("(?:a)") == "(SEQ (GROUP-NONCAP (SEQ (LIT 'a'))))");
        REQUIRE(parse_and_serialize("(?:)") == "(SEQ (GROUP-NONCAP (SEQ)))");
        REQUIRE(parse_and_serialize("(?<foo>a)") == "(SEQ (GROUP #1 <foo> (SEQ (LIT 'a'))))");
    }

    SECTION("Nested groups") {
        REQUIRE(parse_and_serialize("((a))") == "(SEQ (GROUP #1 (SEQ (GROUP #2 (SEQ (LIT 'a'))))))");
        REQUIRE(parse_and_serialize("(?:(?:a))") == "(SEQ (GROUP-NONCAP (SEQ (GROUP-NONCAP (SEQ (LIT 'a'))))))");
        REQUIRE(parse_and_serialize("(?:(?<foo>a))") == "(SEQ (GROUP-NONCAP (SEQ (GROUP #1 <foo> (SEQ (LIT 'a'))))))");
        REQUIRE(parse_and_serialize("(a(b+)c)*") == "(SEQ (QUANT {0,inf} (GROUP #1 (SEQ"
                                                    " (LIT 'a')"
                                                    " (GROUP #2 (SEQ (QUANT {1,inf} (LIT 'b'))))"
                                                    " (LIT 'c')"
                                                    "))))");
    }

    SECTION("Groups are numbered by their left parentheses") {
        // parenIndex (21.2.2.8) -- the number of left-capturing parentheses to the left
        REQUIRE(parse_and_serialize("(a(?:b)(c))(?<n>d)") == "(SEQ"
                                                             " (GROUP #1 (SEQ"
                                                             " (LIT 'a')"
                                                             " (GROUP-NONCAP (SEQ (LIT 'b')))"
                                                             " (GROUP #2 (SEQ (LIT 'c')))"
                                                             "))"
                                                             " (GROUP #3 <n> (SEQ (LIT 'd')))"
                                                             ")");
    }

    SECTION("Alternation inside groups") {
        REQUIRE(parse_and_serialize("(a|b)+") == "(SEQ (QUANT {1,inf} (GROUP #1 (DISJ"
                                                 " (SEQ (LIT 'a'))"
                                                 " (SEQ (LIT 'b'))"
                                                 "))))");
        REQUIRE(parse_and_serialize("(a|b)|(c|d)") == "(DISJ"
                                                      " (SEQ (GROUP #1 (DISJ (SEQ (LIT 'a')) (SEQ (LIT 'b')))))"
                                                      " (SEQ (GROUP #2 (DISJ (SEQ (LIT 'c')) (SEQ (LIT 'd')))))"
                                                      ")");
        REQUIRE(parse_and_serialize("(ab){2,4}") == "(SEQ (QUANT {2,4} (GROUP #1 (SEQ (LIT 'a') (LIT 'b')))))");
    }

    SECTION("Group names") {
        // RegExpIdentifierName[+U] -- $, _, letters, digits (not at the start), escapes, non-ASCII characters
        REQUIRE(parse_and_serialize("(?<$_aZ9>x)") == "(SEQ (GROUP #1 <$_aZ9> (SEQ (LIT 'x'))))");
        REQUIRE(parse_and_serialize("(?<_>x)") == "(SEQ (GROUP #1 <_> (SEQ (LIT 'x'))))");
        // Names are matched by their StringValue with the escapes replaced (21.2.1.6), the AST keeps the source text
        // (serialized by zstring::encode(), which prints the backslash before 'u' as \u{5c})
        REQUIRE(parse_and_serialize("(?<\\u0061b>c)\\k<ab>") ==
                "(SEQ (GROUP #1 <\\u{5c}u0061b> (SEQ (LIT 'c'))) (BACKREF 1 <ab>))");
        REQUIRE(parse_and_serialize("(?<a\\u{62}>c)\\k<\\u{61}b>") ==
                "(SEQ (GROUP #1 <a\\u{5c}u{62}> (SEQ (LIT 'c'))) (BACKREF 1 <\\u{5c}u{61}b>))");
        REQUIRE_NOTHROW(parse_and_serialize(from_code_points({'(', '?', '<', 0xE1, 0x200C, '>', 'x', ')'})));
    }

    SECTION("Invalid groups throw") {
        REQUIRE_THROWS(parse_and_serialize("(?"));
        REQUIRE_THROWS(parse_and_serialize("(?X)"));
        REQUIRE_THROWS(parse_and_serialize("(?i:a)"));
        REQUIRE_THROWS(parse_and_serialize("(?<"));
        REQUIRE_THROWS(parse_and_serialize("(?<name"));
        REQUIRE_THROWS(parse_and_serialize("(?<name>"));
        REQUIRE_THROWS(parse_and_serialize("(?<>a)"));
        REQUIRE_THROWS(parse_and_serialize("(?<na-me>a)"));
        REQUIRE_THROWS(parse_and_serialize("(?<1a>a)"));
        REQUIRE_THROWS(parse_and_serialize("(?<a b>a)"));
        REQUIRE_THROWS(parse_and_serialize("(?<\\x41>a)"));
        REQUIRE_THROWS(parse_and_serialize("(?<\\u0031>a)"));  // escaped digit at the start
    }

    SECTION("Duplicate group name throws (early error)") {
        REQUIRE_THROWS(parse_and_serialize("(?<a>x)(?<a>y)"));
        REQUIRE_THROWS(parse_and_serialize("(?<a>x)|(?<a>y)"));
        REQUIRE_THROWS(parse_and_serialize("(?<ab>x)(?<a\\u0062>y)"));
    }

    SECTION("Unclosed group throws") {
        REQUIRE_THROWS(parse_and_serialize("(a"));
        REQUIRE_THROWS(parse_and_serialize("(?:a"));
        REQUIRE_THROWS(parse_and_serialize("(?<n>a"));
        REQUIRE_THROWS(parse_and_serialize("(?=a"));
        REQUIRE_THROWS(parse_and_serialize("((a)"));
    }

    SECTION("Unmatched closing paren throws") {
        REQUIRE_THROWS(parse_and_serialize(")"));
        REQUIRE_THROWS(parse_and_serialize("a)"));
        REQUIRE_THROWS(parse_and_serialize("(a))"));
    }

    // ---- 21.2.1 Assertion ----

    SECTION("Assertions and Lookarounds") {
        REQUIRE(parse_and_serialize("^a$") == "(SEQ (ASSERT '^') (LIT 'a') (ASSERT '$'))");
        REQUIRE(parse_and_serialize("\\b") == "(SEQ (ASSERT 'b'))");
        REQUIRE(parse_and_serialize("\\B") == "(SEQ (ASSERT 'B'))");
        REQUIRE(parse_and_serialize("(?=a)") == "(SEQ (ASSERT ?= (SEQ (LIT 'a'))))");
        REQUIRE(parse_and_serialize("(?!a)") == "(SEQ (ASSERT ?! (SEQ (LIT 'a'))))");
        REQUIRE(parse_and_serialize("(?<=a)") == "(SEQ (ASSERT ?<= (SEQ (LIT 'a'))))");
        REQUIRE(parse_and_serialize("(?<!a)") == "(SEQ (ASSERT ?<! (SEQ (LIT 'a'))))");
        REQUIRE(parse_and_serialize("(?=)") == "(SEQ (ASSERT ?= (SEQ)))");
        REQUIRE(parse_and_serialize("(?=a|b)") == "(SEQ (ASSERT ?= (DISJ (SEQ (LIT 'a')) (SEQ (LIT 'b')))))");
    }

    SECTION("Assertions in sequence") {
        REQUIRE(parse_and_serialize("a\\bb") == "(SEQ (LIT 'a') (ASSERT 'b') (LIT 'b'))");
        REQUIRE(parse_and_serialize("^\\ba$") == "(SEQ (ASSERT '^') (ASSERT 'b') (LIT 'a') (ASSERT '$'))");
        REQUIRE(parse_and_serialize("(?=a+)") == "(SEQ (ASSERT ?= (SEQ (QUANT {1,inf} (LIT 'a')))))");
        REQUIRE(parse_and_serialize("(?<![0-9])a") == "(SEQ (ASSERT ?<! (SEQ (CLASS (RANGE '0' '9')))) (LIT 'a'))");
        REQUIRE(parse_and_serialize("(?=a)(b)") == "(SEQ (ASSERT ?= (SEQ (LIT 'a'))) (GROUP #1 (SEQ (LIT 'b'))))");
    }

    // ---- 21.2.1 CharacterEscape ----

    SECTION("Control escapes") {
        // Table 54
        REQUIRE(parse_and_serialize("\\t\\n\\v\\f\\r") ==
                "(SEQ (LIT U+0009) (LIT U+000A) (LIT U+000B) (LIT U+000C) (LIT U+000D))");
        // c ControlLetter -- the code point modulo 32
        REQUIRE(parse_and_serialize("\\cA\\cz\\cJ") == "(SEQ (LIT U+0001) (LIT U+001A) (LIT U+000A))");
    }

    SECTION("Null and hexadecimal escapes") {
        REQUIRE(parse_and_serialize("\\0") == "(SEQ (LIT U+0000))");
        REQUIRE(parse_and_serialize("\\0a") == "(SEQ (LIT U+0000) (LIT 'a'))");
        REQUIRE(parse_and_serialize("\\x41\\x7a\\xFF") == "(SEQ (LIT 'A') (LIT 'z') (LIT U+00FF))");
    }

    SECTION("Unicode escapes") {
        REQUIRE(parse_and_serialize("\\u0041") == "(SEQ (LIT 'A'))");
        // an escaped syntax character is a literal, not a quantifier
        REQUIRE(parse_and_serialize("a\\u002A") == "(SEQ (LIT 'a') (LIT '*'))");
        REQUIRE(parse_and_serialize("\\u{41}") == "(SEQ (LIT 'A'))");
        REQUIRE(parse_and_serialize("\\u{0000000041}") == "(SEQ (LIT 'A'))");
        REQUIRE(parse_and_serialize("\\u{1F600}") == "(SEQ (LIT U+1F600))");
        REQUIRE(parse_and_serialize("\\u{10FFFF}") == "(SEQ (LIT U+10FFFF))");
    }

    SECTION("Unicode escapes with surrogates") {
        // u LeadSurrogate \u TrailSurrogate is a single code point (UTF16DecodeSurrogatePair)
        REQUIRE(parse_and_serialize("\\uD83D\\uDE00") == "(SEQ (LIT U+1F600))");
        REQUIRE(parse_and_serialize("\\uD83D\\uDE00+") == "(SEQ (QUANT {1,inf} (LIT U+1F600)))");
        // lone surrogates are characters on their own
        REQUIRE(parse_and_serialize("\\uD83D") == "(SEQ (LIT U+D83D))");
        REQUIRE(parse_and_serialize("\\uDE00") == "(SEQ (LIT U+DE00))");
        REQUIRE(parse_and_serialize("\\uD83Dx") == "(SEQ (LIT U+D83D) (LIT 'x'))");
        REQUIRE(parse_and_serialize("\\uD83D\\u0041") == "(SEQ (LIT U+D83D) (LIT 'A'))");
        REQUIRE(parse_and_serialize("\\uDE00\\uD83D") == "(SEQ (LIT U+DE00) (LIT U+D83D))");
        REQUIRE(parse_and_serialize("\\uD83D\\uD83D\\uDE00") == "(SEQ (LIT U+D83D) (LIT U+1F600))");
    }

    SECTION("Identity escapes") {
        // IdentityEscape[+U] :: SyntaxCharacter | /
        REQUIRE(parse_and_serialize("\\^\\$\\\\\\.\\*\\+\\?\\(\\)\\[\\]\\{\\}\\|\\/") ==
                "(SEQ (LIT '^') (LIT '$') (LIT '\\') (LIT '.') (LIT '*') (LIT '+') (LIT '?') (LIT '(') (LIT ')') "
                "(LIT '[') (LIT ']') (LIT '{') (LIT '}') (LIT '|') (LIT '/'))");
    }

    SECTION("Invalid escapes throw") {
        REQUIRE_THROWS(parse_and_serialize("\\"));
        REQUIRE_THROWS(parse_and_serialize("a\\"));
        REQUIRE_THROWS(parse_and_serialize("\\c"));
        REQUIRE_THROWS(parse_and_serialize("\\c1"));
        REQUIRE_THROWS(parse_and_serialize("\\c_"));
        REQUIRE_THROWS(parse_and_serialize("\\x"));
        REQUIRE_THROWS(parse_and_serialize("\\x4"));
        REQUIRE_THROWS(parse_and_serialize("\\x4Z"));
        REQUIRE_THROWS(parse_and_serialize("\\u"));
        REQUIRE_THROWS(parse_and_serialize("\\u12"));
        REQUIRE_THROWS(parse_and_serialize("\\uG000"));
        REQUIRE_THROWS(parse_and_serialize("\\u{}"));
        REQUIRE_THROWS(parse_and_serialize("\\u{41"));
        REQUIRE_THROWS(parse_and_serialize("\\u{G}"));
        REQUIRE_THROWS(parse_and_serialize("\\u{110000}"));
        // no legacy octal escapes (B.1.4) for [+U]
        REQUIRE_THROWS(parse_and_serialize("\\00"));
        REQUIRE_THROWS(parse_and_serialize("\\01"));
        // identity escapes of other characters are not allowed for [+U]
        REQUIRE_THROWS(parse_and_serialize("\\a"));
        REQUIRE_THROWS(parse_and_serialize("\\z"));
        REQUIRE_THROWS(parse_and_serialize("\\-"));
        REQUIRE_THROWS(parse_and_serialize("\\_"));
        REQUIRE_THROWS(parse_and_serialize("\\ "));
        REQUIRE_THROWS(parse_and_serialize("\\e"));
        REQUIRE_THROWS(parse_and_serialize("\\i"));
    }

    // ---- 21.2.1 AtomEscape: backreferences ----

    SECTION("Backreferences") {
        REQUIRE(parse_and_serialize("(a)\\1") == "(SEQ (GROUP #1 (SEQ (LIT 'a'))) (BACKREF 1))");
        REQUIRE(parse_and_serialize("(a)(b)\\1\\2") ==
                "(SEQ (GROUP #1 (SEQ (LIT 'a'))) (GROUP #2 (SEQ (LIT 'b'))) (BACKREF 1) (BACKREF 2))");
        REQUIRE(parse_and_serialize("(?<name>a)\\k<name>") ==
                "(SEQ (GROUP #1 <name> (SEQ (LIT 'a'))) (BACKREF 1 <name>))");
        REQUIRE(parse_and_serialize("(?<word>[a-z]+)\\k<word>") ==
                "(SEQ (GROUP #1 <word> (SEQ (QUANT {1,inf} (CLASS (RANGE 'a' 'z'))))) (BACKREF 1 <word>))");
        // a named group can be referenced also by its number
        REQUIRE(parse_and_serialize("(?<x>a)\\1") == "(SEQ (GROUP #1 <x> (SEQ (LIT 'a'))) (BACKREF 1))");
    }

    SECTION("Multi-digit backreference") {
        REQUIRE(parse_and_serialize("(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\\10") ==
                "(SEQ (GROUP #1 (SEQ (LIT 'a'))) (GROUP #2 (SEQ (LIT 'b'))) (GROUP #3 (SEQ (LIT 'c'))) "
                "(GROUP #4 (SEQ (LIT 'd'))) (GROUP #5 (SEQ (LIT 'e'))) (GROUP #6 (SEQ (LIT 'f'))) "
                "(GROUP #7 (SEQ (LIT 'g'))) (GROUP #8 (SEQ (LIT 'h'))) (GROUP #9 (SEQ (LIT 'i'))) "
                "(GROUP #10 (SEQ (LIT 'j'))) (BACKREF 10))");
        // the whole DecimalEscape is one backreference -- there is no fallback to \1 followed by '0'
        REQUIRE_THROWS(parse_and_serialize("(a)\\10"));
    }

    SECTION("Forward backreferences") {
        // The early errors only check NcapturingParens of the whole pattern
        REQUIRE(parse_and_serialize("\\2(a)(b)") ==
                "(SEQ (BACKREF 2) (GROUP #1 (SEQ (LIT 'a'))) (GROUP #2 (SEQ (LIT 'b'))))");
        REQUIRE(parse_and_serialize("\\k<x>(?<x>a)") == "(SEQ (BACKREF 1 <x>) (GROUP #1 <x> (SEQ (LIT 'a'))))");
        REQUIRE(parse_and_serialize("(a\\1)") == "(SEQ (GROUP #1 (SEQ (LIT 'a') (BACKREF 1))))");
    }

    SECTION("Invalid backreferences throw (early errors)") {
        REQUIRE_THROWS(parse_and_serialize("\\1"));
        REQUIRE_THROWS(parse_and_serialize("(a)\\2"));
        REQUIRE_THROWS(parse_and_serialize("\\8"));
        REQUIRE_THROWS(parse_and_serialize("\\9"));
        REQUIRE_THROWS(parse_and_serialize("(?:a)\\1"));
        REQUIRE_THROWS(parse_and_serialize("\\99999999999999999999"));
        REQUIRE_THROWS(parse_and_serialize("\\k<y>"));
        REQUIRE_THROWS(parse_and_serialize("(?<x>a)\\k<y>"));
        REQUIRE_THROWS(parse_and_serialize("(?<x>a)\\k<>"));
        REQUIRE_THROWS(parse_and_serialize("(?<x>a)\\k<x"));
        REQUIRE_THROWS(parse_and_serialize("(?<x>a)\\k"));
        REQUIRE_THROWS(parse_and_serialize("(?<x>a)\\kx"));
    }

    // ---- 21.2.1 CharacterClassEscape ----

    SECTION("Character class escapes") {
        REQUIRE(parse_and_serialize("\\d") == "(SEQ (CLASS (CHAR_CLASS 'd')))");
        REQUIRE(parse_and_serialize("\\D") == "(SEQ (CLASS (CHAR_CLASS 'D')))");
        REQUIRE(parse_and_serialize("\\s") == "(SEQ (CLASS (CHAR_CLASS 's')))");
        REQUIRE(parse_and_serialize("\\S") == "(SEQ (CLASS (CHAR_CLASS 'S')))");
        REQUIRE(parse_and_serialize("\\w") == "(SEQ (CLASS (CHAR_CLASS 'w')))");
        REQUIRE(parse_and_serialize("\\W") == "(SEQ (CLASS (CHAR_CLASS 'W')))");
        REQUIRE(parse_and_serialize("\\d+") == "(SEQ (QUANT {1,inf} (CLASS (CHAR_CLASS 'd'))))");
    }

    SECTION("Unicode property escapes") {
        // Lone General_Category values (Table 57) and binary properties (Table 56)
        REQUIRE(parse_and_serialize("\\p{Lu}") == "(SEQ (CLASS (CHAR_CLASS 'p' {Lu})))");
        REQUIRE(parse_and_serialize("\\P{Uppercase_Letter}") == "(SEQ (CLASS (CHAR_CLASS 'P' {Uppercase_Letter})))");
        REQUIRE(parse_and_serialize("\\p{ASCII}") == "(SEQ (CLASS (CHAR_CLASS 'p' {ASCII})))");
        REQUIRE(parse_and_serialize("\\p{Any}") == "(SEQ (CLASS (CHAR_CLASS 'p' {Any})))");
        // Name=Value forms (Table 55 with Table 57 or Table 58)
        REQUIRE(parse_and_serialize("\\p{gc=Lu}") == "(SEQ (CLASS (CHAR_CLASS 'p' {gc=Lu})))");
        REQUIRE(parse_and_serialize("\\p{General_Category=Decimal_Number}") ==
                "(SEQ (CLASS (CHAR_CLASS 'p' {General_Category=Decimal_Number})))");
        REQUIRE(parse_and_serialize("\\p{Script=Greek}") == "(SEQ (CLASS (CHAR_CLASS 'p' {Script=Greek})))");
        REQUIRE(parse_and_serialize("\\P{scx=Latn}") == "(SEQ (CLASS (CHAR_CLASS 'P' {scx=Latn})))");
        REQUIRE(parse_and_serialize("[\\p{L}\\d]") == "(SEQ (CLASS (CHAR_CLASS 'p' {L}) (CHAR_CLASS 'd')))");
    }

    SECTION("Invalid Unicode property escapes throw") {
        REQUIRE_THROWS(parse_and_serialize("\\p"));
        REQUIRE_THROWS(parse_and_serialize("\\pL"));
        REQUIRE_THROWS(parse_and_serialize("\\p{}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{Lu"));
        REQUIRE_THROWS(parse_and_serialize("\\p{Foo}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{lu}"));  // names are case-sensitive
        REQUIRE_THROWS(parse_and_serialize("\\p{Script}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{General_Category}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{Script=Foo}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{Foo=Lu}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{gc=Greek}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{sc=Lu}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{ASCII=Yes}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{gc=}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{=Lu}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{g1=Lu}"));
        REQUIRE_THROWS(parse_and_serialize("\\p{L u}"));
    }

    // ---- 21.2.1 CharacterClass ----

    SECTION("Character Classes") {
        REQUIRE(parse_and_serialize("[a]") == "(SEQ (CLASS (LIT 'a')))");
        REQUIRE(parse_and_serialize("[^a]") == "(SEQ (CLASS ^ (LIT 'a')))");
        REQUIRE(parse_and_serialize("[a-z]") == "(SEQ (CLASS (RANGE 'a' 'z')))");
        REQUIRE(parse_and_serialize("[a-a]") == "(SEQ (CLASS (LIT 'a')))");
        REQUIRE(parse_and_serialize("[a-zA-Z]") == "(SEQ (CLASS (RANGE 'a' 'z') (RANGE 'A' 'Z')))");
        REQUIRE(parse_and_serialize("[a-zA-Z0-9]") == "(SEQ (CLASS (RANGE 'a' 'z') (RANGE 'A' 'Z') (RANGE '0' '9')))");
        REQUIRE(parse_and_serialize("[_a-z]") == "(SEQ (CLASS (LIT '_') (RANGE 'a' 'z')))");
        REQUIRE(parse_and_serialize("[\\d\\s]") == "(SEQ (CLASS (CHAR_CLASS 'd') (CHAR_CLASS 's')))");
        REQUIRE(parse_and_serialize("[^a-z\\d_]") == "(SEQ (CLASS ^ (RANGE 'a' 'z') (CHAR_CLASS 'd') (LIT '_')))");
        REQUIRE(parse_and_serialize("[^\\w0-9]") == "(SEQ (CLASS ^ (CHAR_CLASS 'w') (RANGE '0' '9')))");
        REQUIRE(parse_and_serialize("[a-z]+") == "(SEQ (QUANT {1,inf} (CLASS (RANGE 'a' 'z'))))");
    }

    SECTION("Empty character classes") {
        // ClassRanges :: [empty]
        REQUIRE(parse_and_serialize("[]") == "(SEQ (CLASS))");
        REQUIRE(parse_and_serialize("[^]") == "(SEQ (CLASS ^))");
        REQUIRE(parse_and_serialize("a[]b") == "(SEQ (LIT 'a') (CLASS) (LIT 'b'))");
    }

    SECTION("Dashes in character classes") {
        REQUIRE(parse_and_serialize("[-]") == "(SEQ (CLASS (LIT '-')))");
        REQUIRE(parse_and_serialize("[a-]") == "(SEQ (CLASS (LIT 'a') (LIT '-')))");
        REQUIRE(parse_and_serialize("[-a]") == "(SEQ (CLASS (LIT '-') (LIT 'a')))");
        REQUIRE(parse_and_serialize("[--]") == "(SEQ (CLASS (LIT '-') (LIT '-')))");
        REQUIRE(parse_and_serialize("[---]") == "(SEQ (CLASS (LIT '-')))");
        REQUIRE(parse_and_serialize("[--/]") == "(SEQ (CLASS (RANGE '-' '/')))");
        REQUIRE(parse_and_serialize("[a-c-e]") == "(SEQ (CLASS (RANGE 'a' 'c') (LIT '-') (LIT 'e')))");
        REQUIRE(parse_and_serialize("[\\d-]") == "(SEQ (CLASS (CHAR_CLASS 'd') (LIT '-')))");
        REQUIRE(parse_and_serialize("[-\\d]") == "(SEQ (CLASS (LIT '-') (CHAR_CLASS 'd')))");
        REQUIRE(parse_and_serialize("[^-]") == "(SEQ (CLASS ^ (LIT '-')))");
    }

    SECTION("Escapes in character classes") {
        // ClassEscape[+U] :: b | - | CharacterClassEscape | CharacterEscape
        REQUIRE(parse_and_serialize("[\\b]") == "(SEQ (CLASS (LIT U+0008)))");
        REQUIRE(parse_and_serialize("[\\-]") == "(SEQ (CLASS (LIT '-')))");
        REQUIRE(parse_and_serialize("[a\\-z]") == "(SEQ (CLASS (LIT 'a') (LIT '-') (LIT 'z')))");
        REQUIRE(parse_and_serialize("[\\]]") == "(SEQ (CLASS (LIT ']')))");
        REQUIRE(parse_and_serialize("[\\^]") == "(SEQ (CLASS (LIT '^')))");
        REQUIRE(parse_and_serialize("[\\\\]") == "(SEQ (CLASS (LIT '\\')))");
        REQUIRE(parse_and_serialize("[\\0]") == "(SEQ (CLASS (LIT U+0000)))");
        REQUIRE(parse_and_serialize("[\\t\\cA\\x41]") == "(SEQ (CLASS (LIT U+0009) (LIT U+0001) (LIT 'A')))");
        REQUIRE(parse_and_serialize("[\\u0041-\\u005A]") == "(SEQ (CLASS (RANGE 'A' 'Z')))");
        REQUIRE(parse_and_serialize("[\\u{1F600}-\\u{1F64F}]") == "(SEQ (CLASS (RANGE U+1F600 U+1F64F)))");
        REQUIRE(parse_and_serialize("[\\uD83D\\uDE00]") == "(SEQ (CLASS (LIT U+1F600)))");
    }

    SECTION("Unescaped characters in character classes") {
        // only '\', ']' and '-' have a special meaning in a class
        REQUIRE(parse_and_serialize("[.*+?(){}|^$/]") ==
                "(SEQ (CLASS (LIT '.') (LIT '*') (LIT '+') (LIT '?') (LIT '(') (LIT ')') (LIT '{') (LIT '}') "
                "(LIT '|') (LIT '^') (LIT '$') (LIT '/')))");
        REQUIRE(parse_and_serialize("[[]") == "(SEQ (CLASS (LIT '[')))");
    }

    SECTION("Invalid character classes throw") {
        REQUIRE_THROWS(parse_and_serialize("["));
        REQUIRE_THROWS(parse_and_serialize("[a"));
        REQUIRE_THROWS(parse_and_serialize("[a-"));
        REQUIRE_THROWS(parse_and_serialize("[^"));
        REQUIRE_THROWS(parse_and_serialize("[\\"));
        REQUIRE_THROWS(parse_and_serialize("[\\B]"));
        REQUIRE_THROWS(parse_and_serialize("[\\8]"));
        REQUIRE_THROWS(parse_and_serialize("[\\1]"));
        REQUIRE_THROWS(parse_and_serialize("[\\00]"));
        REQUIRE_THROWS(parse_and_serialize("[\\k]"));
        REQUIRE_THROWS(parse_and_serialize("[\\c1]"));
        REQUIRE_THROWS(parse_and_serialize("[\\a]"));
    }

    SECTION("Character range out of order throws (early error)") {
        REQUIRE_THROWS(parse_and_serialize("[z-a]"));
        REQUIRE_THROWS(parse_and_serialize("[9-0]"));
        REQUIRE_THROWS(parse_and_serialize("[a--]"));
        REQUIRE_THROWS(parse_and_serialize("[\\u{1F64F}-\\u{1F600}]"));
    }

    SECTION("Character class as range bound throws (early error)") {
        REQUIRE_THROWS(parse_and_serialize("[\\w-z]"));
        REQUIRE_THROWS(parse_and_serialize("[a-\\d]"));
        REQUIRE_THROWS(parse_and_serialize("[\\s-\\S]"));
        REQUIRE_THROWS(parse_and_serialize("[\\p{L}-z]"));
    }

    SECTION("Crazy character class") {
        REQUIRE(parse_and_serialize(R"([^\]\--/a-z^\b--\0\cZ])") == "(SEQ (CLASS ^"
                                                                    " (LIT ']')"
                                                                    " (RANGE '-' '/')"
                                                                    " (RANGE 'a' 'z')"
                                                                    " (LIT '^')"
                                                                    " (RANGE U+0008 '-')"
                                                                    " (LIT U+0000)"
                                                                    " (LIT U+001A)"
                                                                    "))");
        // octal escapes are not allowed for [+U]
        REQUIRE_THROWS(parse_and_serialize(R"([^\]\--/a-z^\b--\0-\37\cZ])"));
    }

    // ---- Realistic patterns ----

    SECTION("Simple email-like pattern") {
        REQUIRE(parse_and_serialize("[a-z]+@[a-z]+\\.[a-z]+") == "(SEQ"
                                                                 " (QUANT {1,inf} (CLASS (RANGE 'a' 'z')))"
                                                                 " (LIT '@')"
                                                                 " (QUANT {1,inf} (CLASS (RANGE 'a' 'z')))"
                                                                 " (LIT '.')"
                                                                 " (QUANT {1,inf} (CLASS (RANGE 'a' 'z')))"
                                                                 ")");
    }

    SECTION("IP address octet pattern") {
        REQUIRE(parse_and_serialize("(25[0-5]|2[0-4][0-9]|[01]?[0-9]{1,2})") ==
                "(SEQ"
                " (GROUP #1 (DISJ"
                " (SEQ (LIT '2') (LIT '5') (CLASS (RANGE '0' '5')))"
                " (SEQ (LIT '2') (CLASS (RANGE '0' '4')) (CLASS (RANGE '0' '9')))"
                " (SEQ"
                " (QUANT {0,1} (CLASS (LIT '0') (LIT '1')))"
                " (QUANT {1,2} (CLASS (RANGE '0' '9')))"
                ")"
                "))"
                ")");
    }

    SECTION("Hex color pattern") {
        REQUIRE(parse_and_serialize("#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})") ==
                "(SEQ"
                " (LIT '#')"
                " (GROUP #1 (DISJ"
                " (SEQ (QUANT {3,3} (CLASS (RANGE '0' '9') (RANGE 'a' 'f') (RANGE 'A' 'F'))))"
                " (SEQ (QUANT {6,6} (CLASS (RANGE '0' '9') (RANGE 'a' 'f') (RANGE 'A' 'F'))))"
                "))"
                ")");
    }

    SECTION("Date pattern with named groups") {
        REQUIRE(parse_and_serialize("(?<year>\\d{4})-(?<month>\\d{2})-(?<day>\\d{2})") ==
                "(SEQ"
                " (GROUP #1 <year> (SEQ (QUANT {4,4} (CLASS (CHAR_CLASS 'd')))))"
                " (LIT '-')"
                " (GROUP #2 <month> (SEQ (QUANT {2,2} (CLASS (CHAR_CLASS 'd')))))"
                " (LIT '-')"
                " (GROUP #3 <day> (SEQ (QUANT {2,2} (CLASS (CHAR_CLASS 'd')))))"
                ")");
    }

    SECTION("URL path segment with lookarounds") {
        REQUIRE(parse_and_serialize("(?<=/)([a-z0-9\\-]+)(?=/)") ==
                "(SEQ"
                " (ASSERT ?<= (SEQ (LIT '/')))"
                " (GROUP #1 (SEQ (QUANT {1,inf} (CLASS (RANGE 'a' 'z') (RANGE '0' '9') (LIT '-')))))"
                " (ASSERT ?= (SEQ (LIT '/')))"
                ")");
    }

    SECTION("Number of capturing groups") {
        const zstring pattern = raw("(a)(?:b)(?<c>c)((d))(?=(e))");
        ECMAParser parser(pattern);
        parser.parse();
        REQUIRE(parser.num_capturing_groups() == 5);
    }
}

// =====================================================================
// RCG SERIALIZATION HELPERS
// =====================================================================

namespace smt::noodler::ecma::test {

    // Converts a Z3 app_ref to a normalized string (collapses whitespace, removes newlines).
    std::string app_to_string(const app_ref& app, ast_manager& m) {
        std::stringstream ss;
        ss << mk_pp(app.get(), m);
        std::string res = ss.str();
        std::erase(res, '\n');
        res.erase(std::ranges::unique(res,
                                      [](const char a, const char b) {
                                          return a == ' ' && b == ' ';
                                      })
                      .begin(),
                  res.end());
        return res;
    }

    // Normalizes a raw pretty-printer string (same as above but takes std::string).
    static std::string normalize_pp(std::string s) {
        std::erase(s, '\n');
        s.erase(std::ranges::unique(s,
                                    [](char a, char b) {
                                        return a == ' ' && b == ' ';
                                    })
                    .begin(),
                s.end());
        return s;
    }

    // Builds the group-start / group-end marker string for a single edge, e.g. " STARTS {1,2} ENDS {1}".
    static std::string serialize_edge_markers(const RegexConstraintGraph& graph, EdgeID eid) {
        std::string res;
        if (graph.group_starts.contains(eid) && !graph.group_starts.at(eid).empty()) {
            res += " STARTS {";
            for (const uint32_t gid : graph.group_starts.at(eid)) {
                res += std::to_string(gid) + ",";
            }
            res.pop_back();
            res += "}";
        }
        if (graph.group_ends.contains(eid) && !graph.group_ends.at(eid).empty()) {
            res += " ENDS {";
            for (const uint32_t gid : graph.group_ends.at(eid)) {
                res += std::to_string(gid) + ",";
            }
            res.pop_back();
            res += "}";
        }
        return res;
    }

    // Forward declaration – serialize_payload and serialize_fragment are mutually recursive
    // (a lookaround can contain a non-regular GraphFragment that itself has edges).
    std::string serialize_fragment(const RegexConstraintGraph& graph, const GraphFragment& frag, ast_manager& m);

    // Serializes a single RCG edge payload.
    // Takes the full graph so it can recursively serialize non-regular lookaround fragments.
    std::string serialize_payload(const RegexConstraintGraph& graph, const RCGEdgePayload& payload, ast_manager& m) {
        if (std::holds_alternative<std::monostate>(payload)) {
            return "EPSILON";
        }

        if (std::holds_alternative<MatchEdge>(payload)) {
            const MatchEdge& match = std::get<MatchEdge>(payload);
            std::stringstream ss;
            ss << mk_pp(match.regex.get(), m);
            return "MATCH " + normalize_pp(ss.str());
        }

        if (std::holds_alternative<AssertionEdge>(payload)) {
            const AssertionEdge& assertion = std::get<AssertionEdge>(payload);

            if (std::holds_alternative<Anchor>(assertion.payload)) {
                const uint32_t anchor_char = std::get<Anchor>(assertion.payload);
                return std::string("ANCHOR '") + static_cast<char>(anchor_char) + "'";
            }

            const Lookaround& la = std::get<Lookaround>(assertion.payload);

            std::string type_str;
            if (la.direction == LookaroundDirection::FORWARD) {
                type_str = la.is_positive ? "?=" : "?!";
            } else {
                type_str = la.is_positive ? "?<=" : "?<!";
            }

            std::string inner_str;
            if (std::holds_alternative<app_ref>(la.subregex)) {
                // Regular lookaround: serialize the Z3 regex expression directly.
                std::stringstream ss;
                ss << mk_pp(std::get<app_ref>(la.subregex).get(), m);
                inner_str = normalize_pp(ss.str());
            } else {
                // Non-regular lookaround: recursively serialize the embedded graph fragment.
                inner_str = serialize_fragment(graph, std::get<GraphFragment>(la.subregex), m);
            }

            return "LOOKAROUND " + type_str + " " + inner_str;
        }

        if (std::holds_alternative<BackrefEdge>(payload)) {
            return "BACKREF " + std::to_string(std::get<BackrefEdge>(payload).backref_id);
        }

        return "UNKNOWN";
    }

    // BFS over the graph starting from start_vertex; appends edge IDs (in BFS order) to ordered_edges.
    void rcg_bfs(const RegexConstraintGraph& graph, VertexID start_vertex, std::vector<bool>& visited_edges,
                 std::vector<EdgeID>& ordered_edges) {
        if (start_vertex == UNKNOWN_VERTEX || start_vertex >= graph.vertices.size()) {
            return;
        }

        std::queue<VertexID> q;
        std::vector<bool> visited_vertices(graph.vertices.size(), false);
        q.push(start_vertex);
        visited_vertices[start_vertex] = true;

        while (!q.empty()) {
            const VertexID curr = q.front();
            q.pop();

            for (EdgeID eid : graph.vertices[curr].outgoing_edges) {
                if (!visited_edges[eid]) {
                    visited_edges[eid] = true;
                    ordered_edges.push_back(eid);
                }
                const VertexID target = graph.edges[eid].target;
                if (!visited_vertices[target]) {
                    visited_vertices[target] = true;
                    q.push(target);
                }
            }
        }
    }

    // Serializes the graph fragment reachable from frag.v_in up to (but not beyond) frag.v_out.
    // Used to serialize non-regular lookaround subgraphs stored inline in edge payloads.
    //
    // The BFS stops exploring outgoing edges of frag.v_out, which correctly bounds the traversal
    // to the inner sub-graph (frag.v_out has no outgoing edges in the outer graph because it was
    // created as an isolated end-vertex of the inner pattern before being moved into the payload).
    std::string serialize_fragment(const RegexConstraintGraph& graph, const GraphFragment& frag, ast_manager& m) {
        std::queue<VertexID> q;
        std::vector<bool> visited_vertices(graph.vertices.size(), false);
        std::vector<bool> visited_edges(graph.edges.size(), false);
        std::vector<EdgeID> ordered_edges;

        q.push(frag.v_in);
        visited_vertices[frag.v_in] = true;

        while (!q.empty()) {
            const VertexID curr = q.front();
            q.pop();

            // Do not follow edges out of the fragment's output vertex.
            if (curr == frag.v_out) {
                continue;
            }

            for (EdgeID eid : graph.vertices[curr].outgoing_edges) {
                if (!visited_edges[eid]) {
                    visited_edges[eid] = true;
                    ordered_edges.push_back(eid);
                }
                const VertexID target = graph.edges[eid].target;
                if (!visited_vertices[target]) {
                    visited_vertices[target] = true;
                    q.push(target);
                }
            }
        }

        std::string res = "(FRAGMENT";
        for (EdgeID eid : ordered_edges) {
            const RCGEdge& edge = graph.edges[eid];
            res += " (EDGE *->* [";
            res += serialize_payload(graph, edge.payload, m) + "]";
            res += serialize_edge_markers(graph, eid);
            res += ")";
        }
        res += ")";
        return res;
    }

    // Full RCG serialization: BFS from start_vertex, emit one token per edge.
    std::string serialize_rcg(const RegexConstraintGraph& graph, ast_manager& m) {
        std::string res = "(RCG";

        if (graph.start_vertex == UNKNOWN_VERTEX) {
            return "(RCG INVALID_START_VERTEX)";
        }

        std::vector<bool> visited_edges(graph.edges.size(), false);
        std::vector<EdgeID> ordered_edges;
        rcg_bfs(graph, graph.start_vertex, visited_edges, ordered_edges);

        for (EdgeID eid : ordered_edges) {
            const RCGEdge& edge = graph.edges[eid];
            res += " (EDGE *->* [";
            res += serialize_payload(graph, edge.payload, m) + "]";
            res += serialize_edge_markers(graph, eid);
            res += ")";
        }
        res += ")";
        return res;
    }

    std::string build_and_serialize_rcg(const std::string& regex, ast_manager& m) {
        theory_str_noodler_params params;
        params.m_ecma_engine_semantics = true;
        RegexConstraintBuilder builder(m, raw(regex), params);
        const RegexConstraintGraph& rcg = builder.build_rcg();
        return serialize_rcg(rcg, m);
    }

}  // namespace smt::noodler::ecma::test

// =====================================================================
// RCG GENERATION FROM AST – UNIT TESTS
// =====================================================================

TEST_CASE("ECMA Regex RCG generation from AST", "[noodler][ecma]") {
    using Catch::Matchers::ContainsSubstring;
    using namespace smt::noodler::ecma;
    using namespace smt::noodler::ecma::test;

    ast_manager m;
    reg_decl_plugins(m);
    seq_util util_s(m);
    RegexConstraintGraph graph;

    auto mk_char = [&](const unsigned ch) {
        return util_s.re.mk_to_re(util_s.str.mk_string(zstring(ch)));
    };

    SECTION("ASTNodeCharacter returns regular app_ref") {
        ASTNodeCharacter character_node('x');

        RegexComponent comp = character_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<app_ref>(comp));
        REQUIRE(graph.vertices.empty());

        app_ref z3_regex = std::get<app_ref>(comp);
        REQUIRE(app_to_string(z3_regex, m) == "(str.to_re \"x\")");
    }

    SECTION("ASTNodeCharacter above the maximal character throws") {
        ASTNodeCharacter character_node(0x10FFFF);
        REQUIRE_THROWS(character_node.get_subgraph(graph, util_s, m));
    }

    SECTION("ASTNodeQuantified wraps the atom in a quantifier") {
        ASTNodeQuantified quant_node({0, UNBOUNDED, true}, std::make_unique<ASTNodeCharacter>('x'));

        RegexComponent comp = quant_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<app_ref>(comp));
        REQUIRE(graph.vertices.empty());

        app_ref z3_regex = std::get<app_ref>(comp);
        REQUIRE(app_to_string(z3_regex, m) == "(re.* (str.to_re \"x\"))");
    }

    SECTION("Lazy ASTNodeQuantified creates the same regex as the greedy one") {
        ASTNodeQuantified lazy_node({1, UNBOUNDED, false}, std::make_unique<ASTNodeCharacter>('x'));
        app_ref z3_regex = std::get<app_ref>(lazy_node.get_subgraph(graph, util_s, m));
        REQUIRE(app_to_string(z3_regex, m) == "(re.+ (str.to_re \"x\"))");
    }

    SECTION("ASTNodeBackreference mutates graph and returns GraphFragment") {
        ASTNodeBackreference backref_node(1);

        RegexComponent comp = backref_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<GraphFragment>(comp));

        GraphFragment frag = std::get<GraphFragment>(comp);
        REQUIRE(graph.vertices.size() == 2);
        REQUIRE(graph.edges.size() == 1);
        REQUIRE(frag.v_in == 0);
        REQUIRE(frag.v_out == 1);
        REQUIRE(frag.edges_pointing_to_vout.size() == 1);

        const RCGEdge& edge = graph.edges[frag.edges_pointing_to_vout[0]];
        REQUIRE(std::holds_alternative<BackrefEdge>(edge.payload));
        REQUIRE(std::get<BackrefEdge>(edge.payload).backref_id == 1);
    }

    SECTION("ASTNodeGroup tags edges correctly") {
        ASTNodeGroup group_node(42, std::make_unique<ASTNodeCharacter>('a'));

        RegexComponent comp = group_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<GraphFragment>(comp));

        GraphFragment frag = std::get<GraphFragment>(comp);
        EdgeID the_edge_id = frag.edges_pointing_to_vout[0];

        REQUIRE(graph.group_starts.count(the_edge_id) > 0);
        REQUIRE(graph.group_starts.at(the_edge_id).size() == 1);
        REQUIRE(graph.group_starts.at(the_edge_id)[0] == 42);

        REQUIRE(graph.group_ends.count(the_edge_id) > 0);
        REQUIRE(graph.group_ends.at(the_edge_id).size() == 1);
        REQUIRE(graph.group_ends.at(the_edge_id)[0] == 42);
    }

    SECTION("Non-capturing ASTNodeGroup is transparent") {
        ASTNodeGroup group_node(std::make_unique<ASTNodeCharacter>('a'));

        RegexComponent comp = group_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<app_ref>(comp));
        REQUIRE(graph.vertices.empty());
    }

    SECTION("Unreferenced named ASTNodeGroup is stripped") {
        const zstring name("name");
        ASTNodeGroup group_node(1, std::make_unique<ASTNodeCharacter>('a'), name);
        group_node.strip_unreferenced_captures({});
        REQUIRE(!group_node.is_capturing());
        REQUIRE(std::holds_alternative<app_ref>(group_node.get_subgraph(graph, util_s, m)));
    }

    SECTION("ASTNodeDot matches all characters except line terminators") {
        ASTNodeDot dot_node;
        RegexComponent comp = dot_node.get_subgraph(graph, util_s, m);

        REQUIRE(std::holds_alternative<app_ref>(comp));
        REQUIRE(graph.vertices.empty());

        // Sigma \ LineTerminator (Table 33)
        app* line_terminators = util_s.re.mk_union(
            util_s.re.mk_union(util_s.re.mk_union(mk_char(0x000A), mk_char(0x000D)), mk_char(0x2028)), mk_char(0x2029));
        const app_ref expected(
            util_s.re.mk_inter(util_s.re.mk_full_char(nullptr), util_s.re.mk_complement(line_terminators)), m);
        REQUIRE(app_to_string(std::get<app_ref>(comp), m) == app_to_string(expected, m));
    }

    SECTION("ASTNodeAssertion anchor ^ creates assertion edge") {
        ASTNodeAssertion assert_node(AssertionKind::START);

        RegexComponent comp = assert_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<GraphFragment>(comp));

        GraphFragment frag = std::get<GraphFragment>(comp);
        REQUIRE(graph.vertices.size() == 2);
        REQUIRE(graph.edges.size() == 1);

        const RCGEdge& edge = graph.edges[frag.edges_pointing_to_vout[0]];
        REQUIRE(std::holds_alternative<AssertionEdge>(edge.payload));
        const AssertionEdge& ae = std::get<AssertionEdge>(edge.payload);
        REQUIRE(std::holds_alternative<Anchor>(ae.payload));
        REQUIRE(std::get<Anchor>(ae.payload) == '^');
    }

    SECTION("ASTNodeAssertion anchor $ creates assertion edge") {
        ASTNodeAssertion assert_node(AssertionKind::END);

        GraphFragment frag = std::get<GraphFragment>(assert_node.get_subgraph(graph, util_s, m));
        const AssertionEdge& ae = std::get<AssertionEdge>(graph.edges[frag.edges_pointing_to_vout[0]].payload);
        REQUIRE(std::get<Anchor>(ae.payload) == '$');
    }

    SECTION("ASTNodeQuantified throws on unbounded non-regular subregex") {
        // Kleene star/plus over backreferences would create a dynamic number of string variables.
        ASTNodeQuantified quant_node({0, UNBOUNDED, true}, std::make_unique<ASTNodeBackreference>(1));
        REQUIRE_THROWS(quant_node.get_subgraph(graph, util_s, m));
    }

    SECTION("ASTNodeQuantified with finite bounds on non-regular subregex creates fragment") {
        // {n,m} quantifier over a backreference expands into m copies of the sub-graph.
        ASTNodeQuantified quant_node({1, 2, true}, std::make_unique<ASTNodeBackreference>(1));

        RegexComponent comp = quant_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<GraphFragment>(comp));
        // Two copies of the backref fragment plus epsilon alternations are created.
        REQUIRE(!graph.vertices.empty());
        REQUIRE(!graph.edges.empty());
    }

    SECTION("ASTNodeAlternative merges two regular characters into one app_ref") {
        ASTNodeAlternative concat_node;
        concat_node.add_term(std::make_unique<ASTNodeCharacter>('a'));
        concat_node.add_term(std::make_unique<ASTNodeCharacter>('b'));

        RegexComponent comp = concat_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<app_ref>(comp));
        REQUIRE(graph.vertices.empty());

        app_ref z3_regex = std::get<app_ref>(comp);
        REQUIRE(app_to_string(z3_regex, m) == "(str.to_re \"ab\")");
    }

    SECTION("ASTNodeAlternative with a character above the maximal character throws") {
        ASTNodeAlternative concat_node;
        concat_node.add_term(std::make_unique<ASTNodeCharacter>('a'));
        concat_node.add_term(std::make_unique<ASTNodeCharacter>(0x10FFFF));
        REQUIRE_THROWS(concat_node.get_subgraph(graph, util_s, m));
    }

    SECTION("ASTNodeAlternative creates graph fragment when mixing character and backref") {
        ASTNodeAlternative concat_node;
        concat_node.add_term(std::make_unique<ASTNodeCharacter>('a'));
        concat_node.add_term(std::make_unique<ASTNodeBackreference>(1));

        RegexComponent comp = concat_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<GraphFragment>(comp));

        // Backref creates {v0,v1,e_br}, character is lifted to {v2,v3,e_match}.
        // chain_fragments orphans v3 (retargets e_match to v0) → 4 vertices, 2 edges.
        REQUIRE(graph.vertices.size() == 4);
        REQUIRE(graph.edges.size() == 2);
    }

    SECTION("ASTNodeAlternative chains adjacent regular subregexes into one") {
        // "ab\1cd" – 'ab' and 'cd' are each merged into a single MATCH edge.
        ASTNodeAlternative concat_node;
        concat_node.add_term(std::make_unique<ASTNodeCharacter>('a'));
        concat_node.add_term(std::make_unique<ASTNodeCharacter>('b'));
        concat_node.add_term(std::make_unique<ASTNodeBackreference>(1));
        concat_node.add_term(std::make_unique<ASTNodeCharacter>('c'));
        concat_node.add_term(std::make_unique<ASTNodeCharacter>('d'));

        RegexComponent comp = concat_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<GraphFragment>(comp));

        // Expected: MATCH('ab'), BACKREF(1), MATCH('cd') — 3 edges total.
        REQUIRE(graph.edges.size() == 3);

        bool has_match_ab = false, has_match_cd = false, has_backref = false;
        for (const RCGEdge& edge : graph.edges) {
            if (std::holds_alternative<MatchEdge>(edge.payload)) {
                std::string s = app_to_string(std::get<MatchEdge>(edge.payload).regex, m);
                if (s.find("str.to_re \"ab\"") != std::string::npos) {
                    has_match_ab = true;
                }
                if (s.find("str.to_re \"cd\"") != std::string::npos) {
                    has_match_cd = true;
                }
            } else if (std::holds_alternative<BackrefEdge>(edge.payload)) {
                has_backref = true;
            }
        }
        REQUIRE((has_match_ab && has_match_cd && has_backref));
    }

    // ---- Lookaround tests ----

    SECTION("ASTNodeAssertion: regular lookahead creates assertion edge with app_ref subregex") {
        ASTNodeAssertion assert_node(AssertionKind::LOOKAHEAD, std::make_unique<ASTNodeCharacter>('a'));

        RegexComponent comp = assert_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<GraphFragment>(comp));

        GraphFragment frag = std::get<GraphFragment>(comp);
        const RCGEdge& edge = graph.edges[frag.edges_pointing_to_vout[0]];
        REQUIRE(std::holds_alternative<AssertionEdge>(edge.payload));
        const Lookaround& la = std::get<Lookaround>(std::get<AssertionEdge>(edge.payload).payload);
        REQUIRE(std::holds_alternative<app_ref>(la.subregex));
        REQUIRE(la.is_positive == true);
        REQUIRE(la.direction == LookaroundDirection::FORWARD);
    }

    SECTION("ASTNodeAssertion: regular negative lookbehind") {
        ASTNodeAssertion assert_node(AssertionKind::NEG_LOOKBEHIND, std::make_unique<ASTNodeCharacter>('a'));

        GraphFragment frag = std::get<GraphFragment>(assert_node.get_subgraph(graph, util_s, m));
        const RCGEdge& edge = graph.edges[frag.edges_pointing_to_vout[0]];
        const Lookaround& la = std::get<Lookaround>(std::get<AssertionEdge>(edge.payload).payload);
        REQUIRE(std::holds_alternative<app_ref>(la.subregex));
        REQUIRE(la.is_positive == false);
        REQUIRE(la.direction == LookaroundDirection::BACKWARD);
    }

    SECTION("ASTNodeAssertion: positive lookaround with non-regular subregex creates fragment") {
        // A positive lookahead whose inner pattern is non-regular (backreference) is supported.
        // The subregex in the Lookaround is stored as a GraphFragment instead of an app_ref.
        ASTNodeAssertion assert_node(AssertionKind::LOOKAHEAD, std::make_unique<ASTNodeBackreference>(1));

        RegexComponent comp = assert_node.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<GraphFragment>(comp));

        GraphFragment frag = std::get<GraphFragment>(comp);
        REQUIRE(graph.edges.size() >= 1);

        const RCGEdge& outer_edge = graph.edges[frag.edges_pointing_to_vout[0]];
        REQUIRE(std::holds_alternative<AssertionEdge>(outer_edge.payload));
        const Lookaround& la = std::get<Lookaround>(std::get<AssertionEdge>(outer_edge.payload).payload);

        // The subregex must be a GraphFragment (non-regular).
        REQUIRE(std::holds_alternative<GraphFragment>(la.subregex));
        REQUIRE(la.is_positive == true);
        REQUIRE(la.direction == LookaroundDirection::FORWARD);

        // The inner fragment must contain exactly one BackrefEdge.
        const GraphFragment& inner = std::get<GraphFragment>(la.subregex);
        REQUIRE(graph.vertices[inner.v_in].outgoing_edges.size() == 1);
        EdgeID inner_eid = graph.vertices[inner.v_in].outgoing_edges[0];
        REQUIRE(std::holds_alternative<BackrefEdge>(graph.edges[inner_eid].payload));
        REQUIRE(std::get<BackrefEdge>(graph.edges[inner_eid].payload).backref_id == 1);
    }

    SECTION("ASTNodeAssertion: negative lookaround with non-regular subregex throws") {
        // A negative non-regular lookaround would require universal quantifiers — not supported.
        ASTNodeAssertion assert_node(AssertionKind::NEG_LOOKAHEAD, std::make_unique<ASTNodeBackreference>(1));
        REQUIRE_THROWS(assert_node.get_subgraph(graph, util_s, m));
    }

    // ---- Character class tests ----

    SECTION("ASTNodeCharClass negates correctly") {
        ASTNodeCharClass char_class(true);
        char_class.add_item(ClassRange {'a', 'a'});

        RegexComponent comp = char_class.get_subgraph(graph, util_s, m);
        REQUIRE(std::holds_alternative<app_ref>(comp));

        app_ref z3_regex = std::get<app_ref>(comp);
        REQUIRE(app_to_string(z3_regex, m) == "(re.inter re.allchar (re.comp (str.to_re \"a\")))");
    }

    SECTION("ASTNodeCharClass unites its items") {
        ASTNodeCharClass char_class(false);
        char_class.add_item(ClassRange {'a', 'z'});
        char_class.add_item(ClassRange {'_', '_'});
        char_class.add_item(CharClassEscape {ClassEscapeKind::DIGIT});

        app_ref z3_regex = std::get<app_ref>(char_class.get_subgraph(graph, util_s, m));
        REQUIRE(app_to_string(z3_regex, m) ==
                "(re.union (re.union (re.range \"a\" \"z\") (str.to_re \"_\")) (re.range \"0\" \"9\"))");
    }

    SECTION("ASTNodeCharClass range out of order throws") {
        ASTNodeCharClass char_class(false);
        REQUIRE_THROWS(char_class.add_item(ClassRange {'z', 'a'}));
    }

    SECTION("Empty ASTNodeCharClass matches nothing, negated one matches everything") {
        ASTNodeCharClass empty_class(false);
        app_ref empty_regex = std::get<app_ref>(empty_class.get_subgraph(graph, util_s, m));
        REQUIRE(app_to_string(empty_regex, m) == "re.none");

        ASTNodeCharClass negated_empty_class(true);
        app_ref all_regex = std::get<app_ref>(negated_empty_class.get_subgraph(graph, util_s, m));
        REQUIRE(app_to_string(all_regex, m) == "re.allchar");
    }

    SECTION("ASTNodeCharClass with \\s contains WhiteSpace and LineTerminator characters") {
        ASTNodeCharClass char_class(false);
        char_class.add_item(CharClassEscape {ClassEscapeKind::SPACE});

        // WhiteSpace (Table 32, including the Zs category) and LineTerminator (Table 33)
        const std::vector<std::pair<unsigned, unsigned>> whitespace_ranges {
            {0x0009, 0x000D}, {0x0020, 0x0020}, {0x00A0, 0x00A0}, {0x1680, 0x1680}, {0x2000, 0x200A},
            {0x2028, 0x2029}, {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000}, {0xFEFF, 0xFEFF}};
        sort* re_sort = util_s.re.mk_re(util_s.mk_string_sort());
        app* whitespaces = nullptr;
        for (const auto& [lo, hi] : whitespace_ranges) {
            app* range = util_s.re.mk_range(re_sort, lo, hi);
            whitespaces = whitespaces == nullptr ? range : util_s.re.mk_union(whitespaces, range);
        }
        const app_ref expected(whitespaces, m);

        app_ref z3_regex = std::get<app_ref>(char_class.get_subgraph(graph, util_s, m));
        REQUIRE(app_to_string(z3_regex, m) == app_to_string(expected, m));
        REQUIRE(app_to_string(z3_regex, m).find("\\u{1f}") == std::string::npos);
    }

    SECTION("ASTNodeCharClass with \\W is a complement of word characters") {
        ASTNodeCharClass char_class(false);
        char_class.add_item(CharClassEscape {ClassEscapeKind::NOT_WORD});

        const app_ref expected(
            util_s.re.mk_inter(util_s.re.mk_full_char(nullptr), util_s.re.mk_complement(util_s.re.mk_word_char())), m);

        app_ref z3_regex = std::get<app_ref>(char_class.get_subgraph(graph, util_s, m));
        REQUIRE(app_to_string(z3_regex, m) == app_to_string(expected, m));
    }

    SECTION("ASTNodeCharClass with a Unicode property escape throws") {
        ASTNodeCharClass char_class(false);
        const zstring property_value("Lu");
        char_class.add_item(CharClassEscape {ClassEscapeKind::PROPERTY, zstring_view(), property_value});
        REQUIRE_THROWS(char_class.get_subgraph(graph, util_s, m));
    }

    SECTION("ASTNodeCharClass clamps ranges to the maximal character") {
        const unsigned max_char = util_s.max_char();

        ASTNodeCharClass clamped_class(false);
        clamped_class.add_item(ClassRange {max_char - 1, 0x10FFFF});
        app_ref clamped_regex = std::get<app_ref>(clamped_class.get_subgraph(graph, util_s, m));
        app_ref expected(util_s.re.mk_range(util_s.str.mk_string(zstring(max_char - 1)),
                                            util_s.str.mk_string(zstring(max_char))),
                         m);
        REQUIRE(clamped_regex == expected);

        // a range completely above the maximal character is left out
        ASTNodeCharClass out_of_range_class(false);
        out_of_range_class.add_item(ClassRange {'a', 'a'});
        out_of_range_class.add_item(ClassRange {max_char + 1, 0x10FFFF});
        app_ref out_of_range_regex = std::get<app_ref>(out_of_range_class.get_subgraph(graph, util_s, m));
        REQUIRE(app_to_string(out_of_range_regex, m) == "(str.to_re \"a\")");
    }
}

// =====================================================================
// SERIALIZED RCG TESTS  (end-to-end: regex string -> RCG -> serialized string)
// =====================================================================

TEST_CASE("ECMA Regex serialized RCG tests", "[noodler][ecma]") {
    using Catch::Matchers::ContainsSubstring;
    using namespace smt::noodler::ecma;
    using namespace smt::noodler::ecma::test;

    ast_manager m;
    reg_decl_plugins(m);

    // ---- Basic cases ----

    SECTION("Single literal") {
        REQUIRE(build_and_serialize_rcg("a", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (str.to_re \"a\")]) (EDGE *->* [MATCH re.all]))");
    }

    SECTION("Capture group with markers") {
        REQUIRE(build_and_serialize_rcg("(a)", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (str.to_re \"a\")]) (EDGE *->* [MATCH re.all]))");
    }

    SECTION("Non-capturing group") {
        // Non-capturing groups are transparent — no STARTS/ENDS markers.
        REQUIRE(build_and_serialize_rcg("(?:a)", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (str.to_re \"a\")]) (EDGE *->* [MATCH re.all]))");
    }

    SECTION("Numeric backreference") {
        REQUIRE(build_and_serialize_rcg("(a)\\1", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (str.to_re \"a\")] STARTS {1} ENDS {1}) (EDGE *->* "
                "[BACKREF 1]) (EDGE *->* [MATCH re.all]))");
    }

    SECTION("Named capture group and named backreference") {
        // Named groups are translated to numeric groups internally.
        REQUIRE(build_and_serialize_rcg("(?<foo>a)\\k<foo>", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (str.to_re \"a\")] STARTS {1} ENDS {1}) (EDGE *->* "
                "[BACKREF 1]) (EDGE *->* [MATCH re.all]))");
    }

    // ---- Anchors ----

    SECTION("Anchors ^a$") {
        REQUIRE(build_and_serialize_rcg("^a$", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [ANCHOR '^']) (EDGE *->* [MATCH (str.to_re \"a\")]) (EDGE "
                "*->* [ANCHOR '$']) (EDGE *->* [MATCH re.all]))");
    }

    // ---- Alternation ----

    SECTION("Alternation without groups") {
        REQUIRE(build_and_serialize_rcg("a|b", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (re.union (str.to_re \"a\") (str.to_re \"b\"))]) "
                "(EDGE *->* [MATCH re.all]))");
    }

    SECTION("Alternation with capture groups") {
        REQUIRE(build_and_serialize_rcg("(a)|(b)", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (re.union (str.to_re \"a\") (str.to_re \"b\"))]) "
                "(EDGE *->* [MATCH re.all]))");
    }

    // ---- Sequential and nested groups ----

    SECTION("Sequential capture groups") {
        REQUIRE(build_and_serialize_rcg("(a)(b)(c)", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (re.++ (re.++ (str.to_re \"a\") (str.to_re \"b\")) "
                "(str.to_re \"c\"))]) (EDGE *->* [MATCH re.all]))");
    }

    SECTION("Complex nested groups and multiple backreferences") {
        REQUIRE(build_and_serialize_rcg(R"(((a)(b))\1\2\3)", m) ==
                "(RCG"
                " (EDGE *->* [MATCH re.all])"
                " (EDGE *->* [MATCH (str.to_re \"a\")] STARTS {2,1} ENDS {2})"
                " (EDGE *->* [MATCH (str.to_re \"b\")] STARTS {3} ENDS {3,1})"
                " (EDGE *->* [BACKREF 1])"
                " (EDGE *->* [BACKREF 2])"
                " (EDGE *->* [BACKREF 3])"
                " (EDGE *->* [MATCH re.all])"
                ")");
    }

    // ---- Regular lookarounds ----

    SECTION("Positive lookahead (regular)") {
        REQUIRE(build_and_serialize_rcg("(?=a)", m) == "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [LOOKAROUND ?= "
                                                       "(str.to_re \"a\")]) (EDGE *->* [MATCH re.all]))");
    }

    SECTION("Negative lookahead (regular)") {
        REQUIRE(build_and_serialize_rcg("(?!a)", m) == "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [LOOKAROUND ?! "
                                                       "(str.to_re \"a\")]) (EDGE *->* [MATCH re.all]))");
    }

    SECTION("Positive lookbehind (regular)") {
        REQUIRE(build_and_serialize_rcg("(?<=a)", m) == "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [LOOKAROUND ?<= "
                                                        "(str.to_re \"a\")]) (EDGE *->* [MATCH re.all]))");
    }

    SECTION("Negative lookbehind (regular)") {
        REQUIRE(build_and_serialize_rcg("(?<!a)", m) == "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [LOOKAROUND ?<! "
                                                        "(str.to_re \"a\")]) (EDGE *->* [MATCH re.all]))");
    }

    // ---- Non-regular lookarounds ----

    SECTION("Positive lookahead containing capture group (non-regular)") {
        // (?=(a)) — the lookahead contains a capture group, making its subregex non-regular.
        REQUIRE(build_and_serialize_rcg("(?=(a))", m) == "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [LOOKAROUND ?= "
                                                         "(str.to_re \"a\")]) (EDGE *->* [MATCH re.all]))");
    }

    SECTION("Positive lookahead with capture group followed by backreference") {
        // (?=(a))\1 — lookahead captures group 1, then \1 references it.
        REQUIRE(build_and_serialize_rcg("(?=(a))\\1", m) == "(RCG"
                                                            " (EDGE *->* [MATCH re.all])"
                                                            " (EDGE *->* [LOOKAROUND ?= (FRAGMENT"
                                                            " (EDGE *->* [MATCH (str.to_re \"a\")] STARTS {1} ENDS {1})"
                                                            " (EDGE *->* [MATCH re.all]))])"  // <-- nová Sigma* hrana
                                                            " (EDGE *->* [BACKREF 1])"
                                                            " (EDGE *->* [MATCH re.all])"
                                                            ")");
    }

    SECTION("Positive lookbehind containing capture group (non-regular)") {
        // (?<=(a)) — lookbehind with capture group.
        REQUIRE(build_and_serialize_rcg("(?<=(a))", m) == "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [LOOKAROUND ?<= "
                                                          "(str.to_re \"a\")]) (EDGE *->* [MATCH re.all]))");
    }

    // ---- Word boundaries ----

    SECTION("Word boundary assertion (\\b)") {
        const std::string rcg_dump = build_and_serialize_rcg("\\b", m);
        // \b expands to two branches: (?<=\w)(?!\w)  |  (?<!\w)(?=\w).
        REQUIRE_THAT(rcg_dump, ContainsSubstring("[LOOKAROUND ?<= "));
        REQUIRE_THAT(rcg_dump, ContainsSubstring("[LOOKAROUND ?! "));
        REQUIRE_THAT(rcg_dump, ContainsSubstring("[LOOKAROUND ?<! "));
        REQUIRE_THAT(rcg_dump, ContainsSubstring("[LOOKAROUND ?= "));
    }

    SECTION("Negated word boundary assertion (\\B)") {
        const std::string rcg_dump = build_and_serialize_rcg("\\B", m);
        // \B is the negation of \b -- same lookaround types, opposite polarity.
        REQUIRE_THAT(rcg_dump, ContainsSubstring("[LOOKAROUND ?<= "));
        REQUIRE_THAT(rcg_dump, ContainsSubstring("[LOOKAROUND ?= "));
        REQUIRE_THAT(rcg_dump, ContainsSubstring("[LOOKAROUND ?<! "));
        REQUIRE_THAT(rcg_dump, ContainsSubstring("[LOOKAROUND ?! "));
    }

    // ---- Greedy merge / interleaving ----

    SECTION("Mix of regular sequence and backreference") {
        // (x)ab\1cd — 'ab' and 'cd' are each merged into a single MATCH edge.
        REQUIRE(build_and_serialize_rcg("(x)ab\\1cd", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (str.to_re \"x\")] STARTS {1} ENDS {1}) (EDGE *->* "
                "[MATCH (str.to_re \"ab\")]) (EDGE *->* [BACKREF 1]) (EDGE *->* [MATCH (str.to_re \"cd\")]) (EDGE *->* "
                "[MATCH re.all]))");
    }

    SECTION("Alternation of regular and non-regular components") {
        // (x)|a|\1 — two MATCH edges and one BACKREF edge in parallel.
        REQUIRE(build_and_serialize_rcg("(x)|a|\\1", m) == "(RCG"
                                                           " (EDGE *->* [MATCH re.all])"
                                                           " (EDGE *->* [MATCH (str.to_re \"x\")] STARTS {1} ENDS {1})"
                                                           " (EDGE *->* [BACKREF 1])"
                                                           " (EDGE *->* [MATCH (str.to_re \"a\")])"
                                                           " (EDGE *->* [MATCH re.all])"
                                                           ")");
    }

    // ---- Complex patterns ----

    SECTION("Complex: Nested groups inside alternation with backreference") {
        REQUIRE(build_and_serialize_rcg("^((a)|(b))\\1$", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [ANCHOR '^']) (EDGE *->* [MATCH (re.union (str.to_re "
                "\"a\") (str.to_re \"b\"))] STARTS {1} ENDS {1}) (EDGE *->* [BACKREF 1]) (EDGE *->* [ANCHOR '$']) "
                "(EDGE *->* [MATCH re.all]))");
    }

    SECTION("Complex: HTML-like tag matching with interrupted greedy merge") {
        REQUIRE(build_and_serialize_rcg("<(?<tag>x)>y\\k<tag>", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (str.to_re \"<\")]) (EDGE *->* [MATCH (str.to_re "
                "\"x\")] STARTS {1} ENDS {1}) (EDGE *->* [MATCH (str.to_re \">y\")]) (EDGE *->* [BACKREF 1]) (EDGE "
                "*->* [MATCH re.all]))");
    }

    SECTION("Final Boss 1: Perfect BFS interleaving of parallel complex branches") {
        REQUIRE(build_and_serialize_rcg("^(?<v1>a)x(?=y)\\1$|^(?<v2>b)z(?<!w)\\2$", m) ==
                "(RCG"
                " (EDGE *->* [MATCH re.all])"
                " (EDGE *->* [ANCHOR '^'])"
                " (EDGE *->* [ANCHOR '^'])"
                " (EDGE *->* [MATCH (str.to_re \"a\")] STARTS {1} ENDS {1})"
                " (EDGE *->* [MATCH (str.to_re \"b\")] STARTS {2} ENDS {2})"
                " (EDGE *->* [MATCH (str.to_re \"x\")])"
                " (EDGE *->* [MATCH (str.to_re \"z\")])"
                " (EDGE *->* [LOOKAROUND ?= (str.to_re \"y\")])"
                " (EDGE *->* [LOOKAROUND ?<! (str.to_re \"w\")])"
                " (EDGE *->* [BACKREF 1])"
                " (EDGE *->* [BACKREF 2])"
                " (EDGE *->* [ANCHOR '$'])"
                " (EDGE *->* [ANCHOR '$'])"
                " (EDGE *->* [MATCH re.all])"
                ")");
    }

    SECTION("Final Boss 2: Greedy merge boundaries with non-capturing groups and named refs") {
        REQUIRE(build_and_serialize_rcg("^a(?:b|c)(?<named>d)\\k<named>(?=e)\\1f$", m) ==
                "(RCG"
                " (EDGE *->* [MATCH re.all])"
                " (EDGE *->* [ANCHOR '^'])"
                // Non-capturing group content merges greedily with the leading 'a'
                " (EDGE *->* [MATCH (re.++ (str.to_re \"a\") (re.union (str.to_re \"b\") (str.to_re \"c\")))])"
                // Named capture group gets its own edge
                " (EDGE *->* [MATCH (str.to_re \"d\")] STARTS {1} ENDS {1})"
                " (EDGE *->* [BACKREF 1])"
                " (EDGE *->* [LOOKAROUND ?= (str.to_re \"e\")])"
                " (EDGE *->* [BACKREF 1])"
                " (EDGE *->* [MATCH (str.to_re \"f\")])"
                " (EDGE *->* [ANCHOR '$'])"
                " (EDGE *->* [MATCH re.all])"
                ")");
    }

    // ---- Parser changes visible in the RCG ----

    SECTION("Unreferenced named capture group") {
        // unreferenced named groups are stripped as well
        REQUIRE(build_and_serialize_rcg("(?<n>a)", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (str.to_re \"a\")]) (EDGE *->* [MATCH re.all]))");
    }

    SECTION("Escaped syntax character is a literal") {
        REQUIRE(build_and_serialize_rcg("a\\u002A", m) ==
                "(RCG (EDGE *->* [MATCH re.all]) (EDGE *->* [MATCH (str.to_re \"a*\")]) (EDGE *->* [MATCH re.all]))");
    }

    SECTION("Syntax errors are reported by build_rcg") {
        REQUIRE_THROWS(build_and_serialize_rcg("a{", m));
        REQUIRE_THROWS(build_and_serialize_rcg("(a", m));
        REQUIRE_THROWS(build_and_serialize_rcg("\\1", m));
    }

    SECTION("Unicode property escapes are not supported") {
        REQUIRE_THROWS(build_and_serialize_rcg("\\p{Lu}", m));
        REQUIRE_THROWS(build_and_serialize_rcg("[^\\P{Script=Greek}]", m));
    }
}
