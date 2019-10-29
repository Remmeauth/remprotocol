#include <eosio/datastream.hpp>
#include <eosio/eosio.hpp>
#include <eosio/multi_index.hpp>
#include <eosio/privileged.hpp>
#include <eosio/serialize.hpp>
#include <eosio/transaction.hpp>

#include <rem.system/rem.system.hpp>
#include <rem.token/rem.token.hpp>

#include <cmath>
#include <map>
#include <algorithm>

#include "name_bidding.cpp"
// Unfortunately, this is needed until CDT fixes the duplicate symbol error with eosio::send_deferred

namespace eosiosystem {

   using eosio::asset;
   using eosio::const_mem_fun;
   using eosio::current_time_point;
   using eosio::indexed_by;
   using eosio::permission_level;
   using eosio::seconds;
   using eosio::time_point_sec;
   using eosio::token;

   using std::map;
   using std::pair;
   using namespace std::string_literals;


   struct [[eosio::table, eosio::contract("rem.system")]] refund_request {
      name            owner;
      time_point_sec  request_time;
      eosio::asset    resource_amount;

      bool is_empty()const { return resource_amount.amount == 0; }
      uint64_t  primary_key()const { return owner.value; }

      // explicit serialization macro is not necessary, used here only to improve compilation time
      EOSLIB_SERIALIZE( refund_request, (owner)(request_time)(resource_amount) )
   };

   /**
    *  These tables are designed to be constructed in the scope of the relevant user, this
    *  facilitates simpler API for per-user queries
    */
   typedef eosio::multi_index< "refunds"_n, refund_request >      refunds_table;

   void validate_b1_vesting( int64_t stake ) {
      const int64_t base_time = 1527811200; /// 2018-06-01
      const int64_t max_claimable = 100'000'000'0000ll;
      const int64_t claimable = int64_t(max_claimable * double(current_time_point().sec_since_epoch() - base_time) / (10*seconds_per_year) );

      check( max_claimable - claimable <= stake, "b1 can only claim their tokens over 10 years" );
   }

