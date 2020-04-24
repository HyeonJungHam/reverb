// Copyright 2019 DeepMind Technologies Limited.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "reverb/cc/platform/net.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "reverb/cc/platform/logging.h"

namespace deepmind::reverb::internal{
namespace {


TEST(Net, PickUnusedPortOrDie) {
  int port0 = PickUnusedPortOrDie();
  int port1 = PickUnusedPortOrDie();
  REVERB_CHECK_GE(port0, 0);
  REVERB_CHECK_LT(port0, 65536);
  REVERB_CHECK_GE(port1, 0);
  REVERB_CHECK_LT(port1, 65536);
  REVERB_CHECK_NE(port0, port1);
}

}  // namespace
}  // namespace deepmind::reverb::internal
