/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KvStore.h"

#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/Random.h>
#include <folly/String.h>

#include <openr/common/Constants.h>
#include <openr/common/Util.h>

using namespace std::chrono_literals;
using namespace std::chrono;

namespace openr {

KvStoreFilters::KvStoreFilters(
    std::vector<std::string> const& keyPrefix,
    std::set<std::string> const& nodeIds)
    : keyPrefixList_(keyPrefix),
      originatorIds_(nodeIds),
      keyPrefixObjList_(KeyPrefix(keyPrefixList_)) {}

bool
KvStoreFilters::keyMatch(
    std::string const& key, thrift::Value const& value) const {
  if (keyPrefixList_.empty() && originatorIds_.empty()) {
    return true;
  }
  if (!keyPrefixList_.empty() && keyPrefixObjList_.keyMatch(key)) {
    return true;
  }
  if (!originatorIds_.empty() && originatorIds_.count(value.originatorId)) {
    return true;
  }
  return false;
}

std::vector<std::string>
KvStoreFilters::getKeyPrefixes() const {
  return keyPrefixList_;
}

std::set<std::string>
KvStoreFilters::getOrigniatorIdList() const {
  return originatorIds_;
}

std::string
KvStoreFilters::str() const {
  std::string result{};
  result += "\nPrefix filters:\n";
  for (const auto& prefixString : keyPrefixList_) {
    result += folly::sformat("{}, ", prefixString);
  }
  result += "\nOriginator ID filters:\n";
  for (const auto& originatorId : originatorIds_) {
    result += folly::sformat("{}, ", originatorId);
  }
  return result;
}

KvStore::KvStore(
    // initializers for immutable state
    fbzmq::Context& zmqContext,
    std::string nodeId,
    KvStoreLocalPubUrl localPubUrl,
    KvStoreGlobalPubUrl globalPubUrl,
    KvStoreGlobalCmdUrl globalCmdUrl,
    MonitorSubmitUrl monitorSubmitUrl,
    folly::Optional<int> maybeIpTos,
    std::chrono::seconds dbSyncInterval,
    std::chrono::seconds monitorSubmitInterval,
    // initializer for mutable state
    std::unordered_map<std::string, thrift::PeerSpec> peers,
    folly::Optional<KvStoreFilters> filters,
    int zmqHwm,
    KvStoreFloodRate floodRate,
    std::chrono::milliseconds ttlDecr,
    bool enableFloodOptimization,
    bool isFloodRoot,
    bool useFloodOptimization)
    : OpenrEventLoop(
          nodeId,
          thrift::OpenrModuleType::KVSTORE,
          zmqContext,
          std::string{globalCmdUrl},
          maybeIpTos,
          zmqHwm),
      DualNode(nodeId, isFloodRoot),
      zmqContext_(zmqContext),
      nodeId_(std::move(nodeId)),
      localPubUrl_(std::move(localPubUrl)),
      globalPubUrl_(std::move(globalPubUrl)),
      dbSyncInterval_(dbSyncInterval),
      monitorSubmitInterval_(monitorSubmitInterval),
      hwm_(zmqHwm),
      ttlDecr_(ttlDecr),
      enableFloodOptimization_(enableFloodOptimization),
      isFloodRoot_(isFloodRoot),
      useFloodOptimization_(useFloodOptimization),
      filters_(std::move(filters)),
      // initialize zmq sockets
      localPubSock_{zmqContext},
      peerSyncSock_(
          zmqContext,
          fbzmq::IdentityString{folly::sformat(
              Constants::kPeerSyncIdTemplate.toString(), nodeId_)},
          folly::none,
          fbzmq::NonblockingFlag{true}),
      floodRate_(floodRate) {
  CHECK(not nodeId_.empty());
  CHECK(not localPubUrl_.empty());
  CHECK(not globalPubUrl_.empty());

  // allocate new global pub socket if not provided
  globalPubSock_ = fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER>(
      zmqContext,
      fbzmq::IdentityString{
          folly::sformat(Constants::kGlobalPubIdTemplate.toString(), nodeId_)},
      folly::none,
      fbzmq::NonblockingFlag{true});

  if (floodRate_.hasValue()) {
    floodLimiter_ = std::make_unique<folly::BasicTokenBucket<>>(
        floodRate_.value().first, // messages per sec
        floodRate_.value().second); // burst size
    pendingPublicationTimer_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
      if (!floodLimiter_->consume(1)) {
        pendingPublicationTimer_->scheduleTimeout(
            Constants::kFloodPendingPublication, false);
        return;
      }
      floodBufferedUpdates();
    });
  }

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  // Schedule periodic timer for counters submission
  const bool isPeriodic = true;
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(monitorSubmitInterval_, isPeriodic);

  //
  // Set various socket options
  //

  // HWM for pub and peer sub sockets
  const auto localPubHwm =
      localPubSock_.setSockOpt(ZMQ_SNDHWM, &hwm_, sizeof(hwm_));
  if (localPubHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm_ << " "
               << localPubHwm.error();
  }
  const auto globalPubHwm =
      globalPubSock_.setSockOpt(ZMQ_SNDHWM, &hwm_, sizeof(hwm_));
  if (globalPubHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm_ << " "
               << globalPubHwm.error();
  }

  const auto peersSyncSndHwm =
      peerSyncSock_.setSockOpt(ZMQ_SNDHWM, &hwm_, sizeof(hwm_));
  if (peersSyncSndHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm_ << " "
               << peersSyncSndHwm.error();
  }
  const auto peerSyncRcvHwm =
      peerSyncSock_.setSockOpt(ZMQ_RCVHWM, &hwm_, sizeof(hwm_));
  if (peerSyncRcvHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm_ << " "
               << peerSyncRcvHwm.error();
  }

  // enable handover for inter process router socket
  const int handover = 1;
  const auto peerSyncHandover =
      peerSyncSock_.setSockOpt(ZMQ_ROUTER_HANDOVER, &handover, sizeof(int));
  if (peerSyncHandover.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_ROUTER_HANDOVER to " << handover << " "
               << peerSyncHandover.error();
  }

  // set keep-alive to retire old flows
  const auto peerSyncKeepAlive = peerSyncSock_.setKeepAlive(
      Constants::kKeepAliveEnable,
      Constants::kKeepAliveTime.count(),
      Constants::kKeepAliveCnt,
      Constants::kKeepAliveIntvl.count());
  if (peerSyncKeepAlive.hasError()) {
    LOG(FATAL) << "Error setting KeepAlive " << peerSyncKeepAlive.error();
  }

  if (maybeIpTos) {
    const int ipTos = *maybeIpTos;
    const auto globalPubTos =
        globalPubSock_.setSockOpt(ZMQ_TOS, &ipTos, sizeof(int));
    if (globalPubTos.hasError()) {
      LOG(FATAL) << "Error setting ZMQ_TOS to " << ipTos << " "
                 << globalPubTos.error();
    }
    const auto peerSyncTos =
        peerSyncSock_.setSockOpt(ZMQ_TOS, &ipTos, sizeof(int));
    if (peerSyncTos.hasError()) {
      LOG(FATAL) << "Error setting ZMQ_TOS to " << ipTos << " "
                 << peerSyncTos.error();
    }
  }

  //
  // Bind the sockets
  //
  VLOG(2) << "KvStore: Binding publisher and replier sockets.";

  // the following will throw exception if something is wrong
  VLOG(2) << "KvStore: Binding localPubUrl '" << localPubUrl_ << "'";
  const auto localPubBind = localPubSock_.bind(fbzmq::SocketUrl{localPubUrl_});
  if (localPubBind.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << localPubUrl_ << "' "
               << localPubBind.error();
  }

  VLOG(2) << "KvStore: Binding globalPubUrl '" << globalPubUrl_ << "'";
  const auto globalPubBind =
      globalPubSock_.bind(fbzmq::SocketUrl{globalPubUrl_});
  if (globalPubBind.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << globalPubUrl_ << "' "
               << globalPubBind.error();
  }

  // Attach socket callbacks/schedule events
  attachCallbacks();

  VLOG(2) << "Subscribing/connecting to all peers...";

  // Add all existing peers again. This will also ensure querying full dump
  // from each peer.
  addPeers(peers);

  // Hook up timer with cleanupTtlCountdownQueue(). The actual scheduling
  // happens within updateTtlCountdownQueue()
  ttlCountdownTimer_ = fbzmq::ZmqTimeout::make(
      this, [this]() noexcept { cleanupTtlCountdownQueue(); });
}

