---
content_title: How to build eosio.system
link_text: How to build eosio.system
---

## Preconditions
Ensure an appropriate version of `eosio.cdt` is installed. Installing `eosio.cdt` from binaries is sufficient, follow the [`eosio.cdt` installation instructions steps](https://developers.eos.io/manuals/eosio.cdt/latest/installation) to install it. To verify if you have `eosio.cdt` installed and its version run the following command

```sh
eosio-cpp -v
```

### Build contracts using the build script

#### To build contracts alone
Run the `build.sh` script in the top directory to build all the contracts.

#### To build the contracts and unit tests
1. Ensure an appropriate version of `eosio` has been built from source and installed. Installing `eosio` from binaries `is not` sufficient. You can find instructions on how to do it in section [Building from Sources](https://developers.eos.io/manuals/eos/latest/install/build-from-source).
2. Run the `build.sh` script in the top directory with the `-t` flag to build all the contracts and the unit tests for these contracts.

### Build contracts manually

To build the `eosio.system` execute the following commands.

On all platforms except macOS:
```sh
cd you_local_path_to/eosio.system/
rm -fr build
mkdir build
cd build
cmake ..
make -j$( nproc )
cd ..
```

For macOS:
```sh
cd you_local_path_to/eosio.system/
rm -fr build
mkdir build
cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
cd ..
```

### After build:
* If the build was configured to also build unit tests, the unit tests executable is placed in the _build/tests_ folder and is named __unit_test__.
* The contracts (both `.wasm` and `.abi` files) are built into their corresponding _build/contracts/\<contract name\>_ folder.
* Finally, simply use __cleos__ to _set contract_ by pointing to the previously mentioned directory for the specific contract.

# How to deploy the eosio.system


## To deploy eosio.system contract execute the following command:
Let's assume your account name to which you want to deploy the contract is `testersystem`
```
cleos set contract testersystem you_local_path_to/eosio.system/build/contracts/eosio.system/ -p testersystem
```
