#ifndef slic3r_LocalesUtils_hpp_
#define slic3r_LocalesUtils_hpp_

#include <string>
#include <clocale>
#include <iomanip>
#include <cassert>
#include <string_view>

#ifdef __APPLE__
#include <xlocale.h>
#endif

namespace Slic3r {

// RAII wrapper that sets LC_NUMERIC to "C" on construction
// and restores the old value on destruction.
class CNumericLocalesSetter {
public:
    CNumericLocalesSetter();
    ~CNumericLocalesSetter();

private:
#ifdef _WIN32
    std::string m_orig_numeric_locale;
#else
    locale_t m_original_locale;
    locale_t m_new_locale;
#endif

};

// A function to check that current C locale uses decimal point as a separator.
// Intended mostly for asserts.
bool is_decimal_separator_point();


// A substitute for std::to_string that works according to
// C++ locales, not C locale. Meant to be used when we need
// to be sure that decimal point is used as a separator.
// (We use user C locales and "C" C++ locales in most of the code.)
std::string float_to_string_decimal_point(double value, int precision = -1);
//std::string float_to_string_decimal_point(float value,  int precision = -1);
double string_to_double_decimal_point(const std::string_view str, size_t* pos = nullptr);

// NEOTKO (audit 1-nov-2026) — a number read the same in any UI language. std::stod / atof / strtod follow
// LC_NUMERIC, which Orca sets to the UI language: with Spanish, German... "6.7845" read as 6, silently.
// Skips leading spaces and '+'; a ',' is taken as the decimal point when there is no '.', so values an
// older build wrote with the locale still read. Returns `dflt` when no number is there; `used` gets the
// characters read from `str` (0 then).
double neo_to_double(std::string_view str, double dflt = 0., size_t* used = nullptr);

} // namespace Slic3r

#endif // slic3r_LocalesUtils_hpp_
