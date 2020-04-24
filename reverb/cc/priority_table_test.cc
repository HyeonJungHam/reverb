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

#include "reverb/cc/priority_table.h"

#include <atomic>
#include <cfloat>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <cstdint>
#include "absl/memory/memory.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "reverb/cc/checkpointing/checkpoint.pb.h"
#include "reverb/cc/chunk_store.h"
#include "reverb/cc/distributions/fifo.h"
#include "reverb/cc/distributions/uniform.h"
#include "reverb/cc/platform/thread.h"
#include "reverb/cc/rate_limiter.h"
#include "reverb/cc/schema.pb.h"
#include "reverb/cc/testing/proto_test_util.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/core/status_test_util.h"

namespace deepmind {
namespace reverb {
namespace {

const absl::Duration kTimeout = absl::Milliseconds(250);

using ::deepmind::reverb::testing::Partially;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::SizeIs;

MATCHER_P(HasItemKey, key, "") { return arg.item.key() == key; }

PriorityTableItem MakeItem(uint64_t key, double priority,
                           const std::vector<SequenceRange>& sequences) {
  PriorityTableItem item;

  std::vector<ChunkData> data(sequences.size());
  for (int i = 0; i < sequences.size(); i++) {
    data[i] = testing::MakeChunkData(key * 100 + i, sequences[i]);
    item.chunks.push_back(std::make_shared<ChunkStore::Chunk>(data[i]));
  }

  item.item = testing::MakePrioritizedItem(key, priority, data);

  return item;
}

PriorityTableItem MakeItem(uint64_t key, double priority) {
  return MakeItem(key, priority, {testing::MakeSequenceRange(key * 100, 0, 1)});
}

std::unique_ptr<RateLimiter> MakeLimiter(int64_t min_size) {
  return absl::make_unique<RateLimiter>(1.0, min_size, -DBL_MAX, DBL_MAX);
}

std::unique_ptr<PriorityTable> MakeUniformTable(const std::string& name,
                                                int64_t max_size = 1000,
                                                int32_t max_times_sampled = 0) {
  return absl::make_unique<PriorityTable>(
      name, absl::make_unique<UniformDistribution>(),
      absl::make_unique<FifoDistribution>(), max_size, max_times_sampled,
      MakeLimiter(1));
}

TEST(PriorityTableTest, SetsName) {
  auto first = MakeUniformTable("first");
  auto second = MakeUniformTable("second");
  EXPECT_EQ(first->name(), "first");
  EXPECT_EQ(second->name(), "second");
}

TEST(PriorityTableTest, CopyAfterInsert) {
  auto table = MakeUniformTable("dist");
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 123)));

  auto items = table->Copy();
  ASSERT_THAT(items, SizeIs(1));
  EXPECT_THAT(
      items[0].item,
      Partially(testing::EqualsProto("key: 3 times_sampled: 0 priority: 123")));
}

TEST(PriorityTableTest, CopySubset) {
  auto table = MakeUniformTable("dist");
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 123)));
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(4, 123)));
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(5, 123)));
  EXPECT_THAT(table->Copy(1), SizeIs(1));
  EXPECT_THAT(table->Copy(2), SizeIs(2));
}

TEST(PriorityTableTest, InsertOrAssignOverwrites) {
  auto table = MakeUniformTable("dist");
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 123)));
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 456)));

  auto items = table->Copy();
  ASSERT_THAT(items, SizeIs(1));
  EXPECT_EQ(items[0].item.priority(), 456);
}

TEST(PriorityTableTest, UpdatesAreAppliedPartially) {
  auto table = MakeUniformTable("dist");
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 123)));
  TF_EXPECT_OK(table->MutateItems(
      {
          testing::MakeKeyWithPriority(5, 55),
          testing::MakeKeyWithPriority(3, 456),
      },
      {}));

  auto items = table->Copy();
  ASSERT_THAT(items, SizeIs(1));
  EXPECT_EQ(items[0].item.priority(), 456);
}

TEST(PriorityTableTest, DeletesAreAppliedPartially) {
  auto table = MakeUniformTable("dist");
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 123)));
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(7, 456)));
  TF_EXPECT_OK(table->MutateItems({}, {5, 3}));
  EXPECT_THAT(table->Copy(), ElementsAre(HasItemKey(7)));
}