// static, public
std::unordered_map<std::string, thrift::Value>
KvStore::mergeKeyValues(
    std::unordered_map<std::string, thrift::Value>& kvStore,
    std::unordered_map<std::string, thrift::Value> const& keyVals,
    folly::Optional<KvStoreFilters> const& filters) {
  // the publication to build if we update our KV store
  std::unordered_map<std::string, thrift::Value> kvUpdates;

  // Counters for logging
  uint32_t ttlUpdateCnt{0}, valUpdateCnt{0};

  for (const auto& kv : keyVals) {
    auto const& key = kv.first;
    auto const& value = kv.second;

    if (filters.hasValue() && not filters->keyMatch(kv.first, kv.second)) {
      VLOG(4) << "key: " << key << " not adding from " << value.originatorId;
      continue;
    }

    // versions must start at 1; setting this to zero here means
    // we would be beaten by any version supplied by the setter
    int64_t myVersion{0};
    int64_t newVersion = value.version;

    // Check if TTL is valid. It must be infinite or positive number
    // Skip if invalid!
    if (value.ttl != Constants::kTtlInfinity && value.ttl <= 0) {
      continue;
    }

    // if key exist, compare values first
    // if they are the same, no need to propagate changes
    auto kvStoreIt = kvStore.find(key);
    if (kvStoreIt != kvStore.end()) {
      myVersion = kvStoreIt->second.version;
    } else {
      VLOG(4) << "(mergeKeyValues) key: '" << key << "' not found, adding";
    }

    // If we get an old value just skip it
    if (newVersion < myVersion) {
      continue;
    }

    bool updateAllNeeded{false};
    bool updateTtlNeeded{false};

    //
    // Check updateAll and updateTtl
    //
    if (value.value.hasValue()) {
      if (newVersion > myVersion) {
        // Version is newer or
        // kvStoreIt is NULL(myVersion is set to 0)
        updateAllNeeded = true;
      } else if (value.originatorId > kvStoreIt->second.originatorId) {
        // versions are the same but originatorId is higher
        updateAllNeeded = true;
      } else if (value.originatorId == kvStoreIt->second.originatorId) {
        // This can occur after kvstore restarts or simply reconnects after
        // disconnection. We let one of the two values win if they differ(higher
        // in this case but can be lower as long as it's deterministic).
        // Otherwise, local store can have new value while other stores have old
        // value and they never sync.
        int rc = (*value.value).compare(*kvStoreIt->second.value);
        if (rc > 0) {
          // versions and orginatorIds are same but value is higher
          VLOG(3) << "Previous incarnation reflected back for key " << key;
          updateAllNeeded = true;
        } else if (rc == 0) {
          // versions, orginatorIds, value are all same
          // retain higher ttlVersion
          if (value.ttlVersion > kvStoreIt->second.ttlVersion) {
            updateTtlNeeded = true;
          }
        }
      }
    }

    //
    // Check updateTtl
    //
    if (not value.value.hasValue() and kvStoreIt != kvStore.end() and
        value.version == kvStoreIt->second.version and
        value.originatorId == kvStoreIt->second.originatorId and
        value.ttlVersion > kvStoreIt->second.ttlVersion) {
      updateTtlNeeded = true;
    }

    if (!updateAllNeeded and !updateTtlNeeded) {
      VLOG(4) << "(mergeKeyValues) no need to update anything for key: '" << key
              << "'";
      continue;
    }

    VLOG(2) << "Updating key: " << key << "\n  Value: "
            << (kvStoreIt != kvStore.end() && kvStoreIt->second.value.hasValue()
                    ? kvStoreIt->second.value.value()
                    : "null")
            << " -> " << (value.value.hasValue() ? value.value.value() : "null")
            << "\n  Version: " << myVersion << " -> " << newVersion
            << "\n  Originator: "
            << (kvStoreIt != kvStore.end() ? kvStoreIt->second.originatorId
                                           : "null")
            << " -> " << value.originatorId << "\n  TtlVersion: "
            << (kvStoreIt != kvStore.end() ? kvStoreIt->second.ttlVersion : 0)
            << " -> " << value.ttlVersion << "\n  Ttl: "
            << (kvStoreIt != kvStore.end() ? kvStoreIt->second.ttl : 0)
            << " -> " << value.ttl;

    // grab the new value (this will copy, intended)
    thrift::Value newValue = value;

    VLOG(4) << "(mergeKeyValues) Inserting/Updating key: '" << key << "'";

    if (updateAllNeeded) {
      ++valUpdateCnt;
      //
      // update everything for such key
      //
      CHECK(value.value.hasValue());
      if (kvStoreIt == kvStore.end()) {
        // create new entry
        std::tie(kvStoreIt, std::ignore) = kvStore.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(std::move(newValue)));
      } else {
        // update the entry in place, the old value will be destructed
        kvStoreIt->second = std::move(newValue);
      }
      // update hash if it's not there
      if (not kvStoreIt->second.hash.hasValue()) {
        kvStoreIt->second.hash =
            generateHash(value.version, value.originatorId, value.value);
      }
    } else if (updateTtlNeeded) {
      ++ttlUpdateCnt;
      //
      // update ttl,ttlVersion only
      //
      CHECK(kvStoreIt != kvStore.end());

      // update TTL only, nothing else
      kvStoreIt->second.ttl = value.ttl;
      kvStoreIt->second.ttlVersion = value.ttlVersion;
    }

    // announce the update
    kvUpdates.emplace(key, value);
  }

  VLOG(4) << "(mergeKeyValues) updating " << kvUpdates.size()
          << " keyvals. ValueUpdates: " << valUpdateCnt
          << ", TtlUpdates: " << ttlUpdateCnt;
  return kvUpdates;
}

void
KvStore::updateTtlCountdownQueue(const thrift::Publication& publication) {
  for (const auto& kv : publication.keyVals) {
    const auto& key = kv.first;
    const auto& value = kv.second;

    if (value.ttl != Constants::kTtlInfinity) {
      TtlCountdownQueueEntry queueEntry;
      queueEntry.expiryTime = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(value.ttl);
      queueEntry.key = key;
      queueEntry.version = value.version;
      queueEntry.ttlVersion = value.ttlVersion;
      queueEntry.originatorId = value.originatorId;

      if ((ttlCountdownQueue_.empty() or
           (queueEntry.expiryTime <= ttlCountdownQueue_.top().expiryTime)) and
          ttlCountdownTimer_) {
        // Reschedule the shorter timeout
        ttlCountdownTimer_->scheduleTimeout(
            std::chrono::milliseconds(value.ttl));
      }

      ttlCountdownQueue_.push(std::move(queueEntry));
    }
  }
}

// build publication out of the requested keys (per request)
// if not keys provided, will return publication with empty keyVals
thrift::Publication
KvStore::getKeyVals(std::vector<std::string> const& keys) {
  thrift::Publication thriftPub;

  for (auto const& key : keys) {
    // if requested key if found, respond with version and value
    auto it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      // copy here
      thriftPub.keyVals[key] = it->second;
    }
  }
  return thriftPub;
}

