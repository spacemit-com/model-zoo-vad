#!/usr/bin/env bash
#
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
#

set -euo pipefail

module_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_dir="${SROBOTIS_TEST_ARTIFACT_DIR:-${module_dir}/build/test-artifacts}"
binary="${artifact_dir}/vad_pr_contract_test"
cxx="${CXX:-c++}"

mkdir -p "${artifact_dir}"

"${cxx}" -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter \
  -I"${module_dir}/include" \
  -I"${module_dir}/src" \
  "${module_dir}/tests/vad_pr_contract_test.cpp" \
  "${module_dir}/src/vad_presets.cpp" \
  -o "${binary}"

"${binary}" --config-and-backend-contract
