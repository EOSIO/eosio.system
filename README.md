# eosio.system

## Version : 1.9.2

The design of the EOSIO blockchain calls for a number of smart contracts that are run at a privileged permission level in order to support functions such as block producer registration and voting, token staking for CPU and network bandwidth, RAM purchasing, multi-sig, etc.  These smart contracts are referred to as the bios, boot, system, msig, wrap (formerly known as sudo) and token contracts.

This repository contains examples of these privileged contracts that are useful when deploying, managing, and/or using an EOSIO blockchain.  They are provided for reference purposes:

   * [eosio.system](./contracts/eosio.system)

Dependencies:
* [eosio.cdt v1.8.x](https://github.com/EOSIO/eosio.cdt/releases/tag/v1.8.0-rc1)
* [eosio v2.1.x](https://github.com/EOSIO/eos/releases/tag/v2.1.0-rc2) (optional dependency only needed to build unit tests)

## Build

To build the contracts follow the instructions in [Build and deploy](https://developers.eos.io/manuals/eosio.contracts/latest/build-and-deploy) section.

**NOTE** This contract pulls down and builds the [eosio.token](https://github.com/eosio/eosio.token) contract as certain token actions are a requirement for some of the systems.

## Contributing

[Contributing Guide](./CONTRIBUTING.md)

[Code of Conduct](./CONTRIBUTING.md#conduct)

## License

[MIT](./LICENSE)

The included icons are provided under the same terms as the software and accompanying documentation, the MIT License.  We welcome contributions from the artistically-inclined members of the community, and if you do send us alternative icons, then you are providing them under those same terms.

## Important

See [LICENSE](./LICENSE) for copyright and license terms.

All repositories and other materials are provided subject to the terms of this [IMPORTANT](./IMPORTANT.md) notice and you must familiarize yourself with its terms.  The notice contains important information, limitations and restrictions relating to our software, publications, trademarks, third-party resources, and forward-looking statements.  By accessing any of our repositories and other materials, you accept and agree to the terms of the notice.