// dump the entries of my KV store whose keys match the given prefix
// if prefix is the empty string, the full KV store is dumped
thrift::Publication
KvStore::dumpAllWithFilters(KvStoreFilters const& kvFilters) const {
  thrift::Publication thriftPub;

  for (auto const& kv : kvStore_) {
    if (not kvFilters.keyMatch(kv.first, kv.second)) {
      continue;
    }
    thriftPub.keyVals[kv.first] = kv.second;
  }
  return thriftPub;
}

// dump the hashes of my KV store whose keys match the given prefix
// if prefix is the empty string, the full hash store is dumped
thrift::Publication
KvStore::dumpHashWithFilters(KvStoreFilters const& kvFilters) const {
  thrift::Publication thriftPub;
  for (auto const& kv : kvStore_) {
    if (not kvFilters.keyMatch(kv.first, kv.second)) {
      continue;
    }
    DCHECK(kv.second.hash.hasValue());
    auto& value = thriftPub.keyVals[kv.first];
    value.version = kv.second.version;
    value.originatorId = kv.second.originatorId;
    value.hash = kv.second.hash;
    value.ttl = kv.second.ttl;
    value.ttlVersion = kv.second.ttlVersion;
  }
  return thriftPub;
}

/**
 * Compare two values to find out which value is better
 * TODO: this function can be leveraged in mergeKeyValues to perform same
 * logic of which value if better to use
 */
int
KvStore::compareValues(const thrift::Value& v1, const thrift::Value& v2) {
  // compare version
  if (v1.version != v2.version) {
    return v1.version > v2.version ? 1 : -1;
  }

  // compare orginatorId
  if (v1.originatorId != v2.originatorId) {
    return v1.originatorId > v2.originatorId ? 1 : -1;
  }

  // compare value
  if (v1.hash.hasValue() and v2.hash.hasValue() and *v1.hash == *v2.hash) {
    // hashes are same => (version, orginatorId, value are same)
    // compare ttl-version
    if (v1.ttlVersion != v2.ttlVersion) {
      return v1.ttlVersion > v2.ttlVersion ? 1 : -1;
    } else {
      return 0;
    }
  }

  // can't use hash, either it's missing or they are different
  // compare values
  if (v1.value.hasValue() and v2.value.hasValue()) {
    return (*v1.value).compare(*v2.value);
  } else {
    // some value is missing
    return -2; // unknown
  }
}

// dump the keys on which hashes differ from given keyVals
// thriftPub.keyVals: better keys or keys exist only in MY-KEY-VAL
// thriftPub.tobeUpdatedKeys: better keys or keys exist only in REQ-KEY-VAL
// this way, full-sync initiator knows what keys need to send back to finish
// 3-way full-sync
thrift::Publication
KvStore::dumpDifference(
    std::unordered_map<std::string, thrift::Value> const& myKeyVal,
    std::unordered_map<std::string, thrift::Value> const& reqKeyVal) const {
  thrift::Publication thriftPub;

  thriftPub.tobeUpdatedKeys = std::vector<std::string>{};
  std::unordered_set<std::string> allKeys;
  for (const auto& kv : myKeyVal) {
    allKeys.insert(kv.first);
  }
  for (const auto& kv : reqKeyVal) {
    allKeys.insert(kv.first);
  }

  for (const auto& key : allKeys) {
    const auto& myKv = myKeyVal.find(key);
    const auto& reqKv = reqKeyVal.find(key);
    if (myKv == myKeyVal.end()) {
      // not exist in myKeyVal
      thriftPub.tobeUpdatedKeys->emplace_back(key);
      continue;
    }
    if (reqKv == reqKeyVal.end()) {
      // not exist in reqKeyVal
      thriftPub.keyVals.emplace(key, myKv->second);
      continue;
    }
    // common key
    const auto& myVal = myKv->second;
    const auto& reqVal = reqKv->second;
    int rc = compareValues(myVal, reqVal);
    if (rc == 1 or rc == -2) {
      // myVal is better or unknown
      thriftPub.keyVals.emplace(key, myVal);
    }
    if (rc == -1 or rc == -2) {
      // reqVal is better or unknown
      thriftPub.tobeUpdatedKeys->emplace_back(key);
    }
  }

  return thriftPub;
}

// add new peers to subscribe to
void
KvStore::addPeers(
    std::unordered_map<std::string, thrift::PeerSpec> const& peers) {
  ++peerAddCounter_;
  std::vector<std::string> dualPeersToAdd;
  for (auto const& kv : peers) {
    auto const& peerName = kv.first;
    auto const& newPeerSpec = kv.second;
    auto const& newPeerCmdId = folly::sformat(
        Constants::kGlobalCmdLocalIdTemplate.toString(),
        peerName,
        peerAddCounter_);
    const auto& supportFloodOptimization = newPeerSpec.supportFloodOptimization;

    try {
      auto it = peers_.find(peerName);
      bool cmdUrlUpdated{false};
      bool isNewPeer{false};

      // add dual peers for both new-peer or update-peer event
      if (supportFloodOptimization) {
        dualPeersToAdd.emplace_back(peerName);
      }

      if (it != peers_.end()) {
        LOG(INFO)
            << "Updating existing peer " << peerName
            << ", support-flood-optimization: " << supportFloodOptimization;

        const auto& peerSpec = it->second.first;

        if (peerSpec.cmdUrl != newPeerSpec.cmdUrl) {
          // case1: peer-spec updated (e.g parallel cases)
          cmdUrlUpdated = true;
          LOG(INFO) << "Disconnecting from " << peerSpec.cmdUrl << " with id "
                    << it->second.second;
          const auto ret =
              peerSyncSock_.disconnect(fbzmq::SocketUrl{peerSpec.cmdUrl});
          if (ret.hasError()) {
            LOG(FATAL) << "Error Disconnecting to URL '" << peerSpec.cmdUrl
                       << "' " << ret.error();
          }
          it->second.second = newPeerCmdId;
        } else {
          // case2. new peer came up (previsously shut down ungracefully)
          LOG(WARNING) << "new peer " << peerName << ", previously "
                       << "shutdown non-gracefully";
          isNewPeer = true;
        }
        // Update entry with new data
        it->second.first = newPeerSpec;
      } else {
        // case3. new peer came up
        LOG(INFO)
            << "Adding new peer " << peerName
            << ", support-flood-optimization: " << supportFloodOptimization;
        isNewPeer = true;
        cmdUrlUpdated = true;
        std::tie(it, std::ignore) =
            peers_.emplace(peerName, std::make_pair(newPeerSpec, newPeerCmdId));
      }

      if (cmdUrlUpdated) {
        CHECK(newPeerCmdId == it->second.second);
        LOG(INFO) << "Connecting sync channel to " << newPeerSpec.cmdUrl
                  << " with id " << newPeerCmdId;
        auto const optStatus = peerSyncSock_.setSockOpt(
            ZMQ_CONNECT_RID, newPeerCmdId.data(), newPeerCmdId.size());
        if (optStatus.hasError()) {
          LOG(FATAL) << "Error setting ZMQ_CONNECT_RID with value "
                     << newPeerCmdId;
        }
        if (peerSyncSock_.connect(fbzmq::SocketUrl{newPeerSpec.cmdUrl})
                .hasError()) {
          LOG(FATAL) << "Error connecting to URL '" << newPeerSpec.cmdUrl
                     << "'";
        }
      }

      if (isNewPeer) {
        if (supportFloodOptimization) {
          // make sure let peer to unset-child for me for all roots first
          // after that, I'll be fed with proper dual-events and I'll be
          // chosing new nexthop if need.
          unsetChildAll(peerName);
        }
      }

      // Enqueue for full dump requests
      LOG(INFO) << "Enqueuing full dump request for peer " << peerName;
      peersToSyncWith_.emplace(
          peerName,
          ExponentialBackoff<std::chrono::milliseconds>(
              Constants::kInitialBackoff, Constants::kMaxBackoff));
    } catch (std::exception const& e) {
      LOG(ERROR) << "Error connecting to: `" << peerName
                 << "` reason: " << folly::exceptionStr(e);
    }
  }
  fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));

  // process dual events if any
  if (enableFloodOptimization_) {
    for (const auto& peer : dualPeersToAdd) {
      LOG(INFO) << "dual peer up: " << peer;
      DualNode::peerUp(peer, 1 /* link-cost */); // use hop count as metric
    }
  }
}