   void system_contract::changebw( name from, const name& receiver,
                                   const asset& stake_delta, bool transfer )
   {
      require_auth( from );
      check( stake_delta.amount != 0, "should stake non-zero amount" );

      name source_stake_from = from;
      if ( transfer ) {
         from = receiver;
      }

      // update stake delegated from "from" to "receiver"
      {
         del_bandwidth_table     del_tbl( get_self(), from.value );
         auto itr = del_tbl.find( receiver.value );
         if( itr == del_tbl.end() ) {
            itr = del_tbl.emplace( from, [&]( auto& dbo ){
                  dbo.from          = from;
                  dbo.to            = receiver;
                  dbo.net_weight    = stake_delta;
                  dbo.cpu_weight    = stake_delta;
               });
         }
         else {
            del_tbl.modify( itr, same_payer, [&]( auto& dbo ){
                  dbo.net_weight    += stake_delta;
                  dbo.cpu_weight    += stake_delta;
               });
         }
         check( 0 <= itr->net_weight.amount, "insufficient staked net bandwidth" );
         check( 0 <= itr->cpu_weight.amount, "insufficient staked cpu bandwidth" );
         if ( itr->is_empty() ) {
            del_tbl.erase( itr );
         }
      } // itr can be invalid, should go out of scope

      // update totals of "receiver"
      {
         user_resources_table   totals_tbl( get_self(), receiver.value );
         auto tot_itr = totals_tbl.find( receiver.value );
         if( tot_itr ==  totals_tbl.end() ) {
            tot_itr = totals_tbl.emplace( from, [&]( auto& tot ) {
                  tot.owner = receiver;
                  tot.net_weight    = stake_delta;
                  tot.cpu_weight    = stake_delta;
                  if (from == receiver) {
                     tot.own_stake_amount = stake_delta.amount;
                  }
               });
         } else {
            totals_tbl.modify( tot_itr, from == receiver ? from : same_payer, [&]( auto& tot ) {
                  tot.net_weight    += stake_delta;
                  tot.cpu_weight    += stake_delta;
                  if (from == receiver) {
                     tot.own_stake_amount += stake_delta.amount;
                     //we have to decrease free bytes in case of own stake
                     const int64_t new_free_stake_amount = tot.free_stake_amount - tot.own_stake_amount;
                     tot.free_stake_amount = std::max(new_free_stake_amount, 0LL);
                  }
               });
         }
         check( 0 <= tot_itr->net_weight.amount, "insufficient staked total net bandwidth" );
         check( 0 <= tot_itr->cpu_weight.amount, "insufficient staked total cpu bandwidth" );
         check( _gstate.min_account_stake <= tot_itr->own_stake_amount + tot_itr->free_stake_amount, "insufficient minimal account stake for " + receiver.to_string() );

         {
            bool ram_managed = false;
            bool net_managed = false;
            bool cpu_managed = false;

            auto voter_itr = _voters.find( receiver.value );
            if( voter_itr != _voters.end() ) {
               ram_managed = has_field( voter_itr->flags1, voter_info::flags1_fields::ram_managed );
               net_managed = has_field( voter_itr->flags1, voter_info::flags1_fields::net_managed );
               cpu_managed = has_field( voter_itr->flags1, voter_info::flags1_fields::cpu_managed );
            }
            if( !(net_managed && cpu_managed && ram_managed) ) {
               int64_t ram_bytes = 0;
               int64_t net       = 0;
               int64_t cpu       = 0;
               get_resource_limits( receiver, ram_bytes, net, cpu );

               const auto system_token_max_supply = eosio::token::get_max_supply(token_account, system_contract::get_core_symbol().code() );
               const double bytes_per_token = (double)_gstate.max_ram_size / (double)system_token_max_supply.amount;
               int64_t bytes_for_stake = bytes_per_token * (tot_itr->own_stake_amount + tot_itr->free_stake_amount);
               set_resource_limits( receiver,
                                    ram_managed ? ram_bytes : bytes_for_stake,
                                    net_managed ? net : tot_itr->net_weight.amount + tot_itr->free_stake_amount,
                                    cpu_managed ? cpu : tot_itr->cpu_weight.amount + tot_itr->free_stake_amount);
            }
         }
      } // tot_itr can be invalid, should go out of scope

      // create refund or update from existing refund
      if ( stake_account != source_stake_from ) { //for eosio.stake both transfer and refund make no sense
         refunds_table refunds_tbl( get_self(), from.value );
         auto req = refunds_tbl.find( from.value );

         //create/update/delete refund
         auto temp_balance = stake_delta;
         bool need_deferred_trx = false;


         // net and cpu are same sign by assertions in delegatebw and undelegatebw
         // redundant assertion also at start of changebw to protect against misuse of changebw
         bool is_undelegating = stake_delta.amount < 0;
         bool is_delegating_to_self = (!transfer && from == receiver);

         if( is_delegating_to_self || is_undelegating ) {
            if ( req != refunds_tbl.end() ) { //need to update refund
               refunds_tbl.modify( req, same_payer, [&]( refund_request& r ) {
                  if ( temp_balance.amount < 0 ) {
                     r.request_time = current_time_point();
                  }
                  r.resource_amount -= temp_balance;
                  if ( r.resource_amount.amount < 0 ) {
                     temp_balance = -r.resource_amount;
                     r.resource_amount.amount = 0;
                  } else {
                     temp_balance.amount = 0;
                  }
               });

               check( 0 <= req->resource_amount.amount, "negative net refund amount" ); //should never happen

               if ( req->is_empty() ) {
                  refunds_tbl.erase( req );
                  need_deferred_trx = false;
               } else {
                  need_deferred_trx = true;
               }
            } else if ( temp_balance.amount < 0 ) { //need to create refund
               refunds_tbl.emplace( from, [&]( refund_request& r ) {
                  r.owner = from;
                  r.resource_amount = -temp_balance;
                  r.request_time = current_time_point();
               });
               temp_balance.amount = 0;
               need_deferred_trx = true;
            } // else stake increase requested with no existing row in refunds_tbl -> nothing to do with refunds_tbl
         } /// end if is_delegating_to_self || is_undelegating

         if ( need_deferred_trx ) {
            eosio::transaction out;
            out.actions.emplace_back( permission_level{from, active_permission},
                                      get_self(), "refund"_n,
                                      from
            );
            out.delay_sec = refund_delay_sec;
            eosio::cancel_deferred( from.value ); // TODO: Remove this line when replacing deferred trxs is fixed
            out.send( from.value, from, true );
         } else {
            eosio::cancel_deferred( from.value );
         }

         auto transfer_amount = temp_balance;
         if ( 0 < transfer_amount.amount ) {
            token::transfer_action transfer_act{ token_account, { {source_stake_from, active_permission} } };
            transfer_act.send( source_stake_from, stake_account, asset(transfer_amount), "stake bandwidth" );
         }
      }

      vote_stake_updater( from );
      update_voting_power( from, stake_delta );
   }

