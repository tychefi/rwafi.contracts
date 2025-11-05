#pragma once
#include <cstdint>
#include <eosio/eosio.hpp>

namespace flon {


struct nsymbol {
    uint64_t value = 0;

    // consts
    static constexpr uint32_t U1E9  = 10'0000'0000UL;
    nsymbol() = default;

    static uint64_t to_raw_value(uint32_t i, uint32_t p) {
        eosio::check( p < U1E9, "pid must be below 10**9" );
        eosio::check( i < U1E9, "id must be below 10**9" );
        return (uint64_t)p * U1E9 + i;
    }

    explicit nsymbol(uint32_t i, uint32_t p = 0): value(to_raw_value(i, p)) {}

    explicit nsymbol(uint64_t raw): value(raw) {}

    friend bool operator==(const nsymbol& a, const nsymbol& b) {
        return( a.value == b.value );
    }

    inline uint64_t raw() const {
        return value;
    }

    inline uint32_t id() const {
        return value % U1E9;
    }

    inline uint32_t pid() const {
        return value / U1E9;
    }

    /**
     * Is this symbol valid
     */
    constexpr bool is_valid()const                 { return value != 0; }

    EOSLIB_SERIALIZE( nsymbol, (value) )
};

struct nasset {
    int64_t         amount  = 0;
    nsymbol         symbol;

    /**
     * Maximum amount possible for this asset. It's capped to 2^62 - 1
     */
    static constexpr int64_t max_amount    = (1LL << 62) - 1;

    nasset() = default;
    explicit nasset(const uint32_t& id): symbol(id), amount(0) {}
    explicit nasset(uint32_t id, uint32_t pid, int64_t amount = 0): symbol(id, pid), amount(amount) {
         eosio::check( is_amount_within_range(), "magnitude of asset amount must be less than 2^62" );
         eosio::check( symbol.is_valid(),        "invalid symbol name" );
    }
    explicit nasset(const int64_t& amount, const nsymbol& symb): amount(amount), symbol(symb) {
         eosio::check( is_amount_within_range(), "magnitude of asset amount must be less than 2^62" );
         eosio::check( symbol.is_valid(),        "invalid symbol name" );
    }


    /**
     * Check if the amount doesn't exceed the max amount
     *
     * @return true - if the amount doesn't exceed the max amount
     * @return false - otherwise
     */
    bool is_amount_within_range()const { return -max_amount <= amount && amount <= max_amount; }

    /**
     * Check if the nasset is valid. %A valid nasset has its amount <= max_amount and its symbol name valid
     *
     * @return true - if the asset is valid
     * @return false - otherwise
     */
    bool is_valid()const               { return is_amount_within_range() && symbol.is_valid(); }

    /**
     * Unary minus operator
     *
     * @return asset - New asset with its amount is the negative amount of this asset
     */
    nasset operator-()const {
        nasset r = *this;
        r.amount = -r.amount;
        return r;
    }

    /**
     * Subtraction assignment operator
     *
     * @param a - Another asset to subtract this asset with
     * @return asset& - Reference to this asset
     * @post The amount of this asset is subtracted by the amount of asset a
     */
    nasset& operator-=( const nasset& a ) {
        eosio::check( a.symbol == symbol, "attempt to subtract asset with different symbol" );
        amount -= a.amount;
        eosio::check( -max_amount <= amount, "subtraction underflow" );
        eosio::check( amount <= max_amount,  "subtraction overflow" );
        return *this;
    }

    /**
     * Addition Assignment  operator
     *
     * @param a - Another asset to subtract this asset with
     * @return asset& - Reference to this asset
     * @post The amount of this asset is added with the amount of asset a
     */
    nasset& operator+=( const nasset& a ) {
        eosio::check( a.symbol == symbol, "attempt to add asset with different symbol" );
        amount += a.amount;
        eosio::check( -max_amount <= amount, "addition underflow" );
        eosio::check( amount <= max_amount,  "addition overflow" );
        return *this;
    }

    /**
     * Addition operator
     *
     * @param a - The first asset to be added
     * @param b - The second asset to be added
     * @return asset - New asset as the result of addition
     */
    inline friend nasset operator+( const nasset& a, const nasset& b ) {
        nasset result = a;
        result += b;
        return result;
    }

    /**
     * Subtraction operator
     *
     * @param a - The asset to be subtracted
     * @param b - The asset used to subtract
     * @return asset - New asset as the result of subtraction of a with b
     */
    inline friend nasset operator-( const nasset& a, const nasset& b ) {
        nasset result = a;
        result -= b;
        return result;
    }

    /**
     * Multiplication assignment operator, with a number
     *
     * @details Multiplication assignment operator. Multiply the amount of this asset with a number and then assign the value to itself.
     * @param a - The multiplier for the asset's amount
     * @return asset - Reference to this asset
     * @post The amount of this asset is multiplied by a
     */
    nasset& operator*=( int64_t a ) {
        int128_t tmp = (int128_t)amount * (int128_t)a;
        eosio::check( tmp <= max_amount, "multiplication overflow" );
        eosio::check( tmp >= -max_amount, "multiplication underflow" );
        amount = (int64_t)tmp;
        return *this;
    }

