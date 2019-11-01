/**
 *  @file
 *  @copyright defined in eos/LICENSE
 */
#include <eosio/mongo_match_plugin/mongo_match_plugin.hpp>
#include <eosio/mongo_match_plugin/match_type.hpp>
#include <eosio/chain/eosio_contract.hpp>
#include <eosio/chain/config.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/transaction.hpp>
#include <eosio/chain/types.hpp>

#include <fc/io/json.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/utf8.hpp>
#include <fc/variant.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/chrono.hpp>
#include <boost/signals2/connection.hpp>

#include <queue>
#include <thread>
#include <mutex>

#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/decimal128.hpp>

#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/exception/logic_error.hpp>
#include <eosio/chain/genesis_state.hpp>

namespace fc { class variant; }

namespace eosio {

using chain::account_name;
using chain::action_name;
using chain::block_id_type;
using chain::permission_name;
using chain::transaction;
using chain::signed_transaction;
using chain::signed_block;
using chain::transaction_id_type;
using chain::packed_transaction;

static appbase::abstract_plugin& _mongo_match_plugin = app().register_plugin<mongo_match_plugin>();

class mongo_match_plugin_impl {
public:
   mongo_match_plugin_impl();
   ~mongo_match_plugin_impl();

   fc::optional<boost::signals2::scoped_connection> applied_transaction_connection;

   void consume_blocks();

   void applied_transaction(const chain::transaction_trace_ptr&);
   void process_applied_transaction(const chain::transaction_trace_ptr&);
   void _process_applied_transaction(const chain::transaction_trace_ptr&);

   void handleorder(const match::order &order_data);
   void handledeal(const match::recorddeal &deal_data);
   void recordonedeal(const match::recorddeal_param &deal_data);
   void modifyorder(const uint64_t &scope,const uint64_t &order_id,const asset &base,const asset &quote);
   void cancelorder(const match::cancelorder &cancel_order);

   bool b_insert_default_abi = false;

   void init();
   void wipe_database();

   template<typename Queue, typename Entry> void queue(Queue& queue, const Entry& e);

   bool configured{false};
   bool wipe_database_on_startup{false};
   uint32_t start_block_num = 0;
   std::atomic_bool start_block_reached{false};

   std::string db_name;
   mongocxx::instance mongo_inst;
   fc::optional<mongocxx::pool> mongo_pool;

   // consum thread
   mongocxx::collection _orders;
   mongocxx::collection _deals;

   size_t max_queue_size = 0;
   int queue_sleep_time = 0;

   std::deque<chain::transaction_trace_ptr> transaction_trace_queue;
   std::deque<chain::transaction_trace_ptr> transaction_trace_process_queue;

   std::mutex mtx;
   std::condition_variable condition;
   std::thread consume_thread;
   std::atomic_bool done{false};
   std::atomic_bool startup{true};
   fc::optional<chain::chain_id_type> chain_id;
   //fc::microseconds abi_serializer_max_time;

   static const action_name openorder;
   static const action_name match;
   static const action_name recorddeal;

