#pragma once

#include <string>
#include <algorithm>
#include <iterator>
#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>

#include "safe.hpp"
#include "errno.h"
#include <cctype>

#include <string_view>
using namespace std;

#define EMPTY_MACRO_FUNC(...)

#define PP(prop) "," #prop ":", prop
#define PP0(prop) #prop ":", prop
#define PRINT_PROPERTIES(...) eosio::print("{", __VA_ARGS__, "}")

#define CHECK(exp, msg) { if (!(exp)) eosio::check(false, msg); }

#ifndef ASSERT
    #define ASSERT(exp) CHECK(exp, #exp)
#endif

#define TRACE_L(...) TRACE(__VA_ARGS__, "\n")


/**
 * @brief 根据两种交易 token 的 symbol，生成唯一 LP Token symbol 名称
 *
 * 命名逻辑：
 *   symtype = 1: L + 前缀(symbol0, 3) + 前缀(symbol1, 3)       → e.g., LUSDETH
 *   symtype = 2: L + 前缀(symbol0, 3) + 后缀(symbol1, 3)       → e.g., LUSDETH
 *   symtype = 3: L + 后缀(symbol0, 3) + 前缀(symbol1, 3)       → e.g., LDTETH
 *   symtype = 4: L + 后缀(symbol0, 3) + 后缀(symbol1, 3)       → e.g., LDTETH
 *
 * 最终 symbol name 限制为不超过 12 个字符
 *
 * @param symbol0 第一个交易币种
 * @param symbol1 第二个交易币种
 * @param symtype 命名方式（取值 1~4）
 * @return std::string 生成的 LP symbol name
 */
inline std::string add_symbol(const symbol& symbol0, const symbol& symbol1, int symtype) {
    const std::string& code0 = symbol0.code().to_string();
    const std::string& code1 = symbol1.code().to_string();

    const int len0 = code0.length();
    const int len1 = code1.length();

    // 安全截取 prefix/suffix 最多3字符
    auto prefix0 = code0.substr(0, std::min(3, len0));
    auto prefix1 = code1.substr(0, std::min(3, len1));
    auto suffix0 = code0.substr(len0 >= 3 ? len0 - 3 : 0, 3);
    auto suffix1 = code1.substr(len1 >= 3 ? len1 - 3 : 0, 3);

    std::string code = "L";  // LP 前缀

    switch (symtype) {
        case 1: code += prefix0 + prefix1; break;
        case 2: code += prefix0 + suffix1; break;
        case 3: code += suffix0 + prefix1; break;
        case 4: code += suffix0 + suffix1; break;
        default:
            check(false, "invalid symtype");
    }

    // 控制最大长度，避免超过 symbol 限制
    if (code.length() > 12) {
        code = code.substr(0, 12);
    }

    return code;
}

inline name pool_symbol(symbol symbol0, symbol symbol1) {
    std::string code0 =  symbol0.code().to_string();
    std::string code1 =  symbol1.code().to_string();
    transform(code0.begin(), code0.end(), code0.begin(), ::tolower);
    transform(code1.begin(), code1.end(), code1.begin(), ::tolower);
    std::string code =   code0 + "." + code1;
    return name(code);
}

template<typename T>
int128_t multiply(int128_t a, int128_t b) {
    int128_t ret = a * b;
    CHECK(ret >= std::numeric_limits<T>::min() && ret <= std::numeric_limits<T>::max(),
          "overflow exception of multiply");
    return ret;
}

template<typename T>
int128_t divide_decimal(int128_t a, int128_t b, int128_t precision) {
    // with rounding-off method
    int128_t tmp = 10 * a * precision  / b;
    CHECK(tmp >= std::numeric_limits<T>::min() && tmp <= std::numeric_limits<T>::max(),
          "overflow exception of divide_decimal");
    return (tmp + 5) / 10;
}

template<typename T>
int128_t multiply_decimal(int128_t a, int128_t b, int128_t precision) {
    // with rounding-off method
    int128_t tmp = 10 * a * b / precision;
    CHECK(tmp >= std::numeric_limits<T>::min() && tmp <= std::numeric_limits<T>::max(),
          "overflow exception of multiply_decimal");
    return (tmp + 5) / 10;
}

template<typename T>
int128_t safe_multiply_decimal(int128_t a, int128_t b, int128_t precision) {
    // not rounding-off method
    int128_t tmp = a * b / precision;
    CHECK(tmp >= std::numeric_limits<T>::min() && tmp <= std::numeric_limits<T>::max(),
          "overflow exception of multiply_decimal");
    return tmp;
}

#define divide_decimal64(a, b, precision) divide_decimal<int64_t>(a, b, precision)
#define multiply_decimal64(a, b, precision) multiply_decimal<int64_t>(a, b, precision)
#define multiply_i64(a, b) multiply<int64_t>(a, b)


