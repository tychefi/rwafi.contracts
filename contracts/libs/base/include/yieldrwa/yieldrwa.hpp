#include "yieldrwadb.hpp"

using namespace std;
using namespace rwafi ;

class [[eosio::contract("yield.rwa")]] yieldrwa: public eosio::contract {
public:
    ACTION updateconfig( const name& key, const uint8_t& value );


}; //contract yieldrwa