TEST(PriorityTableTest, SampleBlocksWhenNotEnoughItems) {
  auto table = MakeUniformTable("dist");

  absl::Notification notification;
  auto sample_thread = internal::StartThread("", [&table, &notification] {
    PriorityTable::SampledItem item;
    TF_EXPECT_OK(table->Sample(&item));
    notification.Notify();
  });

  EXPECT_FALSE(notification.WaitForNotificationWithTimeout(kTimeout));

  // Inserting an item should allow the call to complete.
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 123)));
  EXPECT_TRUE(notification.WaitForNotificationWithTimeout(kTimeout));

  sample_thread = nullptr;  // Joins the thread.
}

TEST(PriorityTableTest, SampleMatchesInsert) {
  auto table = MakeUniformTable("dist");

  PriorityTable::Item item = MakeItem(3, 123);
  TF_EXPECT_OK(table->InsertOrAssign(item));

  PriorityTable::SampledItem sample;
  TF_EXPECT_OK(table->Sample(&sample));
  item.item.set_times_sampled(1);
  sample.item.clear_inserted_at();
  EXPECT_THAT(sample.item, testing::EqualsProto(item.item));
  EXPECT_EQ(sample.chunks, item.chunks);
  EXPECT_EQ(sample.probability, 1);
}

TEST(PriorityTableTest, SampleIncrementsSampleTimes) {
  auto table = MakeUniformTable("dist");

  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 123)));

  PriorityTable::SampledItem item;
  EXPECT_EQ(table->Copy()[0].item.times_sampled(), 0);
  TF_EXPECT_OK(table->Sample(&item));
  EXPECT_EQ(table->Copy()[0].item.times_sampled(), 1);
  TF_EXPECT_OK(table->Sample(&item));
  EXPECT_EQ(table->Copy()[0].item.times_sampled(), 2);
}

TEST(PriorityTableTest, MaxTimesSampledIsRespected) {
  auto table = MakeUniformTable("dist", 10, 2);

  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 123)));

  PriorityTable::SampledItem item;
  EXPECT_EQ(table->Copy()[0].item.times_sampled(), 0);
  TF_ASSERT_OK(table->Sample(&item));
  EXPECT_EQ(table->Copy()[0].item.times_sampled(), 1);
  TF_ASSERT_OK(table->Sample(&item));
  EXPECT_THAT(table->Copy(), IsEmpty());
}

TEST(PriorityTableTest, InsertDeletesWhenOverflowing) {
  auto table = MakeUniformTable("dist", 10);

  for (int i = 0; i < 15; i++) {
    TF_EXPECT_OK(table->InsertOrAssign(MakeItem(i, 123)));
  }
  auto items = table->Copy();
  EXPECT_THAT(items, SizeIs(10));
  for (const PriorityTable::Item& item : items) {
    EXPECT_GE(item.item.key(), 5);
    EXPECT_LT(item.item.key(), 15);
  }
}

TEST(PriorityTableTest, ConcurrentCalls) {
  auto table = MakeUniformTable("dist", 1000);

  std::vector<std::unique_ptr<internal::Thread>> bundle;
  std::atomic<int> count(0);
  for (PriorityTable::Key i = 0; i < 1000; i++) {
    bundle.push_back(internal::StartThread("", [i, &table, &count] {
      TF_EXPECT_OK(table->InsertOrAssign(MakeItem(i, 123)));
      PriorityTable::SampledItem item;
      TF_EXPECT_OK(table->Sample(&item));
      TF_EXPECT_OK(
          table->MutateItems({testing::MakeKeyWithPriority(i, 456)}, {i}));
      count++;
    }));
  }
  bundle.clear();  // Joins all threads.
  EXPECT_EQ(count, 1000);
}

TEST(PriorityTableTest, UseAsQueue) {
  PriorityTable queue(
      /*name=*/"queue",
      /*sampler=*/absl::make_unique<FifoDistribution>(),
      /*remover=*/absl::make_unique<FifoDistribution>(),
      /*max_size=*/10,
      /*max_times_sampled=*/1,
      absl::make_unique<RateLimiter>(
          /*samples_per_insert=*/1.0,
          /*min_size_to_sample=*/1,
          /*min_diff=*/0,
          /*max_diff=*/10.0));
  for (int i = 0; i < 10; i++) {
    TF_EXPECT_OK(queue.InsertOrAssign(MakeItem(i, 123)));
  }

  // This should now be blocked
  absl::Notification insert;
  auto insert_thread = internal::StartThread("", [&] {
    TF_EXPECT_OK(queue.InsertOrAssign(MakeItem(10, 123)));
    insert.Notify();
  });

  EXPECT_FALSE(insert.WaitForNotificationWithTimeout(kTimeout));

  for (int i = 0; i < 11; i++) {
    PriorityTable::SampledItem item;
    TF_EXPECT_OK(queue.Sample(&item));
    EXPECT_THAT(item, HasItemKey(i));
  }

  EXPECT_TRUE(insert.WaitForNotificationWithTimeout(kTimeout));

  insert_thread = nullptr;  // Joins the thread.

  EXPECT_EQ(queue.size(), 0);

  // Sampling should now be blocked.
  absl::Notification sample;
  auto sample_thread = internal::StartThread("", [&] {
    PriorityTable::SampledItem item;
    TF_EXPECT_OK(queue.Sample(&item));
    sample.Notify();
  });

  EXPECT_FALSE(sample.WaitForNotificationWithTimeout(kTimeout));

  // Inserting a new item should result in it being sampled straight away.
  TF_EXPECT_OK(queue.InsertOrAssign(MakeItem(100, 123)));
  EXPECT_TRUE(sample.WaitForNotificationWithTimeout(kTimeout));

  EXPECT_EQ(queue.size(), 0);

  sample_thread = nullptr;  // Joins the thread.
}