   static const std::string orders_col;
   static const std::string deals_col;

};

const std::string mongo_match_plugin_impl::orders_col = "orders";
const std::string mongo_match_plugin_impl::deals_col = "deals";

template<typename Queue, typename Entry>
void mongo_match_plugin_impl::queue( Queue& queue, const Entry& e ) {
   std::unique_lock<std::mutex> lock( mtx );
   auto queue_size = queue.size();
   if( queue_size > max_queue_size ) {
      lock.unlock();
      condition.notify_one();
      queue_sleep_time += 10;
      if( queue_sleep_time > 1000 )
         wlog("queue size: ${q}", ("q", queue_size));
      std::this_thread::sleep_for( std::chrono::milliseconds( queue_sleep_time ));
      lock.lock();
   } else {
      queue_sleep_time -= 10;
      if( queue_sleep_time < 0 ) queue_sleep_time = 0;
   }
   queue.emplace_back( e );
   lock.unlock();
   condition.notify_one();
}

void mongo_match_plugin_impl::applied_transaction( const chain::transaction_trace_ptr& t ) {
   try {
      // Traces emitted from an incomplete block leave the producer_block_id as empty.
      //
      // Avoid adding the action traces or transaction traces to the database if the producer_block_id is empty.
      // This way traces from speculatively executed transactions are not included in the Mongo database which can
      // avoid potential confusion for consumers of that database.
      //
      // Due to forks, it could be possible for multiple incompatible action traces with the same block_num and trx_id
      // to exist in the database. And if the producer double produces a block, even the block_time may not
      // disambiguate the two action traces. Without a producer_block_id to disambiguate and determine if the action
      // trace comes from an orphaned fork branching off of the blockchain, consumers of the Mongo DB database may be
      // reacting to a stale action trace that never actually executed in the current blockchain.
      //
      // It is better to avoid this potential confusion by not logging traces from speculative execution, i.e. emitted
      // from an incomplete block. This means that traces will not be recorded in speculative read-mode, but
      // users should not be using the mongo_match_plugin in that mode anyway.
      //
      // Allow logging traces if node is a producer for testing purposes, so a single nodeos can do both for testing.
      //
      // It is recommended to run mongo_match_plugin in read-mode = read-only.
      //

      // always queue since account information always gathered
      queue( transaction_trace_queue, t );
   } catch (fc::exception& e) {
      elog("FC Exception while applied_transaction ${e}", ("e", e.to_string()));
   } catch (std::exception& e) {
      elog("STD Exception while applied_transaction ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while applied_transaction");
   }
}

void mongo_match_plugin_impl::consume_blocks() {
   try {
      auto mongo_client = mongo_pool->acquire();
      auto& mongo_conn = *mongo_client;

      _orders = mongo_conn[db_name][orders_col];
      _deals = mongo_conn[db_name][deals_col];

     // insert_default_abi();
      while (true) {
         std::unique_lock<std::mutex> lock(mtx);
          while ( 
                 transaction_trace_queue.empty() &&
                 !done ) {
            condition.wait(lock);
         }

         size_t transaction_trace_size = transaction_trace_queue.size();
         if (transaction_trace_size > 0) {
            transaction_trace_process_queue = move(transaction_trace_queue);
            transaction_trace_queue.clear();
         }

         lock.unlock();

         if (done) {
            ilog("draining queue, size: ${q}", ("q", /*transaction_metadata_size + */ transaction_trace_size /*+ block_state_size + irreversible_block_size*/));
         }

         // process transactions
         auto start_time = fc::time_point::now();
         auto size = transaction_trace_process_queue.size();
         while (!transaction_trace_process_queue.empty()) {
            const auto& t = transaction_trace_process_queue.front();
            process_applied_transaction(t);
            transaction_trace_process_queue.pop_front();
         }
         auto time = fc::time_point::now() - start_time;
         auto per = size > 0 ? time.count()/size : 0;
         if( time > fc::microseconds(500000) ) // reduce logging, .5 secs
            ilog( "process_applied_transaction,  time per: ${p}, size: ${s}, time: ${t}", ("s", size)("t", time)("p", per) );

         if( 
             transaction_trace_size == 0 &&
             done ) {
            break;
         }
      }
      ilog("mongo_match_plugin consume thread shutdown gracefully");
   } catch (fc::exception& e) {
      elog("FC Exception while consuming block ${e}", ("e", e.to_string()));
   } catch (std::exception& e) {
      elog("STD Exception while consuming block ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while consuming block");
   }
}

namespace {

void handle_mongo_exception( const std::string& desc, int line_num ) {
   bool shutdown = true;
   try {
      try {
         throw;
      } catch( mongocxx::logic_error& e) {
         // logic_error on invalid key, do not shutdown
         wlog( "mongo logic error, ${desc}, line ${line}, code ${code}, ${what}",
               ("desc", desc)( "line", line_num )( "code", e.code().value() )( "what", e.what() ));
         shutdown = false;
      } catch( mongocxx::operation_exception& e) {
         elog( "mongo exception, ${desc}, line ${line}, code ${code}, ${details}",
               ("desc", desc)( "line", line_num )( "code", e.code().value() )( "details", e.code().message() ));
         if (e.raw_server_error()) {
            elog( "  raw_server_error: ${e}", ( "e", bsoncxx::to_json(e.raw_server_error()->view())));
         }
      } catch( mongocxx::exception& e) {
         elog( "mongo exception, ${desc}, line ${line}, code ${code}, ${what}",
               ("desc", desc)( "line", line_num )( "code", e.code().value() )( "what", e.what() ));
      } catch( bsoncxx::exception& e) {
         elog( "bsoncxx exception, ${desc}, line ${line}, code ${code}, ${what}",
               ("desc", desc)( "line", line_num )( "code", e.code().value() )( "what", e.what() ));
      } catch( fc::exception& er ) {
         elog( "mongo fc exception, ${desc}, line ${line}, ${details}",
               ("desc", desc)( "line", line_num )( "details", er.to_detail_string()));
      } catch( const std::exception& e ) {
         elog( "mongo std exception, ${desc}, line ${line}, ${what}",
               ("desc", desc)( "line", line_num )( "what", e.what()));
      } catch( ... ) {
         elog( "mongo unknown exception, ${desc}, line ${line_nun}", ("desc", desc)( "line_num", line_num ));
      }
   } catch (...) {
      std::cerr << "Exception attempting to handle exception for " << desc << " " << line_num << std::endl;
   }

   if( shutdown ) {
      // shutdown if mongo failed to provide opportunity to fix issue and restart
      app().quit();
   }
}

// custom oid to avoid monotonic throttling
// https://docs.mongodb.com/master/core/bulk-write-operations/#avoid-monotonic-throttling
bsoncxx::oid make_custom_oid() {
   bsoncxx::oid x = bsoncxx::oid();
   const char* p = x.bytes();
   std::swap((short&)p[0], (short&)p[10]);
   return x;
}

} // anonymous namespace



void mongo_match_plugin_impl::process_applied_transaction( const chain::transaction_trace_ptr& t ) {
   try {
      // always call since we need to capture setabi on accounts even if not storing transaction traces
      _process_applied_transaction( t );
   } catch (fc::exception& e) {
      elog("FC Exception while processing applied transaction trace: ${e}", ("e", e.to_detail_string()));
   } catch (std::exception& e) {
      elog("STD Exception while processing applied transaction trace: ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while processing applied transaction trace");
   }
}

void mongo_match_plugin_impl::handleorder(const match::order &order_data) {
   using namespace bsoncxx::types;
   using bsoncxx::builder::basic::kvp;
   using bsoncxx::builder::basic::make_document;

   auto trans_traces_doc = bsoncxx::builder::basic::document{};

   auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()});

   mongocxx::options::update update_opts{};
   update_opts.upsert( true );

   auto var_order = order_data.to_variant();
   auto json = fc::json::to_string(var_order);

   auto block_doc = bsoncxx::builder::basic::document{};

   try {
      const auto& value = bsoncxx::from_json( json );
      block_doc.append( kvp( "order", value ) );
   } catch( bsoncxx::exception& ) {
      try {
         json = fc::prune_invalid_utf8( json );
         const auto& value = bsoncxx::from_json( json );
         block_doc.append( kvp( "order", value ) );
         block_doc.append( kvp( "non-utf8-purged", b_bool{true} ) );
      } catch( bsoncxx::exception& e ) {
         elog( "Unable to convert block JSON to MongoDB JSON: ${e}", ("e", e.what()) );
         elog( "  JSON: ${j}", ("j", json) );
      }
   }

   block_doc.append( kvp( "createdAt", b_date{now} ) );
   auto order_oid_info = make_custom_oid();

   try {

      if( !_orders.update_one( make_document( kvp( "scope_orderid", bsoncxx::decimal128{order_data.scope,order_data.order_id} ) ),
                               make_document( kvp( "$set", block_doc.view() ) ), update_opts ) ) {
         EOS_ASSERT( false, chain::mongo_db_insert_fail, "Failed to insert block ${scope},${order_id}", ("scope", order_data.scope)("order_id",order_data.order_id) );
      }

   } catch( ... ) {
      handle_mongo_exception( "blocks insert: " + json, __LINE__ );
   }

}