    /**
     * Multiplication operator, with a number proceeding
     *
     * @brief Multiplication operator, with a number proceeding
     * @param a - The asset to be multiplied
     * @param b - The multiplier for the asset's amount
     * @return asset - New asset as the result of multiplication
     */
    friend nasset operator*( const nasset& a, int64_t b ) {
        nasset result = a;
        result *= b;
        return result;
    }


    /**
     * Multiplication operator, with a number preceeding
     *
     * @param a - The multiplier for the asset's amount
     * @param b - The asset to be multiplied
     * @return asset - New asset as the result of multiplication
     */
    friend nasset operator*( int64_t b, const nasset& a ) {
        nasset result = a;
        result *= b;
        return result;
    }

    /**
     * @brief Division assignment operator, with a number
     *
     * @details Division assignment operator. Divide the amount of this asset with a number and then assign the value to itself.
     * @param a - The divisor for the asset's amount
     * @return asset - Reference to this asset
     * @post The amount of this asset is divided by a
     */
    nasset& operator/=( int64_t a ) {
        eosio::check( a != 0, "divide by zero" );
        eosio::check( !(amount == std::numeric_limits<int64_t>::min() && a == -1), "signed division overflow" );
        amount /= a;
        return *this;
    }

    /**
     * Division operator, with a number proceeding
     *
     * @param a - The asset to be divided
     * @param b - The divisor for the asset's amount
     * @return asset - New asset as the result of division
     */
    friend nasset operator/( const nasset& a, int64_t b ) {
        nasset result = a;
        result /= b;
        return result;
    }

    /**
     * Division operator, with another asset
     *
     * @param a - The asset which amount acts as the dividend
     * @param b - The asset which amount acts as the divisor
     * @return int64_t - the resulted amount after the division
     * @pre Both asset must have the same symbol
     */
    friend int64_t operator/( const nasset& a, const nasset& b ) {
        eosio::check( b.amount != 0, "divide by zero" );
        eosio::check( a.symbol == b.symbol, "attempt to divide assets with different symbol" );
        return a.amount / b.amount;
    }

    /**
     * Equality operator
     *
     * @param a - The first asset to be compared
     * @param b - The second asset to be compared
     * @return true - if both asset has the same amount
     * @return false - otherwise
     * @pre Both asset must have the same symbol
     */
    friend bool operator==(const nasset& a, const nasset& b) {
        return( a.amount == b.amount && a.symbol == b.symbol );
    }

    /**
     * Inequality operator
     *
     * @param a - The first asset to be compared
     * @param b - The second asset to be compared
     * @return true - if both asset doesn't have the same amount
     * @return false - otherwise
     * @pre Both asset must have the same symbol
     */
    friend bool operator!=( const nasset& a, const nasset& b ) {
        return !( a == b);
    }

    /**
     * Less than operator
     *
     * @param a - The first asset to be compared
     * @param b - The second asset to be compared
     * @return true - if the first asset's amount is less than the second asset amount
     * @return false - otherwise
     * @pre Both asset must have the same symbol
     */
    friend bool operator<( const nasset& a, const nasset& b ) {
        eosio::check( a.symbol == b.symbol, "comparison of assets with different symbols is not allowed" );
        return a.amount < b.amount;
    }

    /**
     * Less or equal to operator
     *
     * @param a - The first asset to be compared
     * @param b - The second asset to be compared
     * @return true - if the first asset's amount is less or equal to the second asset amount
     * @return false - otherwise
     * @pre Both asset must have the same symbol
     */
    friend bool operator<=( const nasset& a, const nasset& b ) {
        eosio::check( a.symbol == b.symbol, "comparison of assets with different symbols is not allowed" );
        return a.amount <= b.amount;
    }

    /**
     * Greater than operator
     *
     * @param a - The first asset to be compared
     * @param b - The second asset to be compared
     * @return true - if the first asset's amount is greater than the second asset amount
     * @return false - otherwise
     * @pre Both asset must have the same symbol
     */
    friend bool operator>( const nasset& a, const nasset& b ) {
        eosio::check( a.symbol == b.symbol, "comparison of assets with different symbols is not allowed" );
        return a.amount > b.amount;
    }

    /**
     * Greater or equal to operator
     *
     * @param a - The first asset to be compared
     * @param b - The second asset to be compared
     * @return true - if the first asset's amount is greater or equal to the second asset amount
     * @return false - otherwise
     * @pre Both asset must have the same symbol
     */
    friend bool operator>=( const nasset& a, const nasset& b ) {
        eosio::check( a.symbol == b.symbol, "comparison of assets with different symbols is not allowed" );
        return a.amount >= b.amount;
    }

    EOSLIB_SERIALIZE( nasset, (amount)(symbol) )
};
}