TEST(PriorityTableTest, ConcurrentInsertOfTheSameKey) {
  PriorityTable table(
      /*name=*/"dist",
      /*sampler=*/absl::make_unique<UniformDistribution>(),
      /*remover=*/absl::make_unique<FifoDistribution>(),
      /*max_size=*/1000,
      /*max_times_sampled=*/0,
      absl::make_unique<RateLimiter>(
          /*samples_per_insert=*/1.0,
          /*min_size_to_sample=*/1,
          /*min_diff=*/-1,
          /*max_diff=*/1));

  // Insert one item to make new inserts block.
  TF_ASSERT_OK(table.InsertOrAssign(MakeItem(1, 123)));  // diff = 1.0

  std::vector<std::unique_ptr<internal::Thread>> bundle;

  // Try to insert the same item 10 times. All should be blocked.
  std::atomic<int> count(0);
  for (int i = 0; i < 10; i++) {
    bundle.push_back(internal::StartThread("", [&] {
      TF_EXPECT_OK(table.InsertOrAssign(MakeItem(10, 123)));
      count++;
    }));
  }

  EXPECT_EQ(count, 0);

  // Making a single sample should unblock one of the inserts. The other inserts
  // are now updates but they are still waiting for their right to insert.
  PriorityTable::SampledItem item;
  TF_EXPECT_OK(table.Sample(&item));

  // Sampling once more would unblock one of the inserts, it will then see that
  // it is now an update and not use its right to insert. Once it releases the
  // lock the same process will follow for all the remaining inserts.
  TF_EXPECT_OK(table.Sample(&item));

  bundle.clear();  // Joins all threads.

  EXPECT_EQ(count, 10);
  EXPECT_EQ(table.size(), 2);
}

TEST(PriorityTableTest, CloseCancelsPendingCalls) {
  PriorityTable table(
      /*name=*/"dist",
      /*sampler=*/absl::make_unique<UniformDistribution>(),
      /*remover=*/absl::make_unique<FifoDistribution>(),
      /*max_size=*/1000,
      /*max_times_sampled=*/0,
      absl::make_unique<RateLimiter>(
          /*samples_per_insert=*/1.0,
          /*min_size_to_sample=*/1,
          /*min_diff=*/-1,
          /*max_diff=*/1));

  // Insert two item to make new inserts block.
  TF_ASSERT_OK(table.InsertOrAssign(MakeItem(1, 123)));  // diff = 1.0

  tensorflow::Status status;
  absl::Notification notification;
  auto thread = internal::StartThread("", [&] {
    status = table.InsertOrAssign(MakeItem(10, 123));
    notification.Notify();
  });

  EXPECT_FALSE(notification.WaitForNotificationWithTimeout(kTimeout));

  table.Close();

  EXPECT_TRUE(notification.WaitForNotificationWithTimeout(kTimeout));
  EXPECT_EQ(status.code(), tensorflow::error::CANCELLED);

  thread = nullptr;  // Joins the thread.
}

