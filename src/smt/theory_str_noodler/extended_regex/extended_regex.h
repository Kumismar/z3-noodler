#pragma once

#include "smt/theory_str_noodler/extended_regex/backend.h"
#include "util/zstring.h"
#include "util/zstring_view.h"

#include <memory>

// Interface of the extended regexes. Each regex flavor has its own frontend (a parser into the common AST) in
// extended_regex/frontend/ and its own nested namespace (e.g. extended_regex::ecma). All the flavors share the backend
// (extended_regex/backend.h) that translates the common AST into string constraints.
namespace smt::noodler::extended_regex {
    /**
     * Supported flavors of extended regexes.
     */
    enum class RegexFlavor {
        ECMA2020,  // re.from_ecma2020
    };

    /**
     * An extended regex pattern of some flavor. Each flavor implements the parsing of the pattern into the common AST.
     *
     * Usage:
     *   auto regex = make_extended_regex(RegexFlavor::ECMA2020, pattern);
     *   RegexConstraintBuilder builder(m, *regex, params);
     *   builder.build_rcg();
     *   expr_ref formula = builder.generate_constraints(target);
     */
    class ExtendedRegex {
    public:
        virtual ~ExtendedRegex() = default;

        // The AST keeps views into the pattern owned by this object, so copying is not allowed
        ExtendedRegex(const ExtendedRegex&) = delete;
        ExtendedRegex& operator=(const ExtendedRegex&) = delete;

        virtual RegexFlavor get_flavor() const = 0;

        /**
         * @brief Parse the pattern into the common AST.
         *
         * Syntax errors and unsupported constructs are reported via util::throw_error. The AST keeps views into the
         * pattern owned by this object, so this object must outlive the AST.
         *
         * @return ASTNodeRef The root of the common AST.
         */
        virtual ASTNodeRef parse() const = 0;

        /**
         * @brief Return the pattern (already preprocessed by the frontend).
         */
        zstring_view get_pattern() const;

    protected:
        explicit ExtendedRegex(zstring pattern);

    private:
        zstring m_pattern;
    };

    /**
     * @brief Create the frontend of the @p flavor for the @p pattern.
     *
     * @param flavor  The flavor of the regex.
     * @param pattern The pattern as given in the SMT-LIB input.
     * @return std::unique_ptr<ExtendedRegex> The regex of the given flavor.
     */
    std::unique_ptr<ExtendedRegex> make_extended_regex(RegexFlavor flavor, const zstring& pattern);
}  // namespace smt::noodler::extended_regex