void mongo_match_plugin_impl::handledeal(const match::recorddeal &deal_data) {
   uint64_t scope_quote = eosio::chain::asset::max_amount,order_quote;
   asset coin_base,coin_quote;
   for ( auto &deal_info: deal_data.params ) {
      modifyorder(deal_info.deal_base.order_scope,deal_info.deal_base.order_id,deal_info.base,deal_info.quote);
      if ( scope_quote != deal_info.deal_quote.order_scope ) {
         scope_quote = deal_info.deal_quote.order_scope;
         order_quote = deal_info.deal_quote.order_id;
         coin_base = deal_info.base;
         coin_quote = deal_info.quote;
      }
      else {
         coin_base += deal_info.base;
         coin_quote += deal_info.quote;
      }
      recordonedeal(deal_info);
   }
   modifyorder(scope_quote,order_quote,coin_quote,coin_base);
}

void mongo_match_plugin_impl::modifyorder(const uint64_t &scope,const uint64_t &order_id,const asset &base,const asset &quote) {
   using bsoncxx::builder::basic::make_document;
   using bsoncxx::builder::basic::kvp;

   auto order_info = _orders.find_one( make_document( kvp( "scope_orderid", bsoncxx::decimal128{scope,order_id})));
   if (!order_info) {
      FC_ASSERT( false, "no order find");
      return ;
   }

   auto undone_base_coin = chain::asset::from_string(order_info->view()["order"]["undone_base_coin"].get_utf8().value.to_string());
   auto undone_quote_coin = chain::asset::from_string(order_info->view()["order"]["undone_quote_coin"].get_utf8().value.to_string());
   
   undone_base_coin -= base;
   undone_quote_coin -= quote;

   try {
      auto order_doc = bsoncxx::builder::basic::document{};
      order_doc.append( kvp( "order.undone_base_coin", undone_base_coin.to_string() ) );
      order_doc.append( kvp( "order.undone_quote_coin", undone_quote_coin.to_string() ) );

      if ( (scope & 1 && undone_quote_coin.get_amount() == 0) || ( !(scope & 1)  && undone_base_coin.get_amount() == 0 )) {
         order_doc.append( kvp( "order.order_status", match::order_status::Done ) );
      }
      auto update_from = make_document(
            kvp( "$set", order_doc.view() ));

      mongocxx::options::update update_opts{};
      update_opts.upsert( true );
      
      if( !_orders.update_one(  make_document( kvp( "scope_orderid", bsoncxx::decimal128{scope,order_id})),
                              make_document( kvp( "$set", order_doc.view() ) ), update_opts ) ){
         EOS_ASSERT( false, chain::mongo_db_insert_fail, "Failed to insert trans ${id}", ("id", order_id) );
      }
   } catch( ... ) {
      handle_mongo_exception( "trans insert", __LINE__ );
   }
   
}