// Send message via socket
folly::Expected<size_t, fbzmq::Error>
KvStore::sendMessageToPeer(
    const std::string& peerSocketId, const thrift::KvStoreRequest& request) {
  auto msg = fbzmq::Message::fromThriftObj(request, serializer_).value();
  tData_.addStatValue("kvstore.peers.bytes_sent", msg.size(), fbzmq::SUM);
  return peerSyncSock_.sendMultiple(
      fbzmq::Message::from(peerSocketId).value(), fbzmq::Message(), msg);
}

// delete some peers we are subscribed to
void
KvStore::delPeers(std::vector<std::string> const& peers) {
  std::vector<std::string> dualPeersToRemove;
  for (auto const& peerName : peers) {
    // not currently subscribed
    auto it = peers_.find(peerName);
    if (it == peers_.end()) {
      LOG(ERROR) << "Trying to delete non-existing peer '" << peerName << "'";
      continue;
    }

    const auto& peerSpec = it->second.first;
    if (peerSpec.supportFloodOptimization) {
      dualPeersToRemove.emplace_back(peerName);
    }

    LOG(INFO) << "Detaching from: " << peerSpec.cmdUrl
              << ", support-flood-optimization: "
              << peerSpec.supportFloodOptimization;
    auto syncRes = peerSyncSock_.disconnect(fbzmq::SocketUrl{peerSpec.cmdUrl});
    if (syncRes.hasError()) {
      LOG(ERROR) << "Failed to detach. " << syncRes.error();
    }

    peersToSyncWith_.erase(peerName);
    peers_.erase(it);
  }

  // remove dual peers if any
  if (enableFloodOptimization_) {
    for (const auto& peer : dualPeersToRemove) {
      LOG(INFO) << "dual peer down: " << peer;
      DualNode::peerDown(peer);
    }
  }
}

// Get full KEY_DUMP from peersToSyncWith_
void
KvStore::requestFullSyncFromPeers() {
  // minimal timeout for next run
  auto timeout = std::chrono::milliseconds(Constants::kMaxBackoff);

  // Make requests
  for (auto it = peersToSyncWith_.begin(); it != peersToSyncWith_.end();) {
    auto& peerName = it->first;
    auto& expBackoff = it->second;

    if (not expBackoff.canTryNow()) {
      timeout = std::min(timeout, expBackoff.getTimeRemainingUntilRetry());
      ++it;
      continue;
    }

    // Generate and send router-socket id of peer first. If the kvstore of
    // peer is not connected over the router socket then it will error out
    // exception and we will retry again.
    auto const& peerCmdSocketId = peers_.at(peerName).second;

    // Build request
    thrift::KvStoreRequest dumpRequest;
    thrift::KeyDumpParams params;

    if (filters_.hasValue()) {
      std::string keyPrefix =
          folly::join(",", filters_.value().getKeyPrefixes());
      params.prefix = keyPrefix;
      params.originatorIds = filters_.value().getOrigniatorIdList();
    }
    std::set<std::string> originator{};
    std::vector<std::string> keyPrefixList{};
    KvStoreFilters kvFilters{keyPrefixList, originator};
    params.keyValHashes = std::move(dumpHashWithFilters(kvFilters).keyVals);

    dumpRequest.cmd = thrift::Command::KEY_DUMP;
    dumpRequest.keyDumpParams = params;

    VLOG(1) << "Sending full sync request to peer " << peerName << " using id "
            << peerCmdSocketId;
    latestSentPeerSync_.emplace(
        peerCmdSocketId, std::chrono::steady_clock::now());

    auto const ret = sendMessageToPeer(peerCmdSocketId, dumpRequest);

    if (ret.hasError()) {
      // this could be pretty common on initial connection setup
      LOG(ERROR) << "Failed to send full sync request to peer " << peerName
                 << " using id " << peerCmdSocketId << " (will try again). "
                 << ret.error();
      collectSendFailureStats(ret.error(), peerCmdSocketId);
      expBackoff.reportError(); // Apply exponential backoff
      timeout = std::min(timeout, expBackoff.getTimeRemainingUntilRetry());
      ++it;
    } else {
      // Remove the iterator
      it = peersToSyncWith_.erase(it);
    }
  } // for

  // We should be able to perfom full dump from all peers. Log warning if
  // there
  // are still some peers to sync with.
  if (not peersToSyncWith_.empty()) {
    LOG(WARNING) << peersToSyncWith_.size() << " peers still require sync."
                 << "Scheduling retry after " << timeout.count() << "ms.";
    // schedule next timeout
    fullSyncTimer_->scheduleTimeout(timeout);
  }
}

// dump all peers we are subscribed to
thrift::PeerCmdReply
KvStore::dumpPeers() {
  thrift::PeerCmdReply reply;
  for (auto const& kv : peers_) {
    reply.peers.emplace(kv.first, kv.second.first);
  }
  return reply;
}

// update TTL with remainng time to expire, TTL version remains
// same so existing keys will not be updated with this TTL
void
KvStore::updatePublicationTtl(
    thrift::Publication& thriftPub, bool removeAboutToExpire) {
  auto timeNow = std::chrono::steady_clock::now();
  for (const auto& qE : ttlCountdownQueue_) {
    // Find key and ensure we are taking time from right entry from queue
    auto kv = thriftPub.keyVals.find(qE.key);
    if (kv == thriftPub.keyVals.end() or kv->second.version != qE.version or
        kv->second.originatorId != qE.originatorId or
        kv->second.ttlVersion != qE.ttlVersion) {
      continue;
    }

    // Compute timeLeft and do sanity check on it
    auto timeLeft = duration_cast<milliseconds>(qE.expiryTime - timeNow);
    if (timeLeft <= ttlDecr_) {
      thriftPub.keyVals.erase(kv);
      continue;
    }

    // filter key from publication if time left is below ttl threshold
    if (removeAboutToExpire and timeLeft < Constants::kTtlThreshold) {
      thriftPub.keyVals.erase(kv);
      continue;
    }

    // Set the time-left and decrement it by one so that ttl decrement
    // deterministically whenever it is exchanged between KvStores. This will
    // avoid looping of updates between stores.
    kv->second.ttl = timeLeft.count() - ttlDecr_.count();
  }
}

