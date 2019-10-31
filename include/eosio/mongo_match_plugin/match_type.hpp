#pragma once

#include <eosio/chain/authority.hpp>
#include <eosio/chain/chain_config.hpp>
#include <eosio/chain/config.hpp>
#include <eosio/chain/types.hpp>

namespace eosio { namespace match {

using action_name    = eosio::chain::action_name;

const static uint64_t match_account_name    = N(sys.match);


struct openorder {
   account_name traders;
   asset base_coin;
   asset quote_coin;
   name trade_pair_name;
   account_name exc_acc;

   static account_name get_account() {
      return match_account_name;
   }

   static action_name get_name() {
      return N(openorder);
   }
};

struct match {
   uint64_t scope_base;
   uint64_t base_id;
   uint64_t scope_quote;
   name trade_pair_name;
   account_name exc_acc;

   static account_name get_account() {
      return match_account_name;
   }

   static action_name get_name() {
      return N(match);
   }
};

struct order_deal_info{
   uint64_t    order_scope;
   uint64_t    order_id;
   account_name exc_acc;
   account_name trader;

   fc::variant to_variant() const
   {
      chain::mutable_variant_object o;
              o( "order_scope",              order_scope             )
               ( "order_id",                 order_id                )
               ( "exc_acc",                  exc_acc                 )
               ( "trader",                   trader                  );

      return o;
   }
};

struct recorddeal_param {
   uint64_t deal_scope;
   order_deal_info deal_base;
   order_deal_info deal_quote;
   asset base;
   asset quote;
   uint32_t current_block;
   account_name exc_acc;

   fc::variant to_variant() const
   {
      chain::mutable_variant_object o;
              o( "deal_scope",                  deal_scope              )
               ( "deal_base",                   deal_base               )
               ( "deal_quote",                  deal_quote              )
               ( "base",                        base                    )
               ( "quote",                       quote                   )
               ( "current_block",               current_block           )
               ( "exc_acc",                     exc_acc                 );

      return o;
   }
};

struct recorddeal {
   vector<recorddeal_param> params;

   static account_name get_account() {
      return match_account_name;
   }

   static action_name get_name() {
      return N(recorddeal);
   }
};

enum  order_status {
   Normal = 0,
   Done,
   Cancel
};

struct order{
   uint64_t scope;
   uint64_t order_id;
   account_name traders;
   asset base_coin;
   asset quote_coin;
   asset undone_base_coin;
   asset undone_quote_coin;
   name trade_pair_name;
   account_name exc_acc;
   uint32_t   order_status;
   uint64_t   order_price;

   void set_open_value( const openorder &a) {
      traders = a.traders;
      base_coin = a.base_coin;
      quote_coin = a.quote_coin;
      undone_base_coin = a.base_coin;
      undone_quote_coin = a.quote_coin;
      trade_pair_name = a.trade_pair_name;
      exc_acc = a.exc_acc;
      order_status = order_status::Normal;
   }

   void set_match_value( const match &a ) {
      scope = a.scope_base;
      order_id = a.base_id;
      if (scope & 1) {
         order_price = base_coin.get_amount() * 1000000 / quote_coin.get_amount();
      }
      else {
         order_price = quote_coin.get_amount() * 1000000 / base_coin.get_amount();
      }
   }

   bool isempty() const {
      return base_coin.get_amount() == 0;
   }

   fc::variant to_variant() const
   {
      chain::mutable_variant_object o;
              o( "scope",              scope                   )
               ( "order_id",           order_id                )
               ( "traders",            traders                 )
               ( "base_coin",          base_coin               )
               ( "quote_coin",         quote_coin              )
               ( "undone_base_coin",   undone_base_coin         )
               ( "undone_quote_coin",  undone_quote_coin         )
               ( "trade_pair_name",    trade_pair_name         )
               ( "exc_acc",            exc_acc                 )
               ( "order_status",       order_status            )
               ( "order_price",       order_price            );

      return o;
   }
};

struct cancelorder {
   uint64_t orderscope;
   uint64_t orderid;

   static account_name get_account() {
      return match_account_name;
   }

   static action_name get_name() {
      return N(cancelorder);
   }

   fc::variant to_variant() const
   {
      chain::mutable_variant_object o;
              o( "orderscope",               orderscope                   )
               ( "orderid",                  orderid                );

      return o;
   }
};


} } /// namespace eosio::match

FC_REFLECT( eosio::match::openorder                       , (traders)(base_coin)(quote_coin)(trade_pair_name)(exc_acc) )
FC_REFLECT( eosio::match::match                           , (scope_base)(base_id)(scope_quote)(trade_pair_name)(exc_acc) )
FC_REFLECT( eosio::match::order_deal_info                 , (order_scope)(order_id)(exc_acc)(trader) )
FC_REFLECT( eosio::match::recorddeal_param                , (deal_scope)(deal_base)(deal_quote)(base)(quote)(current_block)(exc_acc) )
FC_REFLECT( eosio::match::recorddeal                      , (params) )
FC_REFLECT( eosio::match::cancelorder                    , (orderscope)(orderid) )
FC_REFLECT( eosio::match::order                           , (scope)(order_id)(traders)(base_coin)(quote_coin)(undone_base_coin)(undone_quote_coin)(trade_pair_name)(exc_acc)(order_status)(order_price) )




