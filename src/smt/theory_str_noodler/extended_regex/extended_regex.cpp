#include "extended_regex.h"

#include "parsers/ecma.h"
#include "smt/theory_str_noodler/util.h"

namespace smt::noodler::extended_regex {
    ExtendedRegex::ExtendedRegex(zstring pattern)
        : m_pattern(std::move(pattern)) { }

    zstring_view ExtendedRegex::get_pattern() const {
        return m_pattern;
    }

    std::unique_ptr<ExtendedRegex> make_extended_regex(const RegexFlavor flavor, const zstring& pattern) {
        switch (flavor) {
            case RegexFlavor::ECMA2020:
                return std::make_unique<ecma::ECMARegex>(pattern);
        }
        util::throw_error("Extended regex: unknown regex flavor");
        return nullptr;
    }

    std::optional<RegexFlavor> regex_flavor_from_name(const std::string& name) {
        if (name == "ecma2020") {
            return RegexFlavor::ECMA2020;
        }
        return std::nullopt;
    }
}  // namespace smt::noodler::extended_regex