// process a request
folly::Expected<fbzmq::Message, fbzmq::Error>
KvStore::processRequestMsg(fbzmq::Message&& request) {
  auto maybeThriftReq =
      request.readThriftObj<thrift::KvStoreRequest>(serializer_);

  if (maybeThriftReq.hasError()) {
    LOG(ERROR) << "processRequest: failed reading thrift::KvStoreRequest "
               << maybeThriftReq.error();
    return folly::makeUnexpected(fbzmq::Error());
  }
  auto& thriftReq = maybeThriftReq.value();

  VLOG(3)
      << "processRequest: command: `"
      << apache::thrift::TEnumTraits<thrift::Command>::findName(thriftReq.cmd)
      << "` received";

  std::vector<std::string> keys;
  switch (thriftReq.cmd) {
  case thrift::Command::KEY_SET: {
    VLOG(3) << "Set key requested";
    if (not thriftReq.keySetParams.has_value()) {
      LOG(ERROR) << "received none keySetParams";
      return folly::makeUnexpected(fbzmq::Error());
    }

    tData_.addStatValue("kvstore.cmd_key_set", 1, fbzmq::COUNT);

    auto& ketSetParamsVal = thriftReq.keySetParams.value();
    if (ketSetParamsVal.keyVals.empty()) {
      LOG(ERROR) << "Malformed set request, ignoring";
      return folly::makeUnexpected(fbzmq::Error());
    }

    // Update hash for key-values
    for (auto& kv : ketSetParamsVal.keyVals) {
      auto& value = kv.second;
      if (value.value.hasValue()) {
        value.hash =
            generateHash(value.version, value.originatorId, value.value);
      }
    }

    // Create publication and merge it with local KvStore
    thrift::Publication rcvdPublication;
    rcvdPublication.keyVals = std::move(ketSetParamsVal.keyVals);
    rcvdPublication.nodeIds = std::move(ketSetParamsVal.nodeIds);
    rcvdPublication.floodRootId = std::move(ketSetParamsVal.floodRootId);
    mergePublication(rcvdPublication);

    // respond to the client
    if (ketSetParamsVal.solicitResponse) {
      return fbzmq::Message::from(Constants::kSuccessResponse.toString());
    }
    return fbzmq::Message();
  }
  case thrift::Command::KEY_GET: {
    VLOG(3) << "Get key requested";
    if (not thriftReq.keyGetParams.has_value()) {
      LOG(ERROR) << "received none keyGetParams";
      return folly::makeUnexpected(fbzmq::Error());
    }

    tData_.addStatValue("kvstore.cmd_key_get", 1, fbzmq::COUNT);

    auto thriftPub = getKeyVals(thriftReq.keyGetParams.value().keys);
    updatePublicationTtl(thriftPub);
    return fbzmq::Message::fromThriftObj(thriftPub, serializer_);
  }
  case thrift::Command::KEY_DUMP: {
    VLOG(3) << "Dump all keys requested";
    if (not thriftReq.keyDumpParams.has_value()) {
      LOG(ERROR) << "received none keyDumpParams";
      return folly::makeUnexpected(fbzmq::Error());
    }

    auto& keyDumpParamsVal = thriftReq.keyDumpParams.value();
    if (keyDumpParamsVal.keyValHashes.hasValue()) {
      VLOG(3) << "Dump keys requested along with "
              << keyDumpParamsVal.keyValHashes.value().size()
              << " keyValHashes item(s) provided from peer";
    } else {
      VLOG(3) << "Dump all keys requested - "
              << "KeyPrefixes:" << keyDumpParamsVal.prefix << " Originator IDs:"
              << folly::join(",", keyDumpParamsVal.originatorIds);
    }
    // TODO, add per request id counters in thrift server
    tData_.addStatValue("kvstore.cmd_key_dump", 1, fbzmq::COUNT);

    std::vector<std::string> keyPrefixList;
    folly::split(",", keyDumpParamsVal.prefix, keyPrefixList, true);
    const auto keyPrefixMatch =
        KvStoreFilters(keyPrefixList, keyDumpParamsVal.originatorIds);
    auto thriftPub = dumpAllWithFilters(keyPrefixMatch);
    if (keyDumpParamsVal.keyValHashes.hasValue()) {
      thriftPub = dumpDifference(
          thriftPub.keyVals, keyDumpParamsVal.keyValHashes.value());
    }
    updatePublicationTtl(thriftPub);
    // I'm the initiator, set flood-root-id
    thriftPub.floodRootId = DualNode::getSptRootId();
    return fbzmq::Message::fromThriftObj(thriftPub, serializer_);
  }
  case thrift::Command::HASH_DUMP: {
    VLOG(3) << "Dump all hashes requested";
    if (not thriftReq.keyDumpParams.has_value()) {
      LOG(ERROR) << "received none keyDumpParams";
      return folly::makeUnexpected(fbzmq::Error());
    }

    tData_.addStatValue("kvstore.cmd_hash_dump", 1, fbzmq::COUNT);

    std::set<std::string> originator{};
    std::vector<std::string> keyPrefixList{};
    folly::split(
        ",", thriftReq.keyDumpParams.value().prefix, keyPrefixList, true);
    KvStoreFilters kvFilters{keyPrefixList, originator};
    auto hashDump = dumpHashWithFilters(kvFilters);
    updatePublicationTtl(hashDump);
    return fbzmq::Message::fromThriftObj(hashDump, serializer_);
  }
  case thrift::Command::COUNTERS_GET: {
    VLOG(3) << "Counters are requested";
    fbzmq::thrift::CounterValuesResponse counters{apache::thrift::FRAGILE,
                                                  getCounters()};
    return fbzmq::Message::fromThriftObj(counters, serializer_);
  }
  case thrift::Command::PEER_ADD: {
    VLOG(2) << "Peer addition requested";
    tData_.addStatValue("kvstore.cmd_peer_add", 1, fbzmq::COUNT);

    if (not thriftReq.peerAddParams.hasValue()) {
      LOG(ERROR) << "received none peerAddParams";
      return folly::makeUnexpected(fbzmq::Error());
    }
    if (thriftReq.peerAddParams.value().peers.empty()) {
      LOG(ERROR) << "Malformed peer-add request, ignoring";
      return folly::makeUnexpected(fbzmq::Error());
    }
    addPeers(thriftReq.peerAddParams.value().peers);
    return fbzmq::Message::fromThriftObj(dumpPeers(), serializer_);
  }
  case thrift::Command::PEER_DEL: {
    VLOG(2) << "Peer deletion requested";
    tData_.addStatValue("kvstore.cmd_per_del", 1, fbzmq::COUNT);

    if (not thriftReq.peerDelParams.hasValue()) {
      LOG(ERROR) << "received none peerDelParams";
      return folly::makeUnexpected(fbzmq::Error());
    }
    if (thriftReq.peerDelParams.value().peerNames.empty()) {
      LOG(ERROR) << "Malformed peer-del request, ignoring";
      return folly::makeUnexpected(fbzmq::Error());
    }
    delPeers(thriftReq.peerDelParams.value().peerNames);
    return fbzmq::Message::fromThriftObj(dumpPeers(), serializer_);
  }
  case thrift::Command::PEER_DUMP: {
    VLOG(2) << "Peer dump requested";
    tData_.addStatValue("kvstore.cmd_peer_dump", 1, fbzmq::COUNT);
    return fbzmq::Message::fromThriftObj(dumpPeers(), serializer_);
  }
  case thrift::Command::DUAL: {
    VLOG(2) << "DUAL messages received";
    if (not thriftReq.dualMessages.hasValue()) {
      LOG(ERROR) << "received none dualMessages";
      return fbzmq::Message(); // ignore it
    }
    if (thriftReq.dualMessages.value().messages.empty()) {
      LOG(ERROR) << "received empty dualMessages";
      return fbzmq::Message(); // ignore it
    }
    tData_.addStatValue("kvstore.received_dual_messages", 1, fbzmq::COUNT);
    DualNode::processDualMessages(std::move(*thriftReq.dualMessages));
    return fbzmq::Message();
  }
  case thrift::Command::FLOOD_TOPO_SET: {
    VLOG(2) << "FLOOD_TOPO_SET command requested";
    if (not thriftReq.floodTopoSetParams.hasValue()) {
      LOG(ERROR) << "received none floodTopoSetParams";
      return fbzmq::Message(); // ignore it
    }
    processFloodTopoSet(std::move(*thriftReq.floodTopoSetParams));
    return fbzmq::Message();
  }
  case thrift::Command::FLOOD_TOPO_GET: {
    VLOG(3) << "FLOOD_TOPO_GET command requested";
    return fbzmq::Message::fromThriftObj(processFloodTopoGet(), serializer_);
  }
  default: {
    LOG(ERROR) << "Unknown command received";
    return folly::makeUnexpected(fbzmq::Error());
  }
  }
}