void mongo_match_plugin_impl::recordonedeal(const match::recorddeal_param &deal_data) {
   using namespace bsoncxx::types;
   using bsoncxx::builder::basic::kvp;
   using bsoncxx::builder::basic::make_document;

   auto trans_traces_doc = bsoncxx::builder::basic::document{};

   auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()});

   mongocxx::options::update update_opts{};
   update_opts.upsert( true );

   auto var_deal = deal_data.to_variant();
   auto json = fc::json::to_string(var_deal);

   auto deal_doc = bsoncxx::builder::basic::document{};

   try {
      const auto& value = bsoncxx::from_json( json );
      deal_doc.append( kvp( "deal", value ) );
   } catch( bsoncxx::exception& ) {
      try {
         json = fc::prune_invalid_utf8( json );
         const auto& value = bsoncxx::from_json( json );
         deal_doc.append( kvp( "deal", value ) );
         deal_doc.append( kvp( "non-utf8-purged", b_bool{true} ) );
      } catch( bsoncxx::exception& e ) {
         elog( "Unable to convert block JSON to MongoDB JSON: ${e}", ("e", e.what()) );
         elog( "  JSON: ${j}", ("j", json) );
      }
   }

   deal_doc.append( kvp( "createdAt", b_date{now} ) );

   try {

      if( !_deals.update_one( make_document( kvp( "_id", make_custom_oid() ) ),
                               make_document( kvp( "$set", deal_doc.view() ) ), update_opts ) ) {
         EOS_ASSERT( false, chain::mongo_db_insert_fail, "Failed to insert block ${scope}", ("scope", deal_data.deal_scope) );
      }

   } catch( ... ) {
      handle_mongo_exception( "blocks insert: " + json, __LINE__ );
   }
}

