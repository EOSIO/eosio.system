#!/bin/bash
set -eo pipefail
dpkg -i $MOUNTED_DIR/eosio.cdt.deb
export PATH=/usr/opt/eosio.cdt/$(ls /usr/opt/eosio.cdt/)/bin:$PATH
cd /root/eosio/
printf "EOSIO commit: $(git rev-parse --verify HEAD). Click \033]1339;url=https://github.com/EOSIO/eos/commit/$(git rev-parse --verify HEAD);content=here\a for details.\n"
echo 'Authenticating to GitHub'
mkdir /root/.ssh
echo "$BUILDKITE_AGENT_KEY_PRIVATE" > /root/.ssh/id_rsa
echo "$BUILDKITE_AGENT_KEY_PUBLIC" > /root/.ssh/id_rsa.pub
chmod 400 /root/.ssh/id_rsa
chmod 400 /root/.ssh/id_rsa.pub
ssh -T git@github.com || :
echo 'Authenticated to GitHub'
cd $MOUNTED_DIR/build
cmake -DBUILD_TESTS=true ..
make -j $JOBS
