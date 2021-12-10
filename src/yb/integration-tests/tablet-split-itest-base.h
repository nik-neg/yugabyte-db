// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#ifndef YB_INTEGRATION_TESTS_TABLET_SPLIT_ITEST_BASE_H
#define YB_INTEGRATION_TESTS_TABLET_SPLIT_ITEST_BASE_H

#include <chrono>

#include <boost/range/adaptors.hpp>

#include "yb/client/client-test-util.h"
#include "yb/client/ql-dml-test-base.h"
#include "yb/client/snapshot_test_util.h"
#include "yb/client/session.h"
#include "yb/client/table_handle.h"
#include "yb/client/table_info.h"
#include "yb/client/table.h"
#include "yb/client/transaction.h"
#include "yb/client/txn-test-base.h"
#include "yb/client/yb_op.h"

#include "yb/common/ql_expr.h"
#include "yb/consensus/consensus.h"

#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/test_workload.h"

#include "yb/master/catalog_manager.h"

#include "yb/rpc/messenger.h"

#include "yb/util/size_literals.h"
#include "yb/util/tsan_util.h"

DECLARE_int32(replication_factor);

namespace yb {

Result<size_t> SelectRowsCount(
    const client::YBSessionPtr& session, const client::TableHandle& table);

void DumpTableLocations(
    master::CatalogManagerIf* catalog_mgr, const client::YBTableName& table_name);

void DumpWorkloadStats(const TestWorkload& workload);

CHECKED_STATUS SplitTablet(master::CatalogManagerIf* catalog_mgr, const tablet::Tablet& tablet);

CHECKED_STATUS DoSplitTablet(master::CatalogManagerIf* catalog_mgr, const tablet::Tablet& tablet);

template <class MiniClusterType>
class TabletSplitITestBase : public client::TransactionTestBase<MiniClusterType> {
 protected:
  static constexpr std::chrono::duration<int64> kRpcTimeout = 60s * kTimeMultiplier;
  static constexpr int kDefaultNumRows = 500;
  // We set small data block size, so we don't have to write much data to have multiple blocks.
  // We need multiple blocks to be able to detect split key (see BlockBasedTable::GetMiddleKey).
  static constexpr size_t kDbBlockSizeBytes = 2_KB;
 public:
  void SetUp() override;

  // Creates read request for tablet_id which reflects following query (see
  // client::KeyValueTableTest for schema and kXxx constants):
  // SELECT `kValueColumn` FROM `kTableName` WHERE `kKeyColumn` = `key`;
  // Uses YBConsistencyLevel::CONSISTENT_PREFIX as this is default for YQL clients.
  Result<tserver::ReadRequestPB> CreateReadRequest(const TabletId& tablet_id, int32_t key);

  // Creates write request for tablet_id which reflects following query (see
  // client::KeyValueTableTest for schema and kXxx constants):
  // INSERT INTO `kTableName`(`kValueColumn`) VALUES (`value`);
  tserver::WriteRequestPB CreateInsertRequest(
      const TabletId& tablet_id, int32_t key, int32_t value);

  // Writes `num_rows` rows into test table using `CreateInsertRequest`.
  // Returns a pair with min and max hash code written.
  Result<std::pair<docdb::DocKeyHash, docdb::DocKeyHash>> WriteRows(
      size_t num_rows = 2000, size_t start_key = 1);

  CHECKED_STATUS FlushTestTable();

  Result<std::pair<docdb::DocKeyHash, docdb::DocKeyHash>> WriteRowsAndFlush(
      const size_t num_rows = kDefaultNumRows, const size_t start_key = 1);

  Result<docdb::DocKeyHash> WriteRowsAndGetMiddleHashCode(size_t num_rows);

  Result<scoped_refptr<master::TabletInfo>> GetSingleTestTabletInfo(
      master::CatalogManagerIf* catalog_manager);

  void CreateSingleTablet() {
    this->SetNumTablets(1);
    this->CreateTable();
  }

  CHECKED_STATUS CheckRowsCount(size_t expected_num_rows) {
    auto rows_count = VERIFY_RESULT(SelectRowsCount(this->NewSession(), this->table_));
    SCHECK_EQ(rows_count, expected_num_rows, InternalError, "Got unexpected rows count");
    return Status::OK();
  }

  Result<TableId> GetTestTableId() {
    return VERIFY_RESULT(this->client_->GetYBTableInfo(client::kTableName)).table_id;
  }

  // Make sure table contains only keys 1...num_keys without gaps.
  void CheckTableKeysInRange(const size_t num_keys);

 protected:
  virtual int64_t GetRF() { return 3; }

  std::unique_ptr<rpc::ProxyCache> proxy_cache_;

};
// Let compiler know about these explicit specializations since below subclasses inherit from them.
extern template class TabletSplitITestBase<MiniCluster>;
extern template class TabletSplitITestBase<ExternalMiniCluster>;


class TabletSplitITest : public TabletSplitITestBase<MiniCluster> {
 public:
  std::unique_ptr<client::SnapshotTestUtil> snapshot_util_;
  void SetUp() override;