void mongo_match_plugin_impl::cancelorder(const match::cancelorder &cancel_order) {
   using bsoncxx::builder::basic::make_document;
   using bsoncxx::builder::basic::kvp;

   auto order_info = _orders.find_one( make_document( kvp( "scope_orderid", bsoncxx::decimal128{cancel_order.orderscope,cancel_order.orderid})));
   if (!order_info) {
      FC_ASSERT( false, "no order find");
      return ;
   }

   try {
      auto order_doc = bsoncxx::builder::basic::document{};
      order_doc.append( kvp( "order.order_status", match::order_status::Cancel ) );
      auto update_from = make_document(
            kvp( "$set", order_doc.view() ));

      mongocxx::options::update update_opts{};
      update_opts.upsert( true );
      
      if( !_orders.update_one(  make_document( kvp( "scope_orderid", bsoncxx::decimal128{cancel_order.orderscope,cancel_order.orderid})),
                              make_document( kvp( "$set", order_doc.view() ) ), update_opts ) ){
         EOS_ASSERT( false, chain::mongo_db_insert_fail, "Failed to cancel order ${scope} ${id}", ("scope",cancel_order.orderscope)("id", cancel_order.orderid) );
      }
   } catch( ... ) {
      handle_mongo_exception( "trans insert", __LINE__ );
   }
}



void mongo_match_plugin_impl::_process_applied_transaction( const chain::transaction_trace_ptr& t ) {
   if( !start_block_reached ) return;
   match::order match_order;
   for (auto &act_tract: t->action_traces) {
      if (act_tract.act.account == N(sys.match)) {
         if ( act_tract.act.name == N(openorder) ) {
            auto openorder_temp = act_tract.act.data_as<match::openorder>();
            match_order.set_open_value(openorder_temp);
         }
         else if ( act_tract.act.name == N(match) ) {
            auto match_temp = act_tract.act.data_as<match::match>();
            match_order.set_match_value(match_temp);
            if ( !match_order.isempty() ) {
               handleorder(match_order);
            }
         }
         else if ( act_tract.act.name == N(recorddeal) ) {
            auto recorddeal_temp = act_tract.act.data_as<match::recorddeal>();
            handledeal(recorddeal_temp);
         }
         else if ( act_tract.act.name == N(cancelorder) ) {
            auto cancelorder_temp = act_tract.act.data_as<match::cancelorder>();
            cancelorder(cancelorder_temp);
         }
      }
   }

}

mongo_match_plugin_impl::mongo_match_plugin_impl()
{
}

mongo_match_plugin_impl::~mongo_match_plugin_impl() {
   if (!startup) {
      try {
         ilog( "mongo_match_plugin shutdown in process please be patient this can take a few minutes" );
         done = true;
         condition.notify_one();

         consume_thread.join();

         mongo_pool.reset();
      } catch( std::exception& e ) {
         elog( "Exception on mongo_match_plugin shutdown of consume thread: ${e}", ("e", e.what()));
      }
   }
}

void mongo_match_plugin_impl::wipe_database() {
   ilog("mongo db wipe_database");

   auto client = mongo_pool->acquire();
   auto& mongo_conn = *client;

   auto orders = mongo_conn[db_name][orders_col];
   auto deals = mongo_conn[db_name][deals_col];

   orders.drop();
   deals.drop();
   ilog("done wipe_database");
}

void mongo_match_plugin_impl::init() {
   using namespace bsoncxx::types;
   using bsoncxx::builder::basic::make_document;
   using bsoncxx::builder::basic::kvp;
   // Create the native contract accounts manually; sadly, we can't run their contracts to make them create themselves
   // See native_contract_chain_initializer::prepare_database()

   ilog("init mongo");
   try {
      auto client = mongo_pool->acquire();
      auto& mongo_conn = *client;

      auto orders = mongo_conn[db_name][orders_col];
      if( orders.count( make_document()) == 0 ) { 
         orders.create_index( bsoncxx::from_json( R"xxx({ "order_oid" : 1, "_id" : 1 })xxx" ));

         auto deals = mongo_conn[db_name][deals_col];
         deals.create_index( bsoncxx::from_json( R"xxx({ "scope" : 1, "_id" : 1 })xxx" ));
      }
      //todo
   } catch (...) {
      handle_mongo_exception( "mongo init", __LINE__ );
   }

   ilog("starting db plugin thread");

   consume_thread = std::thread( [this] {
      fc::set_os_thread_name( "mongodb" );
      consume_blocks();
   } );

   startup = false;
}