thrift::SptInfos
KvStore::processFloodTopoGet() noexcept {
  thrift::SptInfos sptInfos;
  const auto& duals = DualNode::getDuals();

  // set spt-infos
  for (const auto& kv : duals) {
    const auto& rootId = kv.first;
    const auto& info = kv.second.getInfo();
    thrift::SptInfo sptInfo;
    sptInfo.passive = info.sm.state == DualState::PASSIVE;
    sptInfo.cost = info.distance;
    sptInfo.parent = info.nexthop;
    sptInfo.children = kv.second.children();
    sptInfos.infos.emplace(rootId, sptInfo);
  }

  // set counters
  sptInfos.counters = DualNode::getCounters();

  // set flood root-id and peers
  sptInfos.floodRootId = DualNode::getSptRootId();
  sptInfos.floodPeers = getFloodPeers(sptInfos.floodRootId);
  return sptInfos;
}

void
KvStore::processFloodTopoSet(
    const thrift::FloodTopoSetParams& setParams) noexcept {
  if (setParams.allRoots.hasValue() and *setParams.allRoots and
      not setParams.setChild) {
    // process unset-child for all-roots command
    auto& duals = DualNode::getDuals();
    for (auto& kv : duals) {
      kv.second.removeChild(setParams.srcId);
    }
    return;
  }

  if (not DualNode::hasDual(setParams.rootId)) {
    LOG(ERROR) << "processFloodTopoSet unknown root-id: " << setParams.rootId;
    return;
  }
  auto& dual = DualNode::getDual(setParams.rootId);
  const auto& child = setParams.srcId;
  if (setParams.setChild) {
    // set child command
    LOG(INFO) << "dual child set: root-id: (" << setParams.rootId
              << ") child: " << setParams.srcId;
    dual.addChild(child);
  } else {
    // unset child command
    LOG(INFO) << "dual child unset: root-id: (" << setParams.rootId
              << ") child: " << setParams.srcId;
    dual.removeChild(child);
  }
}

void
KvStore::sendTopoSetCmd(
    const std::string& rootId,
    const std::string& peerName,
    bool setChild,
    bool allRoots) noexcept {
  const auto& dstCmdSocketId = peers_.at(peerName).second;
  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::FLOOD_TOPO_SET;

  thrift::FloodTopoSetParams setParams;
  setParams.rootId = rootId;
  setParams.srcId = nodeId_;
  setParams.setChild = setChild;
  if (allRoots) {
    setParams.allRoots = allRoots;
  }
  request.floodTopoSetParams = setParams;

  const auto ret = sendMessageToPeer(dstCmdSocketId, request);
  if (ret.hasError()) {
    LOG(ERROR) << rootId << ": failed to " << (setChild ? "set" : "unset")
               << " spt-parent " << peerName << ", error: " << ret.error();
    collectSendFailureStats(ret.error(), dstCmdSocketId);
  }
}

void
KvStore::setChild(
    const std::string& rootId, const std::string& peerName) noexcept {
  sendTopoSetCmd(rootId, peerName, true, false);
}

void
KvStore::unsetChild(
    const std::string& rootId, const std::string& peerName) noexcept {
  sendTopoSetCmd(rootId, peerName, false, false);
}

void
KvStore::unsetChildAll(const std::string& peerName) noexcept {
  sendTopoSetCmd("" /* root-id is ignored */, peerName, false, true);
}

void
KvStore::processNexthopChange(
    const std::string& rootId,
    const folly::Optional<std::string>& oldNh,
    const folly::Optional<std::string>& newNh) noexcept {
  // sanity check
  std::string oldNhStr = oldNh.hasValue() ? *oldNh : "none";
  std::string newNhStr = newNh.hasValue() ? *newNh : "none";
  CHECK(oldNh != newNh)
      << rootId
      << ": callback invoked while nexthop does not change: " << oldNhStr;
  // root should NEVER change its nexthop (nexthop always equal to myself)
  CHECK_NE(nodeId_, rootId);
  LOG(INFO) << "dual nexthop change: root-id (" << rootId << ") " << oldNhStr
            << " -> " << newNhStr;

  // set new parent if any
  if (newNh.hasValue()) {
    // peers_ MUST have this new parent
    // if peers_ does not have this peer, that means KvStore already recevied
    // NEIGHBOR-DOWN event (so does dual), but dual still think I should have
    // this neighbor as nexthop, then something is wrong with DUAL
    CHECK(peers_.count(*newNh))
        << rootId << ": trying to set new spt-parent who does not exist "
        << *newNh;
    CHECK_NE(nodeId_, *newNh) << "new nexthop is myself";
    setChild(rootId, *newNh);

    // Enqueue new-nexthop for full-sync (insert only if entry doesn't exists)
    // NOTE we have to perform full-sync after we do FLOOD_TOPO_SET, so that
    // we can be sure that I won't be in a disconnected state after we got
    // full synced. (ps: full-sync is 3-way-sync, one direction sync should be
    // good enough)
    LOG(INFO) << "dual full-sync with " << *newNh;
    peersToSyncWith_.emplace(
        *newNh,
        ExponentialBackoff<std::chrono::milliseconds>(
            Constants::kInitialBackoff, Constants::kMaxBackoff));

    // initial full sync request if peersToSyncWith_ was empty
    if (not fullSyncTimer_->isScheduled()) {
      fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
    }
  }

  // unset old parent if any
  if (oldNh.hasValue() and peers_.count(*oldNh)) {
    // valid old parent AND it's still my peer, unset it
    CHECK_NE(nodeId_, *oldNh) << "old nexthop was myself";
    // unset it
    unsetChild(rootId, *oldNh);
  }
}

void
KvStore::processSyncResponse() noexcept {
  VLOG(4) << "awaiting for sync response message";

  fbzmq::Message requestIdMsg, delimMsg, syncPubMsg;

  auto ret = peerSyncSock_.recvMultiple(requestIdMsg, delimMsg, syncPubMsg);
  if (ret.hasError()) {
    LOG(ERROR) << "processSyncResponse: failed processing syncRespone: "
               << ret.error();
    return;
  }

  // at this point we received all three parts
  if (not delimMsg.empty()) {
    LOG(ERROR) << "processSyncResponse: unexpected delimiter: "
               << delimMsg.read<std::string>().value();
    return;
  }

  auto const requestId = requestIdMsg.read<std::string>().value();

  // syncPubMsg can be of two types
  // 1. ack to SET_KEY ("OK" or "ERR")
  // 2. response of KEY_DUMP (thrift::Publication)
  // We check for first one and then fallback to second one
  if (syncPubMsg.size() < 3) {
    auto syncPubStr = syncPubMsg.read<std::string>().value();
    if (syncPubStr == Constants::kErrorResponse) {
      LOG(ERROR) << "Got error for sent publication from " << requestId;
      return;
    }
    if (syncPubStr == Constants::kSuccessResponse) {
      VLOG(2) << "Got ack for sent publication on " << requestId;
      return;
    }
  }

  tData_.addStatValue(
      "kvstore.peers.bytes_received", syncPubMsg.size(), fbzmq::SUM);
  // Perform error check
  auto maybeSyncPub =
      syncPubMsg.readThriftObj<thrift::Publication>(serializer_);
  if (maybeSyncPub.hasError()) {
    LOG(ERROR) << "Received bad response on peerSyncSock_";
    return;
  }

  const auto& syncPub = maybeSyncPub.value();
  const size_t kvUpdateCnt = mergePublication(syncPub, requestId);
  LOG(INFO) << "Sync response received from " << requestId << " with "
            << syncPub.keyVals.size() << " key value pairs which incured "
            << kvUpdateCnt << " key-value updates";

  if (latestSentPeerSync_.count(requestId)) {
    auto syncDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - latestSentPeerSync_.at(requestId));
    tData_.addStatValue(
        "kvstore.peer_sync_time_ms", syncDuration.count(), fbzmq::AVG);
    VLOG(1) << "It took " << syncDuration.count() << " ms to sync with "
            << requestId;
    latestSentPeerSync_.erase(requestId);
  }
}