  Result<TabletId> CreateSingleTabletAndSplit(size_t num_rows);

  Result<tserver::GetSplitKeyResponsePB> GetSplitKey(const std::string& tablet_id);

  Result<master::CatalogManagerIf*> catalog_manager() {
    return &CHECK_NOTNULL(VERIFY_RESULT(cluster_->GetLeaderMiniMaster()))->catalog_manager();
  }

  Result<master::TabletInfos> GetTabletInfosForTable(const TableId& table_id) {
    return VERIFY_RESULT(catalog_manager())->GetTableInfo(table_id)->GetTablets();
  }

  // By default we wait until all split tablets are cleanup. expected_split_tablets could be
  // overridden if needed to test behaviour of split tablet when its deletion is disabled.
  // If num_replicas_online is 0, uses replication factor.
  CHECKED_STATUS WaitForTabletSplitCompletion(
      const size_t expected_non_split_tablets, const size_t expected_split_tablets = 0,
      size_t num_replicas_online = 0, const client::YBTableName& table = client::kTableName,
      bool core_dump_on_failure = true);

  Result<TabletId> SplitSingleTablet(docdb::DocKeyHash split_hash_code);

  Result<TabletId> SplitTabletAndValidate(
      docdb::DocKeyHash split_hash_code,
      size_t num_rows,
      bool parent_tablet_protected_from_deletion = false);

  // Checks source tablet behaviour after split:
  // - It should reject reads and writes.
  CHECKED_STATUS CheckSourceTabletAfterSplit(const TabletId& source_tablet_id);

  // Tests appropriate client requests structure update at YBClient side.
  // split_depth specifies how deep should we split original tablet until trying to write again.
  void SplitClientRequestsIds(int split_depth);

  // Returns all tablet peers in the cluster which are marked as being in
  // TABLET_DATA_SPLIT_COMPLETED state. In most of the test cases below, this corresponds to the
  // post-split parent/source tablet peers.
  Result<std::vector<tablet::TabletPeerPtr>> ListSplitCompleteTabletPeers();

  // Returns all tablet peers in the cluster which are not part of a transaction table and which are
  // not in TABLET_DATA_SPLIT_COMPLETED state. In most of the test cases below, this corresponds to
  // post-split children tablet peers.
  Result<std::vector<tablet::TabletPeerPtr>> ListPostSplitChildrenTabletPeers();

  // Wait for all peers to complete post-split compaction.
  CHECKED_STATUS WaitForTestTablePostSplitTabletsFullyCompacted(MonoDelta timeout);

  Result<int> NumPostSplitTabletPeersFullyCompacted();

  // Returns the bytes read at the RocksDB layer by each split child tablet.
  Result<uint64_t> GetActiveTabletsBytesRead();

  // Returns the bytes written at the RocksDB layer by the split parent tablet.
  Result<uint64_t> GetInactiveTabletsBytesWritten();

  // Returns the smallest sst file size among all replicas for a given tablet id
  Result<uint64_t> GetMinSstFileSizeAmongAllReplicas(const std::string& tablet_id);

  // Checks active tablet replicas (all expect ones that have been split) to have all rows from 1 to
  // `num_rows` and nothing else.
  // If num_replicas_online is 0, uses replication factor.
  CHECKED_STATUS CheckPostSplitTabletReplicasData(
      size_t num_rows, size_t num_replicas_online = 0, size_t num_active_tablets = 2);
 protected:
  MonoDelta split_completion_timeout_ = 40s * kTimeMultiplier;
};


class TabletSplitExternalMiniClusterITest : public TabletSplitITestBase<ExternalMiniCluster> {
 public:
  void SetFlags() override;

  CHECKED_STATUS SplitTablet(const std::string& tablet_id);

  CHECKED_STATUS FlushTabletsOnSingleTServer(
      int tserver_idx, const std::vector<yb::TabletId> tablet_ids, bool is_compaction);

  Result<std::set<TabletId>> GetTestTableTabletIds(int tserver_idx);

  Result<std::set<TabletId>> GetTestTableTabletIds();

  Result<vector<tserver::ListTabletsResponsePB_StatusAndSchemaPB>> ListTablets(int tserver_idx);

  Result<vector<tserver::ListTabletsResponsePB_StatusAndSchemaPB>> ListTablets();

  CHECKED_STATUS WaitForTabletsExcept(
      int num_tablets, int tserver_idx, const TabletId& exclude_tablet);

  CHECKED_STATUS WaitForTablets(int num_tablets, int tserver_idx);

  CHECKED_STATUS WaitForTablets(int num_tablets);

  CHECKED_STATUS SplitTabletCrashMaster(bool change_split_boundary, string* split_partition_key);

  Result<TabletId> GetOnlyTestTabletId(int tserver_idx);

  Result<TabletId> GetOnlyTestTabletId();
};

}  // namespace yb

#endif /* YB_INTEGRATION_TESTS_TABLET_SPLIT_ITEST_BASE_H */