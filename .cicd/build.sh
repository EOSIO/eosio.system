#!/bin/bash
set -eo pipefail
. ./.cicd/helpers/buildkite.sh
. ./.cicd/helpers/general.sh
. ./.cicd/helpers/dependency-info.sh
mkdir -p $BUILD_DIR
DOCKER_REGISTRY="${MIRROR_REGISTRY:-docker.io/eosio/ci-contracts-builder}"
[[ -z "$DOCKER_IMAGE" ]] && export DOCKER_IMAGE="$DOCKER_REGISTRY:base-ubuntu-18.04-$SANITIZED_EOSIO_VERSION"
if [[ "$BUILDKITE" == 'true' ]]; then
    buildkite-agent meta-data set cdt-url "$CDT_URL"
    buildkite-agent meta-data set cdt-version "$CDT_VERSION"
    buildkite-agent meta-data set docker-image "$DOCKER_IMAGE"
else
    export CDT_URL
    export CDT_VERSION
    export DOCKER_IMAGE
fi
export SSH_AUTH_SOCK="$(readlink -f "$SSH_AUTH_SOCK")" # resolve symlinks
# Test CDT binary download to prevent failures due to eosio.cdt pipeline.
INDEX='1'
echo "$ curl -sSf $CDT_URL --output eosio.cdt.deb"
while ! $(curl -sSf $CDT_URL --output eosio.cdt.deb); do
    echo "ERROR: Expected CDT binary for commit ${CDT_COMMIT} from $CDT_VERSION. It does not exist at $CDT_URL!"
    printf "There must be a successful build against ${CDT_COMMIT} \033]1339;url=https://buildkite.com/EOSIO/eosio-dot-cdt/builds?commit=$CDT_COMMIT;content=here\a for this package to exist.\n"
    echo "Attempt $INDEX, retry in 60 seconds..."
    echo ''
    INDEX=$(( $INDEX + 1 ))
    sleep 60
done
# retry docker pull to protect against failures due to race conditions with eosio pipeline
INDEX='1'
echo "$ docker pull $DOCKER_IMAGE"
while [[ "$(docker pull $DOCKER_IMAGE 2>&1 | grep -ice "manifest for $DOCKER_IMAGE not found")" != '0' ]]; do
    echo "ERROR: Docker image \"$DOCKER_IMAGE\" not found for eosio \"$EOSIO_VERSION\""'!'
    printf "There must be a successful build against ${EOSIO_VERSION} \033]1339;url=${EOSIO_BK_URL};content=here\a for this container to exist.\n"
    echo "Attempt $INDEX, retry in 60 seconds..."
    echo ''
    INDEX=$(( $INDEX + 1 ))
    sleep 60
done
# run
DOCKER_RUN="docker run --rm -v '$(pwd):$MOUNTED_DIR' -w '$MOUNTED_DIR' -e BUILDKITE_AGENT_KEY_PUBLIC -e BUILDKITE_AGENT_KEY_PRIVATE $(buildkite-intrinsics) $DOCKER_IMAGE bash -c './scripts/build.sh'"
echo "$ $DOCKER_RUN"
eval $DOCKER_RUN
