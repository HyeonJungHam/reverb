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

#include "reverb/cc/table_extensions/insert_on_sample.h"

#include <memory>
#include <string>
#include <vector>

#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "reverb/cc/platform/logging.h"
#include "reverb/cc/schema.pb.h"

namespace deepmind::reverb {

const absl::string_view kUndefinedName = "__UNDEFINED__";

InsertOnSampleExtension::InsertOnSampleExtension(
    std::shared_ptr<Table> target_table, absl::Duration timeout)
    : target_table_(std::move(target_table)),
      table_name_(kUndefinedName),
      timeout_(timeout) {}

void InsertOnSampleExtension::ApplyOnSample(const ExtensionItem& item) {
  // Only insert the item into the target table the first time the item is
  // sampled.
  if (item.times_sampled != 1) return;

  // Make a copy of the item in the source table.
  PrioritizedItem copy = item.ref->AsPrioritizedItem();

  // Clear the inserted_at but we keep the same `key` and `times_sampled` (1).
  // Keeping the same key allows the user to send priority updates to the target
  // table straight away.
  copy.set_table(target_table_->name());
  copy.clear_inserted_at();

  // Insert the item into the target table.
  auto status = target_table_->InsertOrAssign(
      Table::Item(std::move(copy), item.ref->chunks()), timeout_);
  REVERB_LOG_IF(REVERB_WARNING, !status.ok())
      << "Unexpected error when copying item "
      << "from table '" << item.ref->table() << "' to table '"
      << target_table_->name() << "': " << status;
}

void InsertOnSampleExtension::AfterRegisterTable(const Table& table) {
  table_name_ = table.name();
}
void InsertOnSampleExtension::BeforeUnregisterTable(const Table& table) {
  table_name_ = kUndefinedName;
}

std::string InsertOnSampleExtension::DebugString() const {
  return absl::StrFormat("InsertOnSampleExtension(source=%s, target=%s)",
                         table_name_, target_table_->name());
}

void InsertOnSampleExtension::OnCheckpointLoaded(
    const std::vector<std::shared_ptr<Table>>& tables) {
  for (const auto& table : tables) {
    if (table->name() == target_table_->name()) {
      target_table_ = table;
      return;
    }
  }
  REVERB_CHECK(false) << "Target table (" << target_table_->name()
                      << ") not found in list of loaded tables";
}

}  // namespace deepmind::reverb