TEST(PriorityTableTest, ResetResetsRateLimiter) {
  PriorityTable table(
      /*name=*/"dist",
      /*sampler=*/absl::make_unique<UniformDistribution>(),
      /*remover=*/absl::make_unique<FifoDistribution>(),
      /*max_size=*/1000,
      /*max_times_sampled=*/0,
      absl::make_unique<RateLimiter>(
          /*samples_per_insert=*/1.0,
          /*min_size_to_sample=*/1,
          /*min_diff=*/-1,
          /*max_diff=*/1));

  // Insert two item to make new inserts block.
  TF_ASSERT_OK(table.InsertOrAssign(MakeItem(1, 123)));  // diff = 1.0

  absl::Notification notification;
  auto thread = internal::StartThread("", [&] {
    TF_ASSERT_OK(table.InsertOrAssign(MakeItem(10, 123)));
    notification.Notify();
  });

  EXPECT_FALSE(notification.WaitForNotificationWithTimeout(kTimeout));

  // Resetting the table should unblock new inserts.
  TF_ASSERT_OK(table.Reset());

  EXPECT_TRUE(notification.WaitForNotificationWithTimeout(kTimeout));

  thread = nullptr;  // Joins the thread.
}

TEST(PriorityTableTest, ResetClearsAllData) {
  auto table = MakeUniformTable("dist");
  TF_ASSERT_OK(table->InsertOrAssign(MakeItem(1, 123)));
  EXPECT_EQ(table->size(), 1);
  TF_ASSERT_OK(table->Reset());
  EXPECT_EQ(table->size(), 0);
}

TEST(PriorityTableTest, ResetWhileConcurrentCalls) {
  auto table = MakeUniformTable("dist");
  std::vector<std::unique_ptr<internal::Thread>> bundle;
  for (PriorityTable::Key i = 0; i < 1000; i++) {
    bundle.push_back(internal::StartThread("", [i, &table] {
      if (i % 123 == 0) TF_EXPECT_OK(table->Reset());
      TF_EXPECT_OK(table->InsertOrAssign(MakeItem(i, 123)));
      TF_EXPECT_OK(
          table->MutateItems({testing::MakeKeyWithPriority(i, 456)}, {i}));
    }));
  }
  bundle.clear();  // Joins all threads.
}

TEST(PriorityTableTest, CheckpointOrderItems) {
  auto table = MakeUniformTable("dist");

  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(1, 123)));
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 125)));
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(2, 124)));

  auto checkpoint = table->Checkpoint();
  EXPECT_THAT(checkpoint.checkpoint.items(),
              ElementsAre(Partially(testing::EqualsProto("key: 1")),
                          Partially(testing::EqualsProto("key: 3")),
                          Partially(testing::EqualsProto("key: 2"))));
}

TEST(PriorityTableTest, CheckpointSanityCheck) {
  PriorityTable table("dist", absl::make_unique<UniformDistribution>(),
                      absl::make_unique<FifoDistribution>(), 10, 1,
                      absl::make_unique<RateLimiter>(1.0, 3, -10, 7));

  TF_EXPECT_OK(table.InsertOrAssign(MakeItem(1, 123)));

  auto checkpoint = table.Checkpoint();

  PriorityTableCheckpoint want;
  want.set_table_name("dist");
  want.set_max_size(10);
  want.set_max_times_sampled(1);
  want.add_items()->set_key(1);
  want.mutable_rate_limiter()->set_samples_per_insert(1.0);
  want.mutable_rate_limiter()->set_min_size_to_sample(3);
  want.mutable_rate_limiter()->set_min_diff(-10);

  EXPECT_THAT(checkpoint.checkpoint,
              Partially(testing::EqualsProto("table_name: 'dist' "
                                             "max_size: 10 "
                                             "max_times_sampled: 1 "
                                             "items: { key: 1 } "
                                             "rate_limiter: { "
                                             "  samples_per_insert: 1.0"
                                             "  min_size_to_sample: 3"
                                             "  min_diff: -10"
                                             "  max_diff: 7"
                                             "  sample_count: 0"
                                             "  insert_count: 1"
                                             "} "
                                             "sampler: { uniform: true } "
                                             "remover: { fifo: true } ")));
}

