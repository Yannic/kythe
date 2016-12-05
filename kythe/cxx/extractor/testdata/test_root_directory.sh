#!/bin/bash -e
# Copyright 2015 Google Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# This test checks that the extractor handles transcripts.
# It should be run from the Kythe root.
TEST_NAME="test_root_directory"
. ./kythe/cxx/extractor/testdata/test_common.sh
cd kythe/cxx/extractor/testdata/altroot/altpath
KYTHE_OUTPUT_DIRECTORY="${OUT_DIR}" \
KYTHE_ROOT_DIRECTORY="${BASE_DIR}/altroot" \
    "../../../cxx_extractor" --with_executable "/usr/bin/g++" \
    file.cc
[[ $(ls -1 "${OUT_DIR}"/*.kindex | wc -l) -eq 1 ]]
INDEX_PATH=$(ls -1 "${OUT_DIR}"/*.kindex)
"${KINDEX_TOOL}" -canonicalize_hashes -suppress_details -explode "${INDEX_PATH}"

# Remove lines that will change depending on the machine the test is run on.
skip_inplace "-target" 1 "${INDEX_PATH}_UNIT"
skip_inplace "signature" 0 "${INDEX_PATH}_UNIT"

sed "s|TEST_CWD|${PWD}/|" "${BASE_DIR}/altroot_altpath_file.UNIT" | \
    skip "-target" 1 |
    skip "signature" 0 |
    diff - "${INDEX_PATH}_UNIT"
