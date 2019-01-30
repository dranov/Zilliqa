/*
 * Copyright (C) 2019 Zilliqa
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <array>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <boost/multiprecision/cpp_int.hpp>
#pragma GCC diagnostic pop
#include <chrono>
#include <functional>
#include <thread>

#include "Node.h"
#include "common/Constants.h"
#include "common/Messages.h"
#include "common/Serializable.h"
#include "depends/common/RLP.h"
#include "depends/libDatabase/MemoryDB.h"
#include "depends/libTrie/TrieDB.h"
#include "depends/libTrie/TrieHash.h"
#include "libConsensus/ConsensusUser.h"
#include "libCrypto/Sha2.h"
#include "libData/AccountData/Account.h"
#include "libData/AccountData/AccountStore.h"
#include "libData/AccountData/Transaction.h"
#include "libMediator/Mediator.h"
#include "libMessage/Messenger.h"
#include "libNetwork/Blacklist.h"
#include "libNetwork/Guard.h"
#include "libPOW/pow.h"
#include "libPersistence/IncrementalDB.h"
#include "libUtils/BitVector.h"
#include "libUtils/DataConversion.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/HashUtils.h"
#include "libUtils/Logger.h"
#include "libUtils/SanityChecks.h"
#include "libUtils/TimeLockedFunction.h"
#include "libUtils/TimeUtils.h"
#include "libUtils/TimestampVerifier.h"
#include "libUtils/UpgradeManager.h"

using namespace std;
using namespace boost::multiprecision;

void Node::StoreDSBlockToDisk(const DSBlock& dsblock) {
  LOG_MARKER();

  m_mediator.m_dsBlockChain.AddBlock(dsblock);
  LOG_GENERAL(INFO, "Block num = " << dsblock.GetHeader().GetBlockNum());
  LOG_GENERAL(
      INFO, "DS diff   = " << to_string(dsblock.GetHeader().GetDSDifficulty()));
  LOG_GENERAL(INFO,
              "Diff      = " << to_string(dsblock.GetHeader().GetDifficulty()));
  LOG_GENERAL(INFO, "Timestamp = " << dsblock.GetTimestamp());

  // Update the rand1 value for next PoW
  m_mediator.UpdateDSBlockRand();

  // Store DS Block to disk
  bytes serializedDSBlock;
  dsblock.Serialize(serializedDSBlock, 0);

  BlockStorage::GetBlockStorage().PutDSBlock(dsblock.GetHeader().GetBlockNum(),
                                             serializedDSBlock);
  if (ENABLE_INCR_DB) {
    IncrementalDB::GetInstance().PutDSBlock(dsblock.GetHeader().GetBlockNum(),
                                            serializedDSBlock,
                                            dsblock.GetHeader().GetBlockNum());
  }
  m_mediator.m_ds->m_latestActiveDSBlockNum = dsblock.GetHeader().GetBlockNum();
  BlockStorage::GetBlockStorage().PutMetadata(
      LATESTACTIVEDSBLOCKNUM, DataConversion::StringToCharArray(to_string(
                                  m_mediator.m_ds->m_latestActiveDSBlockNum)));

  uint64_t latestInd = m_mediator.m_blocklinkchain.GetLatestIndex() + 1;

  m_mediator.m_blocklinkchain.AddBlockLink(
      latestInd, dsblock.GetHeader().GetBlockNum(), BlockType::DS,
      dsblock.GetBlockHash());
}

void Node::UpdateDSCommiteeComposition(DequeOfNode& dsComm,
                                       const DSBlock& dsblock) {
  LOG_MARKER();
  const map<PubKey, Peer> NewDSMembers = dsblock.GetHeader().GetDSPoWWinners();
  DequeOfNode::iterator it;

  for (const auto& DSPowWinner : NewDSMembers) {
    if (m_mediator.m_selfKey.second == DSPowWinner.first) {
      if (!GUARD_MODE) {
        dsComm.emplace_front(m_mediator.m_selfKey.second, Peer());
      } else {
        it = dsComm.begin() + (Guard::GetInstance().GetNumOfDSGuard());
        dsComm.emplace(it, m_mediator.m_selfKey.second, Peer());
      }
    } else {
      if (!GUARD_MODE) {
        dsComm.emplace_front(DSPowWinner);

      } else {
        it = dsComm.begin() + (Guard::GetInstance().GetNumOfDSGuard());
        dsComm.emplace(it, DSPowWinner);
      }
    }
    dsComm.pop_back();
  }
}

bool Node::VerifyDSBlockCoSignature(const DSBlock& dsblock) {
  LOG_MARKER();

  unsigned int index = 0;
  unsigned int count = 0;

  const vector<bool>& B2 = dsblock.GetB2();
  if (m_mediator.m_DSCommittee->size() != B2.size()) {
    LOG_CHECK_FAIL("Cosig size", B2.size(), m_mediator.m_DSCommittee->size());
    return false;
  }

  // Generate the aggregated key
  vector<PubKey> keys;
  for (auto const& kv : *m_mediator.m_DSCommittee) {
    if (B2.at(index)) {
      keys.emplace_back(kv.first);
      count++;
    }
    index++;
  }

  if (count != ConsensusCommon::NumForConsensus(B2.size())) {
    LOG_GENERAL(WARNING, "Cosig was not generated by enough nodes");
    return false;
  }

  shared_ptr<PubKey> aggregatedKey = MultiSig::AggregatePubKeys(keys);
  if (aggregatedKey == nullptr) {
    LOG_GENERAL(WARNING, "Aggregated key generation failed");
    return false;
  }

  // Verify the collective signature
  bytes message;
  if (!dsblock.GetHeader().Serialize(message, 0)) {
    LOG_GENERAL(WARNING, "DSBlockHeader serialization failed");
    return false;
  }
  dsblock.GetCS1().Serialize(message, message.size());
  BitVector::SetBitVector(message, message.size(), dsblock.GetB1());
  if (!MultiSig::GetInstance().MultiSigVerify(
          message, 0, message.size(), dsblock.GetCS2(), *aggregatedKey)) {
    LOG_GENERAL(WARNING, "Cosig verification failed");
    for (auto& kv : keys) {
      LOG_GENERAL(WARNING, kv);
    }
    return false;
  }

  return true;
}

void Node::LogReceivedDSBlockDetails([[gnu::unused]] const DSBlock& dsblock) {
  LOG_GENERAL(INFO,
              "DS Diff   = " << (int)dsblock.GetHeader().GetDSDifficulty());
  LOG_GENERAL(INFO, "Diff      = " << (int)dsblock.GetHeader().GetDifficulty());
  LOG_GENERAL(INFO, "Block num = " << dsblock.GetHeader().GetBlockNum());
  LOG_GENERAL(INFO, "Leader    = " << dsblock.GetHeader().GetLeaderPubKey());

  LOG_GENERAL(INFO, "DS committee");
  unsigned int ds_index = 0;
  for (const auto& dsmember : dsblock.GetHeader().GetDSPoWWinners()) {
    LOG_GENERAL(INFO,
                "[" << PAD(ds_index++, 3, ' ') << "] " << dsmember.second);
  }
}

bool Node::LoadShardingStructure(bool callByRetrieve) {
  if (LOOKUP_NODE_MODE) {
    LOG_GENERAL(WARNING,
                "Node::LoadShardingStructure not expected to be called "
                "from LookUp node.");
    return true;
  }

  m_numShards = m_mediator.m_ds->m_shards.size();

  // Check the shard ID against the deserialized structure
  if (m_myshardId >= m_mediator.m_ds->m_shards.size()) {
    LOG_EPOCH(WARNING, m_mediator.m_currentEpochNum,
              "Shard ID " << m_myshardId << " >= num shards "
                          << m_mediator.m_ds->m_shards.size());
    return false;
  }

  const auto& my_shard = m_mediator.m_ds->m_shards.at(m_myshardId);

  // All nodes; first entry is leader
  unsigned int index = 0;
  bool foundMe = false;

  {
    lock_guard<mutex> g(m_mutexShardMember);
    // m_myShardMembers->clear();
    m_myShardMembers.reset(new DequeOfNode);
    for (const auto& shardNode : my_shard) {
      m_myShardMembers->emplace_back(std::get<SHARD_NODE_PUBKEY>(shardNode),
                                     std::get<SHARD_NODE_PEER>(shardNode));

      // Zero out my IP to avoid sending to myself
      if (m_mediator.m_selfPeer == m_myShardMembers->back().second) {
        m_consensusMyID = index;  // Set my ID
        m_myShardMembers->back().second = Peer();
        foundMe = true;
      }

      LOG_GENERAL(INFO, "[" << PAD(index, 3, ' ') << "] "
                            << m_myShardMembers->back().first << " "
                            << m_myShardMembers->back().second);

      index++;
    }
  }

  if (!foundMe && !callByRetrieve) {
    LOG_GENERAL(WARNING, "I'm not in the sharding structure, why?");
    RejoinAsNormal();
    return false;
  }

  return true;
}

void Node::StartFirstTxEpoch() {
  if (LOOKUP_NODE_MODE) {
    LOG_GENERAL(WARNING,
                "Node::StartFirstTxEpoch not expected to be called from "
                "LookUp node.");
    return;
  }

  LOG_MARKER();
  m_requestedForDSGuardNetworkInfoUpdate = false;
  ResetConsensusId();
  Blacklist::GetInstance().Clear();

  uint16_t lastBlockHash = 0;
  if (m_mediator.m_currentEpochNum > 1) {
    lastBlockHash = DataConversion::charArrTo16Bits(
        m_mediator.m_txBlockChain.GetLastBlock().GetBlockHash().asBytes());
  }

  lock_guard<mutex> g(m_mutexShardMember);

  if (m_mediator.m_ds->m_mode != DirectoryService::IDLE && GUARD_MODE) {
    m_consensusLeaderID =
        lastBlockHash % Guard::GetInstance().GetNumOfDSGuard();
  } else {
    m_consensusLeaderID = lastBlockHash % m_myShardMembers->size();
  }

  // Check if I am the leader or backup of the shard
  if (m_mediator.m_selfKey.second ==
      (*m_myShardMembers)[m_consensusLeaderID].first) {
    m_isPrimary = true;
    LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
              "I am leader of the sharded committee");

    LOG_STATE("[IDENT][" << std::setw(15) << std::left
                         << m_mediator.m_selfPeer.GetPrintableIPAddress()
                         << "][" << m_mediator.m_currentEpochNum << "]["
                         << m_myshardId << "][  0] SCLD");
  } else {
    m_isPrimary = false;

    LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
              "I am backup member of the sharded committee");

    LOG_STATE(
        "[SHSTU]["
        << setw(15) << left << m_mediator.m_selfPeer.GetPrintableIPAddress()
        << "]["
        << m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum() +
               1
        << "] RECVD SHARDING STRUCTURE");

    LOG_STATE("[IDENT][" << std::setw(15) << std::left
                         << m_mediator.m_selfPeer.GetPrintableIPAddress()
                         << "][" << m_mediator.m_currentEpochNum << "]["
                         << m_myshardId << "][" << std::setw(3) << std::left
                         << m_consensusMyID << "] SCBK");
  }

  m_justDidFallback = false;

  if (BROADCAST_GOSSIP_MODE) {
    VectorOfNode peers;
    std::vector<PubKey> pubKeys;
    GetEntireNetworkPeerInfo(peers, pubKeys);

    // Initialize every start of DS Epoch
    P2PComm::GetInstance().InitializeRumorManager(peers, pubKeys);
  }

  CommitTxnPacketBuffer();

  auto main_func3 = [this]() mutable -> void { RunConsensusOnMicroBlock(); };

  DetachedFunction(1, main_func3);

  FallbackTimerLaunch();
  FallbackTimerPulse();
}

void Node::ResetConsensusId() {
  m_mediator.m_consensusID = m_mediator.m_currentEpochNum == 1 ? 1 : 0;
}

bool Node::ProcessVCDSBlocksMessage(const bytes& message,
                                    unsigned int cur_offset,
                                    [[gnu::unused]] const Peer& from) {
  LOG_MARKER();
  lock_guard<mutex> g(m_mutexDSBlock);

  if (!LOOKUP_NODE_MODE) {
    if (!CheckState(PROCESS_DSBLOCK)) {
      return false;
    }
  } else {
    LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
              "I the lookup node have received the DS Block");
  }

  DSBlock dsblock;
  vector<VCBlock> vcBlocks;
  uint32_t shardId;
  Peer newleaderIP;

  DequeOfShard t_shards;
  uint32_t shardingStructureVersion = 0;

  if (!Messenger::GetNodeVCDSBlocksMessage(
          message, cur_offset, shardId, dsblock, vcBlocks,
          shardingStructureVersion, t_shards)) {
    LOG_EPOCH(WARNING, m_mediator.m_currentEpochNum,
              "Messenger::GetNodeVCDSBlocksMessage failed.");
    return false;
  }

  if (shardingStructureVersion != SHARDINGSTRUCTURE_VERSION) {
    LOG_CHECK_FAIL("Sharding structure version", shardingStructureVersion,
                   SHARDINGSTRUCTURE_VERSION);
    return false;
  }

  // Verify the DSBlockHashSet member of the DSBlockHeader
  ShardingHash shardingHash;
  if (!Messenger::GetShardingStructureHash(SHARDINGSTRUCTURE_VERSION, t_shards,
                                           shardingHash)) {
    LOG_EPOCH(WARNING, m_mediator.m_currentEpochNum,
              "Messenger::GetShardingStructureHash failed.");
    return false;
  }

  if (dsblock.GetHeader().GetVersion() != DSBLOCK_VERSION) {
    LOG_CHECK_FAIL("DSBlock version", dsblock.GetHeader().GetVersion(),
                   DSBLOCK_VERSION);
    return false;
  }

  // Check timestamp (must be greater than timestamp of last Tx block header in
  // the Tx blockchain)
  if (m_mediator.m_txBlockChain.GetBlockCount() > 0) {
    const TxBlock& lastTxBlock = m_mediator.m_txBlockChain.GetLastBlock();
    uint64_t thisDSTimestamp = dsblock.GetTimestamp();
    uint64_t lastTxBlockTimestamp = lastTxBlock.GetTimestamp();
    if (thisDSTimestamp <= lastTxBlockTimestamp) {
      LOG_GENERAL(WARNING, "Timestamp check failed. Last Tx Block: "
                               << lastTxBlockTimestamp
                               << " DSBlock: " << thisDSTimestamp);
      return false;
    }
  }

  // Check timestamp
  if (!VerifyTimestamp(
          dsblock.GetTimestamp(),
          CONSENSUS_OBJECT_TIMEOUT + (TX_DISTRIBUTE_TIME_IN_MS) / 1000)) {
    return false;
  }

  if (shardingHash != dsblock.GetHeader().GetShardingHash()) {
    LOG_GENERAL(WARNING,
                "Sharding structure hash in newly received DS Block doesn't "
                "match. Calculated: "
                    << shardingHash
                    << " Received: " << dsblock.GetHeader().GetShardingHash());
    return false;
  }

  BlockHash temp_blockHash = dsblock.GetHeader().GetMyHash();
  if (temp_blockHash != dsblock.GetBlockHash()) {
    LOG_GENERAL(WARNING,
                "Block Hash in Newly received DS Block doesn't match. "
                "Calculated: "
                    << temp_blockHash
                    << " Received: " << dsblock.GetBlockHash().hex());
    return false;
  }

  // Checking for freshness of incoming DS Block
  if (!m_mediator.CheckWhetherBlockIsLatest(
          dsblock.GetHeader().GetBlockNum(),
          dsblock.GetHeader().GetEpochNum())) {
    LOG_GENERAL(WARNING,
                "ProcessVCDSBlocksMessage CheckWhetherBlockIsLatest failed");
    return false;
  }

  uint32_t expectedViewChangeCounter = 1;
  for (const auto& vcBlock : vcBlocks) {
    if (!ProcessVCBlockCore(vcBlock)) {
      LOG_GENERAL(WARNING, "Checking for error when processing vc blocknum "
                               << vcBlock.GetHeader().GetViewChangeCounter());
      return false;
    }

    LOG_GENERAL(INFO, "view change completed for vc blocknum "
                          << vcBlock.GetHeader().GetViewChangeCounter());
    expectedViewChangeCounter++;
  }

  // Verify the CommitteeHash member of the BlockHeaderBase
  CommitteeHash committeeHash;
  if (!Messenger::GetDSCommitteeHash(*m_mediator.m_DSCommittee,
                                     committeeHash)) {
    LOG_EPOCH(WARNING, m_mediator.m_currentEpochNum,
              "Messenger::GetDSCommitteeHash failed.");
    return false;
  }
  if (committeeHash != dsblock.GetHeader().GetCommitteeHash()) {
    LOG_GENERAL(WARNING,
                "DS committee hash in newly received DS Block doesn't match. "
                "Calculated: "
                    << committeeHash
                    << " Received: " << dsblock.GetHeader().GetCommitteeHash());
    return false;
  }

  // Check the signature of this DS block
  if (!VerifyDSBlockCoSignature(dsblock)) {
    LOG_EPOCH(WARNING, m_mediator.m_currentEpochNum,
              "DSBlock co-sig verification failed");
    return false;
  }

  // For running from genesis
  if (m_mediator.m_lookup->GetSyncType() != SyncType::NO_SYNC) {
    if (!m_mediator.m_lookup->m_startedPoW) {
      LOG_GENERAL(WARNING, "Haven't started PoW, why I received a DSBlock?");
      return false;
    }

    m_mediator.m_lookup->SetSyncType(SyncType::NO_SYNC);
    m_mediator.m_lookup->cv_waitJoined.notify_all();
    if (m_fromNewProcess) {
      m_fromNewProcess = false;
    }
  }

  m_mediator.m_ds->m_shards = move(t_shards);

  m_myshardId = shardId;
  BlockStorage::GetBlockStorage().PutShardStructure(m_mediator.m_ds->m_shards,
                                                    m_myshardId);

  LogReceivedDSBlockDetails(dsblock);

  auto func = [this, dsblock]() mutable -> void {
    lock_guard<mutex> g(m_mediator.m_mutexCurSWInfo);
    if (m_mediator.m_curSWInfo != dsblock.GetHeader().GetSWInfo()) {
      if (UpgradeManager::GetInstance().DownloadSW()) {
        m_mediator.m_curSWInfo =
            *UpgradeManager::GetInstance().GetLatestSWInfo();
      }
    }
  };
  DetachedFunction(1, func);

  // Add to block chain and Store the DS block to disk.
  StoreDSBlockToDisk(dsblock);

  BlockStorage::GetBlockStorage().ResetDB(BlockStorage::STATE_DELTA);

  m_proposedGasPrice =
      max(m_proposedGasPrice, dsblock.GetHeader().GetGasPrice());
  cv_waitDSBlock.notify_one();

  LOG_STATE(
      "[DSBLK]["
      << setw(15) << left << m_mediator.m_selfPeer.GetPrintableIPAddress()
      << "]["
      << m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum() + 1
      << "] RECVD DSBLOCK -> DS Diff = "
      << to_string(dsblock.GetHeader().GetDSDifficulty())
      << " Diff = " << to_string(dsblock.GetHeader().GetDifficulty()));

  if (LOOKUP_NODE_MODE) {
    LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
              "I the lookup node have stored the DS Block");
  }

  m_mediator.UpdateDSBlockRand();  // Update the rand1 value for next PoW
  UpdateDSCommiteeComposition(*m_mediator.m_DSCommittee,
                              m_mediator.m_dsBlockChain.GetLastBlock());

  if (!LOOKUP_NODE_MODE) {
    uint32_t ds_size = m_mediator.m_DSCommittee->size();
    POW::GetInstance().StopMining();
    m_stillMiningPrimary = false;

    // Assign from size -1 as it will get pop and push into ds committee data
    // structure, Hence, the ordering is reverse.
    const map<PubKey, Peer> dsPoWWinners =
        m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetDSPoWWinners();
    unsigned int newDSMemberIndex = dsPoWWinners.size() - 1;

    // Under guard mode, first n member of ds comm belongs to DS guard.
    // As such, new ds committee member should join ds comm at index
    // newDSMemberIndex + num of ds guard
    if (GUARD_MODE) {
      newDSMemberIndex += Guard::GetInstance().GetNumOfDSGuard();
    }

    bool isNewDSMember = false;

    for (const auto& newDSMember : dsPoWWinners) {
      if (m_mediator.m_selfKey.second == newDSMember.first) {
        isNewDSMember = true;
        m_mediator.m_ds->SetConsensusMyID(newDSMemberIndex);
        LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
                  "I won DS PoW. Currently, one of the new ds "
                  "committee member with id "
                      << m_mediator.m_ds->GetConsensusMyID());
      }
      newDSMemberIndex--;
    }

    uint16_t lastBlockHash = 0;
    if (m_mediator.m_currentEpochNum > 1) {
      lastBlockHash = DataConversion::charArrTo16Bits(
          m_mediator.m_dsBlockChain.GetLastBlock()
              .GetHeader()
              .GetHashForRandom()
              .asBytes());
    }

    if (!GUARD_MODE) {
      m_mediator.m_ds->SetConsensusLeaderID(lastBlockHash % ds_size);
    } else {
      m_mediator.m_ds->SetConsensusLeaderID(
          lastBlockHash % Guard::GetInstance().GetNumOfDSGuard());
    }

    LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
              "lastBlockHash " << lastBlockHash << ", new DS leader Id "
                               << m_mediator.m_ds->GetConsensusLeaderID());

    // If I am the next DS leader -> need to set myself up as a DS node
    if (isNewDSMember) {
      // Process sharding structure as a DS node
      if (!m_mediator.m_ds->ProcessShardingStructure(
              m_mediator.m_ds->m_shards,
              m_mediator.m_ds->m_publicKeyToshardIdMap,
              m_mediator.m_ds->m_mapNodeReputation)) {
        return false;
      }

      {
        lock_guard<mutex> g(m_mediator.m_mutexDSCommittee);
        LOG_GENERAL(INFO, "New DS leader is at "
                              << m_mediator.m_ds->GetConsensusLeaderID());
        if (m_mediator.m_ds->GetConsensusLeaderID() ==
            m_mediator.m_ds->GetConsensusMyID()) {
          // I am the new DS committee leader
          m_mediator.m_ds->m_mode = DirectoryService::Mode::PRIMARY_DS;
          LOG_EPOCHINFO(m_mediator.m_currentEpochNum, DS_LEADER_MSG);
          LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
                    "I am now DS leader for the next round");
          LOG_STATE("[IDENT][" << std::setw(15) << std::left
                               << m_mediator.m_selfPeer.GetPrintableIPAddress()
                               << "][" << m_mediator.m_currentEpochNum
                               << "] DSLD");
        } else {
          m_mediator.m_ds->m_mode = DirectoryService::Mode::BACKUP_DS;
          LOG_EPOCHINFO(m_mediator.m_currentEpochNum, DS_BACKUP_MSG);
          LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
                    "I am now DS backup for the next round");
        }
      }

      m_mediator.m_ds->StartFirstTxEpoch();
    } else {
      // If I am a shard node
      LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
                "I lost PoW (DS level) :-( Better luck next time!");

      // Process sharding structure as a shard node
      if (!LoadShardingStructure()) {
        return false;
      }

      if (BROADCAST_TREEBASED_CLUSTER_MODE) {
        SendDSBlockToOtherShardNodes(message);
      }

      // Finally, start as a shard node
      StartFirstTxEpoch();
    }
  } else {
    // Process sharding structure as a lookup node
    m_mediator.m_lookup->ProcessEntireShardingStructure();

    ResetConsensusId();
    Blacklist::GetInstance().Clear();

    if (m_mediator.m_lookup->GetIsServer() && !ARCHIVAL_LOOKUP) {
      m_mediator.m_lookup->SenderTxnBatchThread();
    }

    FallbackTimerLaunch();
    FallbackTimerPulse();
  }

  BlockStorage::GetBlockStorage().PutDSCommittee(
      m_mediator.m_DSCommittee, m_mediator.m_ds->GetConsensusLeaderID());

  if (LOOKUP_NODE_MODE) {
    bool canPutNewEntry = true;

    // There's no quick way to get the oldest entry in leveldb
    // Hence, we manage deleting old entries here instead
    if ((MAX_ENTRIES_FOR_DIAGNOSTIC_DATA >
         0) &&  // If limit is 0, skip deletion
        (BlockStorage::GetBlockStorage().GetDiagnosticDataNodesCount() >=
         MAX_ENTRIES_FOR_DIAGNOSTIC_DATA) &&  // Limit reached
        (m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetBlockNum() >=
         MAX_ENTRIES_FOR_DIAGNOSTIC_DATA)) {  // DS Block number is not below
                                              // limit

      const uint64_t oldBlockNum =
          m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetBlockNum() -
          MAX_ENTRIES_FOR_DIAGNOSTIC_DATA;

      canPutNewEntry =
          BlockStorage::GetBlockStorage().DeleteDiagnosticDataNodes(
              oldBlockNum);

      if (canPutNewEntry) {
        LOG_GENERAL(INFO,
                    "Deleted old diagnostic data for DS block " << oldBlockNum);
      } else {
        LOG_GENERAL(WARNING,
                    "Failed to delete old diagnostic data for DS block "
                        << oldBlockNum);
      }
    }

    if (canPutNewEntry) {
      BlockStorage::GetBlockStorage().PutDiagnosticDataNodes(
          m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetBlockNum(),
          m_mediator.m_ds->m_shards, *m_mediator.m_DSCommittee);
    }
  }

  return true;
}

void Node::SendDSBlockToOtherShardNodes(const bytes& dsblock_message) {
  LOG_MARKER();
  unsigned int cluster_size = NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD;
  if (cluster_size <= NUM_DS_ELECTION) {
    LOG_GENERAL(
        WARNING,
        "Adjusting NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD to be greater than "
        "NUM_DS_ELECTION. Why not correct the constant.xml next time.");
    cluster_size = NUM_DS_ELECTION + 1;
  }
  LOG_GENERAL(INFO,
              "Primary CLUSTER SIZE used is "
              "(NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD):"
                  << cluster_size);
  SendBlockToOtherShardNodes(dsblock_message, cluster_size,
                             NUM_OF_TREEBASED_CHILD_CLUSTERS);
}
