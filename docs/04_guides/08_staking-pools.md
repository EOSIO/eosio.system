---
content_title: Staking pools
link_text: Staking pools
---

# Overview

The staking pool system, when combined with powerup, displaces (over time) the existing staking, voting, producer pay, and REX systems. Here are some of the major differences:

* Users use `powerup` to obtain NET and CPU resources instead of staking (`delegatebw`) or renting from REX.
* Users stake into pools to be able to vote instead of staking using `delegatebw` or buying REX.
* Users stake into pools to receive ram fees, powerup dees, and namebid fees, instead of buying REX.
* If a user votes for `n` producers, then each producer gets `1/n` of that voter's voting power.
* Inflation pays both producers and staking pools. Inflation occurs each round (126 seconds) and is scaled by `pow(produced_blocks / 252, 10)` to encourage the selection of reliable producers.

# Activating on existing chains

This procedure activates staking pools on an existing chain. `powerup` isn't a prerequisite for voter pools; either one may be activated first and it's possible to give them both the same transition period or different transition periods. Note that ram fees and namebid fees will go to the voter pools instead of the REX pool even if you don't activate powerup.

* Upgrade the `eosio.system` contract.
* If the `eosio` account doesn't have unlimited RAM, then you may need to increase it. Rows in the new tables use `eosio` RAM instead of account holders' RAM.
* Create the following accounts:
  * `eosio.vpool`: This account holds tokens for the staking pools.
  * `eosio.bvpay`: This account holds tokens for the new producer pay system.
* Use `cfgvpool` to activate staking pools. Consider setting `begin_transition` several days into the future to give producers and proxies time to upgrade their accounts before the new voting system starts selecting producers.
* After `cfgvpool` is used, producers should each use `regproducer` or `regproducer2` to make themselves eligible to receive pool-based votes.
* Proxies should register using `regpoolproxy` then vote using `votewithpool`. To change their vote, they should use both `voteproducer` and `votewithpool`. Only `votewithpool` is necessary once `end_transition` has passed.
* Voters may gradually use `undelegatebw` and `sellrex` to move stake out of the old systems and use `stake2pool` to move stake into the new system. `voteproducer` votes with their delegated stake and REX. `votewithpool` votes with their pool stake. Both may be used until `end_transition` has passed; at this point only `votewithpool` votes matter. Caution: voters should understand the limits on `claimstake` before using `votewithpool`. Caution: pools don't start receiving inflation until `begin_transition` has passed. They don't receive the full amount until `end_transition` has passed.

# Activating on new chains

This procedure activates staking pools on new chains. Follow the existing procedure, but make the following changes:

* Don't activate REX, unless you plan to use it instead of powerup.
* Don't stake users' tokens using `delegatebw`. Instead, leave their tokens liquid so they may use `stake2pool` and `votewithpool` if they choose.
* The voter threshold activation procedure does not apply.
* Create the following accounts:
  * `eosio.vpool`: This account holds tokens for the staking pools.
  * `eosio.bvpay`: This account holds tokens for the new producer pay system.
* Use `cfgvpool` to activate staking pools. Set both `begin_transition` and `end_transition` to the same timestamp, but in the past so the system will fully activate.
* Activate the powerup system without a transition period.
* Producers should each use `regproducer` or `regproducer2` to make themselves eligible to receive pool-based votes. Do this after `cfgvpool`.
* Proxies should register using `regpoolproxy` and vote using `votewithpool`. `regproxy` and `voteproducer` have no effect, other than consuming proxies' RAM.
* Users should use `powerup` to obtain NET and CPU resources instead of using `delegatebw`. They need to use `stake2pool` to qualify for voting. They may vote using `votewithpool`; `voteproducer` has no effect, other than consuming users' RAM.