// send sync request from one neighbor randomly
void
KvStore::requestSync() {
  SCOPE_EXIT {
    auto base = dbSyncInterval_.count();
    std::default_random_engine generator;
    // add 20% variance
    std::uniform_int_distribution<int> distribution(-0.2 * base, 0.2 * base);
    auto roll = std::bind(distribution, generator);
    auto period = std::chrono::milliseconds((base + roll()) * 1000);

    // Schedule next sync with peers
    scheduleTimeout(period, [this]() { requestSync(); });
  };

  if (peers_.empty()) {
    return;
  }

  // Randomly select one neighbor to request full-dump from
  int randomIndex = folly::Random::rand32() % peers_.size();
  int index{0};
  std::string randomNeighbor;

  for (auto const& kv : peers_) {
    if (index++ == randomIndex) {
      randomNeighbor = kv.first;
      break;
    }
  }

  // Enqueue neighbor for full-sync (insert only if entry doesn't exists)
  LOG(INFO) << "Requesting periodic sync from " << randomNeighbor;
  peersToSyncWith_.emplace(
      randomNeighbor,
      ExponentialBackoff<std::chrono::milliseconds>(
          Constants::kInitialBackoff, Constants::kMaxBackoff));

  // initial full sync request if peersToSyncWith_ was empty
  if (not fullSyncTimer_->isScheduled()) {
    fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  }
}

// this will poll the sockets listening to the requests
void
KvStore::attachCallbacks() {
  VLOG(2) << "KvStore: Registering events callbacks ...";

  addSocket(
      fbzmq::RawZmqSocketPtr{*peerSyncSock_}, ZMQ_POLLIN, [this](int) noexcept {
        // we received a sync response
        VLOG(3) << "KvStore: sync response received";
        processSyncResponse();
      });

  // Perform full sync if there are peers to sync with.
  fullSyncTimer_ = fbzmq::ZmqTimeout::make(
      this, [this]() noexcept { requestFullSyncFromPeers(); });

  // Schedule periodic call to re-sync with one of our peer
  scheduleTimeout(
      std::chrono::milliseconds(0), [this]() noexcept { requestSync(); });
}

void
KvStore::cleanupTtlCountdownQueue() {
  // record all expired keys
  std::vector<std::string> expiredKeys;
  auto now = std::chrono::steady_clock::now();

  // Iterate through ttlCountdownQueue_ until the top expires in the future
  while (not ttlCountdownQueue_.empty()) {
    auto top = ttlCountdownQueue_.top();
    if (top.expiryTime > now) {
      // Nothing in queue worth evicting
      break;
    }
    auto it = kvStore_.find(top.key);
    if (it != kvStore_.end() and it->second.version == top.version and
        it->second.originatorId == top.originatorId and
        it->second.ttlVersion == top.ttlVersion) {
      expiredKeys.emplace_back(top.key);
      LOG(WARNING)
          << "Delete expired (key, version, originatorId, ttlVersion, node) "
          << folly::sformat(
                 "({}, {}, {}, {}, {})",
                 top.key,
                 it->second.version,
                 it->second.originatorId,
                 it->second.ttlVersion,
                 nodeId_);
      logKvEvent("KEY_EXPIRE", top.key);
      kvStore_.erase(it);
    }
    ttlCountdownQueue_.pop();
  }

  // Reschedule based on most recent timeout
  if (not ttlCountdownQueue_.empty()) {
    ttlCountdownTimer_->scheduleTimeout(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ttlCountdownQueue_.top().expiryTime - now));
  }

  if (expiredKeys.empty()) {
    // no key expires
    return;
  }
  tData_.addStatValue(
      "kvstore.expired_key_vals", expiredKeys.size(), fbzmq::SUM);
  thrift::Publication expiredKeysPub{};
  expiredKeysPub.expiredKeys = std::move(expiredKeys);
  floodPublication(std::move(expiredKeysPub));
}

void
KvStore::bufferPublication(thrift::Publication&& publication) {
  tData_.addStatValue("kvstore.rate_limit_suppress", 1, fbzmq::COUNT);
  tData_.addStatValue(
      "kvstore.rate_limit_keys", publication.keyVals.size(), fbzmq::AVG);
  // update or add keys
  for (auto const& kv : publication.keyVals) {
    publicationBuffer_[publication.floodRootId].emplace(kv.first);
  }
  for (auto const& key : publication.expiredKeys) {
    publicationBuffer_[publication.floodRootId].emplace(key);
  }
}

void
KvStore::floodBufferedUpdates() {
  if (!publicationBuffer_.size()) {
    return;
  }

  // merged-publications to be sent
  std::vector<thrift::Publication> publications;

  // merge publication per root-id
  for (const auto& kv : publicationBuffer_) {
    thrift::Publication publication{};
    publication.floodRootId = kv.first;
    for (const auto& key : kv.second) {
      auto kvStoreIt = kvStore_.find(key);
      if (kvStoreIt != kvStore_.end()) {
        publication.keyVals.emplace(make_pair(key, kvStoreIt->second));
      } else {
        publication.expiredKeys.emplace_back(key);
      }
    }
    publications.emplace_back(std::move(publication));
  }

  publicationBuffer_.clear();

  for (auto& pub : publications) {
    // when sending out merged publication, we maintain orginal-root-id
    // we act as a forwarder, NOT an initiator. Disable set-flood-root here
    floodPublication(
        std::move(pub), false /* rate-limit */, false /* set-flood-root */);
  }
}

void
KvStore::finalizeFullSync(
    const std::vector<std::string>& keys, const std::string& senderId) {
  if (keys.empty()) {
    return;
  }
  VLOG(1) << " finalizeFullSync back to: " << senderId
          << " with keys: " << folly::join(",", keys);

  // build keyval to be sent
  std::unordered_map<std::string, thrift::Value> keyVals;
  for (const auto& key : keys) {
    const auto& it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      keyVals.emplace(key, it->second);
    }
  }

  thrift::KvStoreRequest updateRequest;
  thrift::KeySetParams params;

  params.keyVals = std::move(keyVals);
  params.solicitResponse = false;
  // I'm the initiator, set flood-root-id
  params.floodRootId = DualNode::getSptRootId();

  updateRequest.cmd = thrift::Command::KEY_SET;
  updateRequest.keySetParams = params;

  VLOG(1) << "sending finalizeFullSync back to " << senderId;
  auto const ret = sendMessageToPeer(senderId, updateRequest);
  if (ret.hasError()) {
    // this could fail when senderId goes offline
    LOG(ERROR) << "Failed to send finalizeFullSync to " << senderId
               << " using id " << senderId << ", error: " << ret.error();
    collectSendFailureStats(ret.error(), senderId);
  }
}

std::unordered_set<std::string>
KvStore::getFloodPeers(const folly::Optional<std::string>& rootId) {
  auto sptPeers = DualNode::getSptPeers(rootId);
  bool floodToAll = false;
  if (not enableFloodOptimization_ or not useFloodOptimization_ or
      sptPeers.empty()) {
    // fall back to naive flooding if feature not enabled or can not find
    // valid SPT-peers
    floodToAll = true;
  }

  // flood-peers: SPT-peers + peers-who-does-not-support-dual
  std::unordered_set<std::string> floodPeers;
  for (const auto& kv : peers_) {
    const auto& peer = kv.first;
    const auto& peerSpec = kv.second.first;
    if (floodToAll or sptPeers.count(peer) != 0 or
        not peerSpec.supportFloodOptimization) {
      floodPeers.emplace(peer);
    }
  }
  return floodPeers;
}