////////////
// mongo_match_plugin
////////////

mongo_match_plugin::mongo_match_plugin()
:my(new mongo_match_plugin_impl)
{
}

mongo_match_plugin::~mongo_match_plugin()
{
}

void mongo_match_plugin::set_program_options(options_description& cli, options_description& cfg)
{
   cfg.add_options()
         ("match_mongodb-queue-size,q", bpo::value<uint32_t>()->default_value(1024),
         "The target queue size between nodeos and MongoDB plugin thread.")
         ("match_mongodb-wipe", bpo::bool_switch()->default_value(false),
         "Required with --replay-blockchain, --hard-replay-blockchain, or --delete-all-blocks to wipe mongo db."
         "This option required to prevent accidental wipe of mongo db.")
         ("match_mongodb-block-start", bpo::value<uint32_t>()->default_value(0),
         "If specified then only abi data pushed to mongodb until specified block is reached.")
         ("match_mongodb-uri,m", bpo::value<std::string>(),
         "MongoDB URI connection string, see: https://docs.mongodb.com/master/reference/connection-string/."
               " If not specified then plugin is disabled. Default database 'match' is used if not specified in URI."
               " Example: mongodb://127.0.0.1:27017/match")
         ;
}

void mongo_match_plugin::plugin_initialize(const variables_map& options)
{
   try {
      if( options.count( "match_mongodb-uri" )) {
         ilog( "initializing mongo_match_plugin" );
         my->configured = true;

         if( options.at( "replay-blockchain" ).as<bool>() || options.at( "hard-replay-blockchain" ).as<bool>() || options.at( "delete-all-blocks" ).as<bool>() ) {
            if( options.at( "match_mongodb-wipe" ).as<bool>()) {
               ilog( "Wiping mongo database on startup" );
               my->wipe_database_on_startup = true;
            } else if( options.count( "match_mongodb-block-start" ) == 0 ) {
               EOS_ASSERT( false, chain::plugin_config_exception, "--mongodb-wipe required with --replay-blockchain, --hard-replay-blockchain, or --delete-all-blocks"
                                 " --mongodb-wipe will remove all EOS collections from mongodb." );
            }
         }

         if( options.count( "match_mongodb-queue-size" )) {
            my->max_queue_size = options.at( "match_mongodb-queue-size" ).as<uint32_t>();
         }

         if( options.count( "match_mongodb-block-start" )) {
            my->start_block_num = options.at( "match_mongodb-block-start" ).as<uint32_t>();
         }

         if( my->start_block_num == 0 ) {
            my->start_block_reached = true;
         }

         std::string uri_str = options.at( "match_mongodb-uri" ).as<std::string>();
         ilog( "connecting to ${u}", ("u", uri_str));
         mongocxx::uri uri = mongocxx::uri{uri_str};
         my->db_name = uri.database();
         if( my->db_name.empty())
            my->db_name = "match";
         my->mongo_pool.emplace(uri);

         // hook up to signals on controller
         chain_plugin* chain_plug = app().find_plugin<chain_plugin>();
         EOS_ASSERT( chain_plug, chain::missing_chain_plugin_exception, ""  );
         auto& chain = chain_plug->chain();
         my->chain_id.emplace( chain.get_chain_id());

         my->applied_transaction_connection.emplace(
               chain.applied_transaction.connect( [&]( std::tuple<const chain::transaction_trace_ptr&, const chain::signed_transaction&> t ) {
                  my->applied_transaction( std::get<0>(t) );
               } ));

         if( my->wipe_database_on_startup ) {
            my->wipe_database();
         }
         my->init();
      } else {
         wlog( "eosio::mongo_match_plugin configured, but no --match_mongodb-uri specified." );
         wlog( "mongo_match_plugin disabled." );
      }
   } FC_LOG_AND_RETHROW()
}

void mongo_match_plugin::plugin_startup()
{
}

void mongo_match_plugin::plugin_shutdown()
{
   my->applied_transaction_connection.reset();

   my.reset();
}

} // namespace eosio
