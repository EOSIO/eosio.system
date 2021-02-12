#!/bin/bash
set -eo pipefail
INSTALL_CDT='dpkg -i /eosio.system/eosio.cdt.deb'
echo "$ $INSTALL_CDT"
eval $INSTALL_CDT
export PATH=/usr/opt/eosio.cdt/$(ls /usr/opt/eosio.cdt/)/bin:$PATH
cd /root/eosio/
printf "EOSIO commit: $(git rev-parse --verify HEAD). Click \033]1339;url=https://github.com/EOSIO/eos/commit/$(git rev-parse --verify HEAD);content=here\a for details.\n"
echo 'Authenticating to GitHub'
mkdir /root/.ssh
CREATE_PRIVATE_KEY='echo "$BUILDKITE_AGENT_KEY_PRIVATE" > /root/.ssh/id_rsa'
echo "$ $CREATE_PRIVATE_KEY"
eval $CREATE_PRIVATE_KEY
CHMOD_PRIVATE_KEY='chmod 400 /root/.ssh/id_rsa'
echo "$ $CHMOD_PRIVATE_KEY"
eval $CHMOD_PRIVATE_KEY
CREATE_PUBLIC_KEY='echo "$BUILDKITE_AGENT_KEY_PUBLIC" > /root/.ssh/id_rsa.pub'
echo "$ $CREATE_PUBLIC_KEY"
eval $CREATE_PUBLIC_KEY
CHMOD_PUBLIC_KEY='chmod 400 /root/.ssh/id_rsa.pub'
echo "$ $CHMOD_PUBLIC_KEY"
eval $CHMOD_PUBLIC_KEY
SSH_KEYSCAN='ssh-keyscan -H github.com >> ~/.ssh/known_hosts'
echo "$ $SSH_KEYSCAN"
eval $SSH_KEYSCAN
SSH_TEST='ssh -T git@github.com || :'
echo "$ $SSH_TEST"
eval $SSH_TEST
echo 'Authenticated to GitHub'
cd /eosio.system/build
CMAKE='cmake -DBUILD_TESTS=true ..'
echo "$ $CMAKE"
eval $CMAKE
MAKE='make -j $JOBS'
echo "$ $MAKE"
eval $MAKE