   void system_contract::update_voting_power( const name& voter, const asset& total_update )
   {
      auto voter_itr = _voters.find( voter.value );
      if( voter_itr == _voters.end() ) {
         voter_itr = _voters.emplace( voter, [&]( auto& v ) {
            v.owner  = voter;
            v.staked = total_update.amount;
            v.locked_stake = total_update.amount;
         });
      } else {
         _voters.modify( voter_itr, same_payer, [&]( auto& v ) {
            v.staked += total_update.amount;
            v.locked_stake += total_update.amount;
         });
      }

      check( 0 <= voter_itr->staked, "stake for voting cannot be negative" );

      if( voter == "b1"_n ) {
         validate_b1_vesting( voter_itr->staked );
      }

      if( voter_itr->producers.size() || voter_itr->proxy ) {
         update_votes( voter, voter_itr->proxy, voter_itr->producers, false );
      }
   }

   void system_contract::delegatebw( const name& from, const name& receiver,
                                     const asset& stake_quantity,
                                     bool transfer )
   {
      asset zero_asset( 0, core_symbol() );
      check( stake_quantity >= zero_asset, "must stake a positive amount" );
      check( !transfer || from != receiver, "cannot use transfer flag if delegating to self" );
      changebw( from, receiver, stake_quantity, transfer);

      const auto ct = current_time_point();
      // apply stake-lock to those who received stake
      const auto& voter = _voters.get( transfer ? receiver.value : from.value, "user has no resources");
      _voters.modify( voter, same_payer, [&]( auto& v ) {
         const auto restake_rate = double(stake_quantity.amount) / v.staked;
         const auto prevstake_rate = 1.0 - restake_rate;
         const auto time_to_stake_unlock = std::max( v.stake_lock_time - ct, microseconds{} );

         v.stake_lock_time = ct
               + microseconds{ static_cast< int64_t >( prevstake_rate * time_to_stake_unlock.count() ) }
               + microseconds{ static_cast< int64_t >( restake_rate * _gremstate.stake_lock_period.count() ) };
      });
   } // delegatebw

   void system_contract::undelegatebw( const name& from, const name& receiver,
                                       const asset& unstake_quantity)
   {
      asset zero_asset( 0, core_symbol() );
      check( unstake_quantity >= zero_asset, "must unstake a positive amount" );
      check( _gstate.total_activated_stake >= min_activated_stake,
             "cannot undelegate bandwidth until the chain is activated (at least 15% of all tokens participate in voting)" );


      const auto ct = current_time_point();
      const auto& voter = _voters.get(from.value, "user has no resources");
      check(voter.stake_lock_time <= ct, "cannot undelegate during stake lock period");

      // apply unlock rules only within _gremstate.stake_unlock_period
      if ( voter.last_undelegate_time <= voter.stake_lock_time + _gremstate.stake_unlock_period ) {
         check(voter.locked_stake != 0 && voter.locked_stake >= unstake_quantity.amount, "insufficient locked amount");
         check( ct - voter.last_undelegate_time > eosio::days(1), "already undelegated within past day" );

         const auto unclaimed_days = voter.last_undelegate_time.time_since_epoch().count() ? (ct - voter.last_undelegate_time).count() / eosio::days( 1 ).count()
                                                                                             : 1;

         const auto unlock_period_in_days = _gremstate.stake_unlock_period.count() / eosio::days( 1 ).count();

         auto undelegate_limit = voter.locked_stake * unclaimed_days / unlock_period_in_days;
         check(unstake_quantity.amount <= undelegate_limit, "insufficient unlocked amount: "s + asset(undelegate_limit, core_symbol()).to_string());

         _voters.modify(voter, same_payer, [&](auto &v) {
               v.last_undelegate_time = ct;
         });
      }

      changebw( from, receiver, -unstake_quantity , false);
   } // undelegatebw


   void system_contract::refund( const name& owner ) {
      require_auth( owner );

      refunds_table refunds_tbl( get_self(), owner.value );
      auto req = refunds_tbl.find( owner.value );
      check( req != refunds_tbl.end(), "refund request not found" );
      check( req->request_time + seconds(refund_delay_sec) <= current_time_point(),
             "refund is not available yet" );
      token::transfer_action transfer_act{ token_account, { {stake_account, active_permission}, {req->owner, active_permission} } };
      transfer_act.send( stake_account, req->owner, req->resource_amount, "unstake" );
      refunds_tbl.erase( req );
   }


   void system_contract::refundtostake( const name& owner ) {
      require_auth( owner );

      refunds_table refunds_tbl( get_self(), owner.value );
      auto req = refunds_tbl.get( owner.value, "refund request not found" );
      check( req.request_time + seconds(refund_delay_sec) <= current_time_point(), "refund is not available yet" );

      changebw( owner, owner, req.resource_amount, false );
   }

} //namespace eosiosystem
