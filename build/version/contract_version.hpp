
#pragma once

#include <eosio/eosio.hpp>

#define CONTRACT_VERSION "0.1.0-5465bcd07995f6f4ae01a7e7d3b36ee02880d369-dirty"

#define __DEFINE_VERSION_CONTRACT_CLASS(contract_name, class_name)                      \
class [[eosio::contract(contract_name)]]                                                \
class_name : public eosio::contract {                                                   \
public:                                                                                 \
    using contract::contract;                                                           \
    [[eosio::action, eosio::read_only]] std::string version() {                                           \
        return CONTRACT_VERSION;                                                        \
    }                                                                                   \
    using version_action = eosio::action_wrapper<"version"_n, &class_name::version>;    \
};

#define GET_VERSION_CONTRACT_CLASS(class_subfix) version_contract_##class_subfix

#define DEFINE_VERSION_CONTRACT_CLASS(contract_name, class_subfix)                      \
        __DEFINE_VERSION_CONTRACT_CLASS(contract_name,                                  \
        GET_VERSION_CONTRACT_CLASS(class_subfix))
