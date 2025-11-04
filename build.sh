#!/usr/bin/env bash
export PROJECT_DIR=$(dirname "${BASH_SOURCE[0]}")
export ROOT_DIR=${PROJECT_DIR}/

INSTALL_LOCATION="$HOME/flon/cisum.contracts"

set -eo pipefail

function usage() {
  printf "Usage: $0 OPTION...
  -t DIR      Give the directory where Chain Core Lib is installed, and build unit tests.
  -i DIR      Directory to use for installing contracts (Default: ${INSTALL_LOCATION})
  -m TARGET   make target.(Default is empty)
  -y          Non-interactive mode (Uses defaults for each prompt.)
  -h          Print this help menu.
   \\n" "$0" 1>&2
  exit 1
}

BUILD_TESTS=false

if [ $# -ne 0 ]; then
  while getopts "t:c:i:m:yh" opt; do
    case "${opt}" in
    t)
      CHAIN_CORE_INSTALL_DIR=$OPTARG
      BUILD_TESTS=true
    ;;
    i)
      INSTALL_LOCATION=$OPTARG
      ;;
    m)
      MAKE_TARGET=$OPTARG
      ;;
    y)
      NONINTERACTIVE=true
      PROCEED=true
      ;;
    h)
      usage
      ;;
    ?)
      echo "Invalid Option!" 1>&2
      usage
      ;;
    :)
      echo "Invalid Option: -${OPTARG} requires an argument." 1>&2
      usage
      ;;
    *)
      usage
      ;;
    esac
  done
fi

# Source helper functions and variables.
TEST_DIR=${PROJECT_DIR}/tests
. ${ROOT_DIR}/scripts/.environment
. ${ROOT_DIR}/scripts/helper.sh

if [[ ${BUILD_TESTS} == true ]]; then
echo "CHAIN_CORE_INSTALL_DIR=${CHAIN_CORE_INSTALL_DIR}"
  if [[ "$CHAIN_CORE_INSTALL_DIR" == "" ]]; then
    echo "Chain Core Lib directory is required when building tests." 1>&2
    usage
  fi
  if [[ ! -d $CHAIN_CORE_INSTALL_DIR ]]; then
    echo "Chain Core Lib directory does not exist: $CHAIN_CORE_INSTALL_DIR" 1>&2
    usage
  fi

  # Include CHAIN_CORE_INSTALL_DIR in CMAKE_PREFIX_PATH
  echo "Using CHAIN_CORE installation at: $CHAIN_CORE_INSTALL_DIR"
  [[ ! -z ${CMAKE_PREFIX_PATH} ]] && CMAKE_PREFIX_PATH=":${CMAKE_PREFIX_PATH}"
  export CMAKE_PREFIX_PATH="${CHAIN_CORE_INSTALL_DIR}${CMAKE_PREFIX_PATH}"
fi

if [[ "${CMAKE_OPTIONS}" != "" ]]; then
  echo "Using CMAKE_OPTIONS=${CMAKE_OPTIONS}"
fi

printf "\t=========== Building rwafi.contracts ===========\n\n"
RED='\033[0;31m'
NC='\033[0m'
CPU_CORES=$(getconf _NPROCESSORS_ONLN)
mkdir -p build
pushd build &>/dev/null
cmake -DBUILD_TESTS=${BUILD_TESTS} -DCMAKE_INSTALL_PREFIX="${INSTALL_LOCATION}" -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH} ${CMAKE_OPTIONS} ../
make -j $CPU_CORES ${MAKE_TARGET}
popd &>/dev/null