TEST(PriorityTableTest, BlocksSamplesWhenSizeToSmallDueToAutoDelete) {
  PriorityTable table(
      /*name=*/"dist",
      /*sampler=*/absl::make_unique<FifoDistribution>(),
      /*remover=*/absl::make_unique<FifoDistribution>(),
      /*max_size=*/10,
      /*max_times_sampled=*/2,
      absl::make_unique<RateLimiter>(
          /*samples_per_insert=*/1.0,
          /*min_size_to_sample=*/3,
          /*min_diff=*/0,
          /*max_diff=*/5));
  TF_EXPECT_OK(table.InsertOrAssign(MakeItem(1, 1)));
  TF_EXPECT_OK(table.InsertOrAssign(MakeItem(2, 1)));
  TF_EXPECT_OK(table.InsertOrAssign(MakeItem(3, 1)));

  // It should be fine to sample now as the table has been reached its min size.
  PriorityTable::SampledItem sample_1;
  TF_EXPECT_OK(table.Sample(&sample_1));
  EXPECT_THAT(sample_1, HasItemKey(1));

  // A second sample should be fine since the table is still large enough.
  PriorityTable::SampledItem sample_2;
  TF_EXPECT_OK(table.Sample(&sample_2));
  EXPECT_THAT(sample_2, HasItemKey(1));

  // Due to max_times_sampled, the table should have one item less which should
  // block more samples from proceeding.
  absl::Notification notification;
  auto sample_thread = internal::StartThread("", [&] {
    PriorityTable::SampledItem sample;
    TF_EXPECT_OK(table.Sample(&sample));
    notification.Notify();
  });
  EXPECT_FALSE(notification.WaitForNotificationWithTimeout(kTimeout));

  // Inserting a new item should unblock the sampling.
  TF_EXPECT_OK(table.InsertOrAssign(MakeItem(4, 1)));
  EXPECT_TRUE(notification.WaitForNotificationWithTimeout(kTimeout));

  sample_thread = nullptr;  // Joins the thread.
}

TEST(PriorityTableTest, BlocksSamplesWhenSizeToSmallDueToExplicitDelete) {
  PriorityTable table(
      /*name=*/"dist",
      /*sampler=*/absl::make_unique<FifoDistribution>(),
      /*remover=*/absl::make_unique<FifoDistribution>(),
      /*max_size=*/10,
      /*max_times_sampled=*/-1,
      absl::make_unique<RateLimiter>(
          /*samples_per_insert=*/1.0,
          /*min_size_to_sample=*/3,
          /*min_diff=*/0,
          /*max_diff=*/5));
  TF_EXPECT_OK(table.InsertOrAssign(MakeItem(1, 1)));
  TF_EXPECT_OK(table.InsertOrAssign(MakeItem(2, 1)));
  TF_EXPECT_OK(table.InsertOrAssign(MakeItem(3, 1)));

  // It should be fine to sample now as the table has been reached its min size.
  PriorityTable::SampledItem sample_1;
  TF_EXPECT_OK(table.Sample(&sample_1));
  EXPECT_THAT(sample_1, HasItemKey(1));

  // Deleting an item will make the table too small to allow samples.
  TF_EXPECT_OK(table.MutateItems({}, {1}));

  absl::Notification notification;
  auto sample_thread = internal::StartThread("", [&] {
    PriorityTable::SampledItem sample;
    TF_EXPECT_OK(table.Sample(&sample));
    notification.Notify();
  });
  EXPECT_FALSE(notification.WaitForNotificationWithTimeout(kTimeout));

  // Inserting a new item should unblock the sampling.
  TF_EXPECT_OK(table.InsertOrAssign(MakeItem(4, 1)));
  EXPECT_TRUE(notification.WaitForNotificationWithTimeout(kTimeout));

  sample_thread = nullptr;  // Joins the thread.

  // And any new samples should be fine.
  PriorityTable::SampledItem sample_2;
  TF_EXPECT_OK(table.Sample(&sample_2));
  EXPECT_THAT(sample_2, HasItemKey(2));
}

TEST(PriorityTableTest, GetExistingItem) {
  auto table = MakeUniformTable("dist");

  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(1, 1)));
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(2, 1)));
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 1)));

  PriorityTableItem item;
  EXPECT_TRUE(table->Get(2, &item));
  EXPECT_THAT(item, HasItemKey(2));
}

TEST(PriorityTableTest, GetMissingItem) {
  auto table = MakeUniformTable("dist");

  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(1, 1)));
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(3, 1)));

  PriorityTableItem item;
  EXPECT_FALSE(table->Get(2, &item));
}

TEST(PriorityTableTest, SampleSetsTableSize) {
  auto table = MakeUniformTable("dist");

  for (int i = 1; i <= 10; i++) {
    TF_EXPECT_OK(table->InsertOrAssign(MakeItem(i, 1)));
    PriorityTable::SampledItem sample;
    TF_EXPECT_OK(table->Sample(&sample));
    EXPECT_EQ(sample.table_size, i);
  }
}

TEST(PriorityTableDeathTest, DiesIfUnsafeAddExtensionCalledWhenNonEmpty) {
  auto table = MakeUniformTable("dist");
  TF_EXPECT_OK(table->InsertOrAssign(MakeItem(1, 1)));
  ASSERT_DEATH(table->UnsafeAddExtension(nullptr), "");
}

}  // namespace
}  // namespace reverb
}  // namespace deepmind