inline constexpr int64_t power(int64_t base, int64_t exp) {
    int64_t ret = 1;
    while( exp > 0  ) {
        ret *= base; --exp;
    }
    return ret;
}

inline constexpr int64_t power10(int64_t exp) {
    return power(10, exp);
}

inline constexpr int64_t calc_precision(int64_t digit) {
    return power10(digit);
}

string_view trim(string_view sv) {
    sv.remove_prefix(std::min(sv.find_first_not_of(" "), sv.size())); // left trim
    sv.remove_suffix(std::min(sv.size()-sv.find_last_not_of(" ")-1, sv.size())); // right trim
    return sv;
}

vector<string_view> split(string_view str, string_view delims = " ")
{
    vector<string_view> res;
    std::size_t current, previous = 0;
    current = str.find_first_of(delims);
    while (current != std::string::npos) {
        res.push_back(trim(str.substr(previous, current - previous)));
        previous = current + 1;
        current = str.find_first_of(delims, previous);
    }
    res.push_back(trim(str.substr(previous, current - previous)));
    return res;
}

bool starts_with(string_view sv, string_view s) {
    return sv.size() >= s.size() && sv.compare(0, s.size(), s) == 0;
}

int64_t to_int64(string_view s, const char* err_title) {
    errno = 0;
    uint64_t ret = std::strtoll(s.data(), nullptr, 10);
    CHECK(errno == 0, string(err_title) + ": convert str to int64 error: " + std::strerror(errno));
    return ret;
}

uint64_t to_uint64(string_view s, const char* err_title) {
    errno = 0;
    uint64_t ret = std::strtoul(s.data(), nullptr, 10);
    CHECK(errno == 0, string(err_title) + ": convert str to uint64 error: " + std::strerror(errno));
    return ret;
}

template <class T>
void precision_from_decimals(int8_t decimals, T& p10)
{
    CHECK(decimals <= 18, "symbol precision should be <= 18");
    p10 = 1;
    T p = decimals;
    while( p > 0  ) {
        p10 *= 10; --p;
    }
}

static symbol symbol_from_string(string_view from)
{
    string_view s = trim(from);
    CHECK(!s.empty(), "creating symbol from empty string");
    auto comma_pos = s.find(',');
    CHECK(comma_pos != string::npos, "missing comma in symbol");
    auto prec_part = s.substr(0, comma_pos);
    uint64_t p = to_uint64(prec_part, "symbol");
    string_view name_part = s.substr(comma_pos + 1);
    CHECK( p <= 18, "symbol precision should be <= 18");
    return symbol(name_part, p);
}

asset asset_from_string(string_view from)
{
    string_view s = trim(from);

    // Find space in order to split amount and symbol
    auto space_pos = s.find(' ');
    CHECK(space_pos != string::npos, "Asset's amount and symbol should be separated with space");
    auto symbol_str = trim(s.substr(space_pos + 1));
    auto amount_str = s.substr(0, space_pos);

    // Ensure that if decimal point is used (.), decimal fraction is specified
    auto dot_pos = amount_str.find('.');
    if (dot_pos != string::npos) {
        CHECK(dot_pos != amount_str.size() - 1, "Missing decimal fraction after decimal point");
    }

    // Parse symbol
    uint8_t precision_digit = 0;
    if (dot_pos != string::npos) {
        precision_digit = amount_str.size() - dot_pos - 1;
    }

    symbol sym = symbol(symbol_str, precision_digit);

    // Parse amount
    safe<int64_t> int_part, fract_part;
    if (dot_pos != string::npos) {
        int_part = to_int64(amount_str.substr(0, dot_pos), "asset");
        fract_part = to_uint64(amount_str.substr(dot_pos + 1), "asset");
        if (amount_str[0] == '-') fract_part *= -1;
    } else {
        int_part = to_int64(amount_str, "asset");
    }

    safe<int64_t> amount = int_part;
    safe<int64_t> precision; precision_from_decimals(sym.precision(), precision);
    amount *= precision;
    amount += fract_part;

    return asset(amount.value, sym);
}
std::string symbol_to_str(const symbol &sym) {
    return std::to_string(sym.precision()) + "," + sym.code().to_string();
}
std::string ext_symbol_to_str(const extended_symbol &ext_sym) {
    return symbol_to_str(ext_sym.get_symbol());
 }

uint64_t to_uint64(std::string_view s) {
    uint64_t result = 0;
    for (char c : s) {
        if (c < '0' || c > '9') break;
        result = result * 10 + (c - '0');
    }
    return result;
}

uint8_t to_uint8(std::string_view s) {
    return static_cast<uint8_t>(to_uint64(s));
}
 
bool is_numeric(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}