void
KvStore::collectSendFailureStats(
    const fbzmq::Error& error, const std::string& dstSockId) {
  tData_.addStatValue(
      folly::sformat("kvstore.send_failure.{}.{}", dstSockId, error.errNum),
      1,
      fbzmq::COUNT);
}

void
KvStore::floodPublication(
    thrift::Publication&& publication, bool rateLimit, bool setFloodRoot) {
  // rate limit if configured
  if (floodLimiter_ && rateLimit && !floodLimiter_->consume(1)) {
    bufferPublication(std::move(publication));
    pendingPublicationTimer_->scheduleTimeout(
        Constants::kFloodPendingPublication, false);
    return;
  }
  // merge with buffered publication and flood
  if (publicationBuffer_.size()) {
    bufferPublication(std::move(publication));
    return floodBufferedUpdates();
  }
  // Update ttl on keys we are trying to advertise. Also remove keys which
  // are about to expire.
  updatePublicationTtl(publication, true);

  // If there are no changes then return
  if (publication.keyVals.empty() && publication.expiredKeys.empty()) {
    return;
  }

  // Find from whom we might have got this publication. Last entry is our ID
  // and hence second last entry is the node from whom we get this publication
  folly::Optional<std::string> senderId;
  if (publication.nodeIds.hasValue() and publication.nodeIds->size()) {
    senderId = publication.nodeIds->back();
  }
  if (not publication.nodeIds.hasValue()) {
    publication.nodeIds = std::vector<std::string>{};
  }
  publication.nodeIds->emplace_back(nodeId_);

  // Flood publication on local PUB socket
  //
  // Usually only local subscribers need to know, but we are also sending
  // on global socket so that it can help debugging things via breeze as
  // well as preserve backward compatibility
  auto const msg =
      fbzmq::Message::fromThriftObj(publication, serializer_).value();
  localPubSock_.sendOne(msg);
  globalPubSock_.sendOne(msg);

  //
  // Create request and send only keyValue updates to all neighbors
  //
  if (publication.keyVals.empty()) {
    return;
  }

  if (setFloodRoot and not senderId.hasValue()) {
    // I'm the initiator, set flood-root-id
    publication.floodRootId = DualNode::getSptRootId();
  }

  thrift::KvStoreRequest floodRequest;
  thrift::KeySetParams params;

  params.keyVals = publication.keyVals;
  params.solicitResponse = false;
  params.nodeIds = publication.nodeIds;
  params.floodRootId = publication.floodRootId;

  floodRequest.cmd = thrift::Command::KEY_SET;
  floodRequest.keySetParams = params;

  const auto& floodPeers = getFloodPeers(params.floodRootId);
  for (const auto& peer : floodPeers) {
    if (senderId.hasValue() && senderId.value() == peer) {
      // Do not flood towards senderId from whom we received this publication
      continue;
    }
    VLOG(4) << "Forwarding publication, received from: "
            << (senderId.hasValue() ? senderId.value() : "N/A")
            << ", to: " << peer << ", via: " << nodeId_;

    tData_.addStatValue("kvstore.sent_publications", 1, fbzmq::COUNT);
    tData_.addStatValue(
        "kvstore.sent_key_vals", publication.keyVals.size(), fbzmq::SUM);

    // Send flood request
    auto const& peerCmdSocketId = peers_.at(peer).second;
    auto const ret = sendMessageToPeer(peerCmdSocketId, floodRequest);
    if (ret.hasError()) {
      // this could be pretty common on initial connection setup
      LOG(ERROR) << "Failed to flood publication to peer " << peer
                 << " using id " << peerCmdSocketId
                 << ", error: " << ret.error();
      collectSendFailureStats(ret.error(), peerCmdSocketId);
    }
  }
}

size_t
KvStore::mergePublication(
    const thrift::Publication& rcvdPublication,
    folly::Optional<std::string> senderId) {
  // Add counters
  tData_.addStatValue("kvstore.received_publications", 1, fbzmq::COUNT);
  tData_.addStatValue(
      "kvstore.received_key_vals", rcvdPublication.keyVals.size(), fbzmq::SUM);

  const bool needFinalizeFullSync = senderId.hasValue() and
      rcvdPublication.tobeUpdatedKeys.hasValue() and
      not rcvdPublication.tobeUpdatedKeys->empty();

  // This can happen when KvStore is emitting expired-key updates
  if (rcvdPublication.keyVals.empty() and not needFinalizeFullSync) {
    return 0;
  }

  // Check for loop
  const auto& nodeIds = rcvdPublication.nodeIds;
  if (nodeIds.hasValue() and
      std::find(nodeIds->begin(), nodeIds->end(), nodeId_) != nodeIds->end()) {
    tData_.addStatValue("kvstore.looped_publications", 1, fbzmq::COUNT);
    return 0;
  }

  // Generate delta with local KvStore
  thrift::Publication deltaPublication;
  deltaPublication.keyVals =
      mergeKeyValues(kvStore_, rcvdPublication.keyVals, filters_);
  deltaPublication.floodRootId = rcvdPublication.floodRootId;

  const size_t kvUpdateCnt = deltaPublication.keyVals.size();
  tData_.addStatValue("kvstore.updated_key_vals", kvUpdateCnt, fbzmq::SUM);

  // Populate nodeIds and our nodeId_ to the end
  if (rcvdPublication.nodeIds.hasValue()) {
    deltaPublication.nodeIds = rcvdPublication.nodeIds;
  }

  // Update ttl values of keys
  updateTtlCountdownQueue(deltaPublication);

  if (not deltaPublication.keyVals.empty()) {
    // Flood change to all of our neighbors/subscribers
    floodPublication(std::move(deltaPublication));
  } else {
    // Keep track of received publications which din't update any field
    tData_.addStatValue(
        "kvstore.received_redundant_publications", 1, fbzmq::COUNT);
  }

  // response to senderId with tobeUpdatedKeys + Vals
  // (last step in 3-way full-sync)
  if (needFinalizeFullSync) {
    finalizeFullSync(*rcvdPublication.tobeUpdatedKeys, *senderId);
  }

  return kvUpdateCnt;
}

fbzmq::thrift::CounterMap
KvStore::getCounters() {
  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();

  // Add some more flat counters
  counters["kvstore.num_keys"] = kvStore_.size();
  counters["kvstore.num_peers"] = peers_.size();
  counters["kvstore.pending_full_sync"] = peersToSyncWith_.size();
  counters["kvstore.zmq_event_queue_size"] = getEventQueueSize();

  return prepareSubmitCounters(std::move(counters));
}

void
KvStore::submitCounters() {
  VLOG(3) << "Submitting counters ... ";
  zmqMonitorClient_->setCounters(getCounters());
}

void
KvStore::logKvEvent(const std::string& event, const std::string& key) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("node_name", nodeId_);
  sample.addString("key", key);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

bool
KvStore::sendDualMessages(
    const std::string& neighbor, const thrift::DualMessages& msgs) noexcept {
  if (peers_.count(neighbor) == 0) {
    LOG(ERROR) << "fail to send dual messages to " << neighbor << ", not exist";
    return false;
  }
  const auto& neighborCmdSocketId = peers_.at(neighbor).second;
  thrift::KvStoreRequest dualRequest;
  dualRequest.cmd = thrift::Command::DUAL;
  dualRequest.dualMessages = msgs;
  const auto ret = sendMessageToPeer(neighborCmdSocketId, dualRequest);
  // TODO: for dual.query, we need to use a blocking socket to get a ack
  // from destination node to know if it receives or not
  // due to zmq async fashion, ret here is always true even on failure
  if (ret.hasError()) {
    LOG(ERROR) << "failed to send dual messages to " << neighbor << " using id "
               << neighborCmdSocketId << ", error: " << ret.error();
    collectSendFailureStats(ret.error(), neighborCmdSocketId);
    return false;
  }
  return true;
}

} // namespace openr
