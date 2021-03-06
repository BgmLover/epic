// Copyright (c) 2019 EPI-ONE Core Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "dag_manager.h"
#include "block_store.h"
#include "peer_manager.h"
#include "rpc_server.h"

DAGManager::DAGManager() : verifyThread_(1), syncPool_(1), storagePool_(1) {
    milestoneChains_.push(std::make_unique<Chain>());
    msVertices_.emplace(GENESIS->GetHash(), GENESIS_VERTEX);

    // Start threadpools
    verifyThread_.Start();
    syncPool_.Start();
    storagePool_.Start();
}

bool DAGManager::Init() {
    // dag should have only one chain when calling Init()
    return milestoneChains_.size() == 1;
}

/////////////////////////////////////
// Synchronization specific methods
//

void DAGManager::RequestInv(uint256 fromHash, const size_t& length, PeerPtr peer) {
    syncPool_.Execute([peer = std::move(peer), fromHash = std::move(fromHash), length, this]() {
        std::vector<uint256> locator = ConstructLocator(fromHash, length, peer);
        if (locator.empty()) {
            spdlog::debug("RequestInv return: locator is null");
            return;
        }

        peer->SetLastGetInvEnd(locator.back());
        peer->SetLastGetInvLength(locator.size());

        auto task = std::make_shared<GetInvTask>(sync_task_timeout);
        peer->AddPendingGetInvTask(task);
        peer->SendMessage(std::make_unique<GetInv>(locator, task->nonce));
    });
}

void DAGManager::CallbackRequestInv(std::unique_ptr<Inv> inv, PeerPtr peer) {
    syncPool_.Execute([inv = std::move(inv), peer = std::move(peer), this]() {
        auto& result = inv->hashes;
        if (result.empty()) {
            spdlog::info("Received an empty inv, which means we have reached the same height as the peer's {}.",
                         peer->address.ToString());
            auto task = std::make_shared<GetDataTask>(GetDataTask::PENDING_SET, sync_task_timeout);
            peer->AddPendingGetDataTask(task);
            auto pending_request = std::make_unique<GetData>(task->type);
            pending_request->AddPendingSetNonce(task->nonce);
            peer->SendMessage(std::move(pending_request));
        } else if (result.size() == 1 && result.at(0) == GENESIS->GetHash()) {
            if (peer->GetLastGetInvEnd() == GENESIS->GetHash()) {
                spdlog::info("peer {} response fork to genesis hash request", peer->address.ToString());
                peer->Disconnect();
                return;
            }

            size_t length = 2 * peer->GetLastGetInvLength();
            if (length >= max_get_inv_length) {
                length = max_get_inv_length;
            }
            RequestInv(peer->GetLastGetInvEnd(), length, peer);
            spdlog::debug("We are probably on a fork... sending a larger locator.");
        } else {
            RequestData(result, peer);
        }

        peer->RemovePendingGetInvTask(inv->nonce);
    });
}

void DAGManager::RespondRequestInv(std::vector<uint256>& locator, uint32_t nonce, PeerPtr peer) {
    syncPool_.Execute([peer = std::move(peer), locator = std::move(locator), nonce, this]() {
        std::vector<uint256> hashes;
        for (const uint256& start : locator) {
            if (start == GetMilestoneHead()->cblock->GetHash()) {
                // The peer already reach our head. Send an empty inv.
                spdlog::debug("The peer should already reach our head. Sending empty inv. "
                              "Last bundle sent to this peer: {}",
                              std::to_string(peer->GetLastSentBundleHash()));
                std::vector<uint256> empty;
                peer->SendMessage(std::make_unique<Inv>(empty, nonce));
                return;
            }
            if (IsMainChainMS(start)) {
                auto startMs = GetMsVertex(start);
                // This locator intersects with our database. We now have a starting point.
                // Traverse the milestone chain forward from the starting point itself.
                spdlog::debug("Constructing inv... Found a starting point of height {}", startMs->height);
                hashes = TraverseMilestoneForward(startMs, Inv::kMaxInventorySize);
                break;
            }
        }

        if (hashes.empty()) {
            // Cannot locate the peer's position. Send a genesis hash.
            hashes.push_back(GENESIS->GetHash());
        } else {
            uint256 lih = peer->GetLastSentInvHash();
            uint256 lbh = peer->GetLastSentBundleHash();

            // First, find the if the hashes to be sent contain the most recent
            // ms hash that we have sent to the peer via either Inv or Bundle,
            // to avoid duplicated GetData requests.
            auto it = std::find(hashes.begin(), hashes.end(), lih);
            if (it == hashes.end()) {
                lih.SetNull();
                it = std::find(hashes.begin(), hashes.end(), lbh);
            }

            // If found, remove all the hashes come before it,
            // and only keep the sublist starting from its next
            if (it != hashes.end()) {
                std::vector<uint256> sublist(it + 1, hashes.end());
                hashes.swap(sublist);
                sublist.clear();
            }

            if (hashes.empty()) {
                spdlog::debug("Sublist of inv is empty. Sending empty inv. "
                              "Last {} sent to this peer: {}",
                              lih.IsNull() ? "bundle" : "inv",
                              lih.IsNull() ? std::to_string(lbh) : std::to_string(lih));
            } else {
                peer->SetLastSentInvHash(hashes.back());
            }
        }

        peer->SendMessage(std::make_unique<Inv>(hashes, nonce));
    });
}

void DAGManager::RespondRequestPending(uint32_t nonce, const PeerPtr& peer) const {
    peer->SendMessage(std::make_unique<Bundle>(GetBestChain()->GetPendingBlocks(), nonce));
}

void DAGManager::RespondRequestLVS(const std::vector<uint256>& hashes,
                                   const std::vector<uint32_t>& nonces,
                                   PeerPtr peer) {
    assert(hashes.size() == nonces.size());
    auto hs_iter = hashes.begin();
    auto nc_iter = nonces.begin();
    while (hs_iter != hashes.end() && nc_iter != nonces.end()) {
        syncPool_.Execute([n = *nc_iter, h = *hs_iter, peer, this]() {
            auto bundle  = std::make_unique<Bundle>(n);
            auto payload = GetMainChainRawLevelSet(h);
            if (payload.empty()) {
                spdlog::debug("Milestone {} cannot be found. Sending a Not Found Message instead", h.to_substr());
                peer->SendMessage(std::make_unique<NotFound>(h, n));
                return;
            }

            bundle->SetPayload(std::move(payload));
            spdlog::debug("Sending bundle of LVS with nonce {} with MS hash {} to peer {}", n, h.to_substr(),
                          peer->address.ToString());
            peer->SetLastSentBundleHash(h);
            peer->SendMessage(std::move(bundle));
        });
        hs_iter++;
        nc_iter++;
    }
}

void DAGManager::RequestData(std::vector<uint256>& requests, const PeerPtr& requestFrom) {
    auto message = std::make_unique<GetData>(GetDataTask::LEVEL_SET);
    for (auto& h : requests) {
        if (downloading_.contains(h) || STORE->DAGExists(h)) {
            continue;
        }

        auto task = std::make_shared<GetDataTask>(GetDataTask::LEVEL_SET, h, sync_task_timeout);
        message->AddItem(h, task->nonce);
        requestFrom->AddPendingGetDataTask(task);
        downloading_.insert(h);

        if (message->hashes.size() >= maxGetDataSize) {
            spdlog::debug("Requesting lvs {} to {}", message->hashes.front().to_substr(),
                          message->hashes.back().to_substr());
            requestFrom->SendMessage(std::move(message));
            message = std::make_unique<GetData>(GetDataTask::LEVEL_SET);
        }
    }

    if (!message->hashes.empty()) {
        spdlog::debug("Requesting lvs {} to {}", message->hashes.front().to_substr(),
                      message->hashes.back().to_substr());
        requestFrom->SendMessage(std::move(message));
    }
}

std::vector<uint256> DAGManager::ConstructLocator(const uint256& fromHash, size_t length, const PeerPtr& peer) {
    const VertexPtr startMilestone = fromHash.IsNull() ? GetMilestoneHead() : GetMsVertex(fromHash);
    if (!startMilestone) {
        return {};
    }

    return TraverseMilestoneBackward(startMilestone, length);
}

std::vector<uint256> DAGManager::TraverseMilestoneBackward(VertexPtr cursor, size_t length) const {
    std::vector<uint256> result;
    result.reserve(length);

    for (int i = 0; i < length; ++i) {
        assert(cursor);
        assert(cursor->isMilestone);
        result.push_back(cursor->cblock->GetHash());
        if (cursor->cblock->GetHash() == GENESIS->GetHash()) {
            break;
        }
        cursor = GetMsVertex(cursor->cblock->GetMilestoneHash());
    }

    return result;
}

std::vector<uint256> DAGManager::TraverseMilestoneForward(const VertexPtr cursor, size_t length) const {
    std::vector<uint256> result;
    result.reserve(length);
    const auto bestChain = GetBestChain();
    std::shared_lock<std::shared_mutex> reader(bestChain->GetMilestones().get_mutex());

    size_t cursorHeight = cursor->height + 1;

    // If the cursor height is less than the least height in cache, traverse DB.
    while (cursorHeight <= STORE->GetHeadHeight() && result.size() <= length) {
        result.push_back(STORE->GetMilestoneAt(cursorHeight)->cblock->GetHash());
        ++cursorHeight;
    }

    if (!bestChain->GetMilestones().empty()) {
        uint64_t min_height = bestChain->GetMilestones().front()->height;
        uint64_t max_height = bestChain->GetMilestones().back()->height;
        // If we have reached the hightest milestone in DB and still don't get enough hashes,
        // continue to traverse the best chain cache until we reach the head or the capacity.
        while (cursorHeight <= max_height && result.size() <= length) {
            result.push_back(bestChain->GetMilestones()[cursorHeight - min_height]->GetMilestoneHash());
            ++cursorHeight;
        }
    }

    return result;
}

void DAGManager::EnableOBC() {
    time_t now      = time(nullptr);
    time_t headTime = GetMilestoneHead()->cblock->GetTime();
    if (now - headTime < obcEnableThreshold) {
        STORE->EnableOBC();
    }
}

/////////////////////////////////////
// End of synchronization methods
//

void DAGManager::AddNewBlock(ConstBlockPtr blk, PeerPtr peer) {
    verifyThread_.Execute([=, blk = std::move(blk), peer = std::move(peer)]() mutable {
        spdlog::trace("[Verify Thread] Adding blocks to pending {}", blk->GetHash().to_substr());
        if (*blk == *GENESIS) {
            spdlog::trace("[Syntax] Abort adding the genesis block.");
            return;
        }

        if (STORE->Exists(blk->GetHash())) {
            spdlog::trace("[Syntax] Abort adding existed block [{}].", std::to_string(blk->GetHash()));
            return;
        }

        /////////////////////////////////
        // Start of online verification

        if (!blk->Verify()) {
            return;
        }

        // Check solidity ///////////////
        const uint256& msHash   = blk->GetMilestoneHash();
        const uint256& prevHash = blk->GetPrevHash();
        const uint256& tipHash  = blk->GetTipHash();

        auto mask = [msHash, prevHash, tipHash]() {
            return ((!STORE->DAGExists(msHash) << 0) | (!STORE->DAGExists(prevHash) << 2) |
                    (!STORE->DAGExists(tipHash) << 1));
        };

        // First, check if we already received its preceding blocks
        if (STORE->IsWeaklySolid(blk)) {
            if (STORE->AnyLinkIsOrphan(blk)) {
                spdlog::info("[Syntax] Block is not solid (link in obc) with mask {} [{}]", mask(),
                             blk->GetHash().to_substr());
                STORE->AddBlockToOBC(std::move(blk), mask());
                return;
            }
        } else {
            // We have not received at least one of its parents.

            // Drop if the block is too old
            VertexPtr ms = GetMsVertex(msHash, false);
            if (ms && !CheckPuntuality(blk, ms)) {
                return;
            }
            // Abort and send GetBlock requests.
            spdlog::info("[Syntax] Block is not solid with mask {} [{}] prev {} tip {} ms {}", mask(),
                         std::to_string(blk->GetHash()), prevHash.to_substr(), tipHash.to_substr(), msHash.to_substr());
            STORE->AddBlockToOBC(std::move(blk), mask());

            if (peer) {
                peer->StartSync();
            }

            return;
        }

        // Check difficulty target //////

        VertexPtr ms = GetMsVertex(msHash, false);
        if (!ms) {
            spdlog::warn("[Syntax] Block has missing or invalid milestone link [{}]", blk->GetHash().to_substr());
            return;
        }

        uint32_t expectedTarget = ms->snapshot->blockTarget.GetCompact();
        if (blk->GetDifficultyTarget() != expectedTarget) {
            spdlog::warn("[Syntax] Block has unexpected change in difficulty: current {} v.s. expected {} [{}]",
                         blk->GetDifficultyTarget(), expectedTarget, blk->GetHash().to_substr());
            return;
        }

        // Check punctuality ////////////

        if (!CheckPuntuality(blk, ms)) {
            return;
        }

        // End of online verification
        /////////////////////////////////

        STORE->Cache(blk);

        if (peer) {
            PEERMAN->RelayBlock(blk, peer);
        }

        AddBlockToPending(blk);
        STORE->ReleaseBlocks(blk->GetHash());
    });
}

bool DAGManager::CheckPuntuality(const ConstBlockPtr& blk, const VertexPtr& ms) const {
    assert(ms);
    assert(!milestoneChains_.empty());
    assert(milestoneChains_.best());

    auto bestHeight = GetBestMilestoneHeight();
    if (bestHeight > ms->height && (bestHeight - ms->height) >= GetParams().punctualityThred) {
        spdlog::info("[Syntax] Block is too old: pointing to height {} vs. current head height {} [{}]", ms->height,
                     bestHeight, std::to_string(blk->GetHash()));
        return false;
    }

    return true;
}

void DAGManager::AddBlockToPending(const ConstBlockPtr& block) {
    // Extract utxos from outputs and pass their pointers to chains
    std::vector<UTXOPtr> utxos;
    const auto& txns = block->GetTransactions();
    for (size_t i = 0; i < txns.size(); ++i) {
        const auto& outs = txns[i]->GetOutputs();
        utxos.reserve(utxos.size() + outs.size());
        for (size_t j = 0; j < outs.size(); ++j) {
            utxos.emplace_back(std::make_shared<UTXO>(outs[j], i, j));
        }
    }

    // Add to pending on every chain
    for (const auto& chain : milestoneChains_) {
        chain->AddPendingBlock(block);
        if (!block->IsFirstRegistration()) {
            chain->AddPendingUTXOs(utxos);
        }
    }

    // Check if it's a new ms from the main chain
    auto const mainchain = GetBestChain();
    const auto& msHash   = block->GetMilestoneHash();
    VertexPtr msBlock    = mainchain->GetMsVertexCache(msHash);
    if (!msBlock) {
        msBlock = STORE->GetVertex(msHash);
    }

    if (msBlock) {
        auto ms = msBlock->snapshot;
        if (CheckMsPOW(block, ms)) {
            if (*msBlock->cblock == *GetMilestoneHead()->cblock) {
                // new milestone on mainchain
                spdlog::debug("[Verify Thread] Updating main chain head {} pointing to the previous MS {}",
                              block->GetHash().to_substr(), block->GetMilestoneHash().to_substr());
                ProcessMilestone(mainchain, block);
                NotifyOnChainUpdated(block, true);
                EnableOBC();
                DeleteFork();
                FlushTrigger();
            } else {
                // new fork
                spdlog::debug(
                    "[Verify Thread] A fork created with head {} pointing to the previous main chain MS {} --- "
                    "total chains {}",
                    block->GetHash().to_substr(), block->GetMilestoneHash().to_substr(), milestoneChains_.size());
                auto new_fork = std::make_shared<Chain>(*mainchain, block);
                ProcessMilestone(new_fork, block);
                bool isMainchain = milestoneChains_.emplace(std::move(new_fork));
                NotifyOnChainUpdated(block, isMainchain);
                if (isMainchain) {
                    spdlog::debug("[Verify Thread] Switched to the best chain: head from {} to {}",
                                  mainchain->GetChainHead()->GetMilestoneHash().to_substr(),
                                  GetBestChain()->GetChainHead()->GetMilestoneHash().to_substr());
                }
            }
        }
        return;
    }

    // Check if it's a milestone on any other chain
    for (auto chainIt = milestoneChains_.begin(); chainIt != milestoneChains_.end(); ++chainIt) {
        ChainPtr& chain = *chainIt;

        if (chain->IsMainChain()) {
            continue;
        }

        msBlock = chain->GetMsVertexCache(msHash);

        if (!msBlock) {
            continue;
        }

        MilestonePtr ms = msBlock->snapshot;

        if (CheckMsPOW(block, ms)) {
            bool isMainchain;
            if (msBlock->cblock->GetHash() == chain->GetChainHead()->GetMilestoneHash()) {
                // new milestone on fork
                spdlog::debug("[Verify Thread] A fork grows with head {} pointing to the previous MS {}",
                              block->GetHash().to_substr(), block->GetMilestoneHash().to_substr());
                ProcessMilestone(*chainIt, block);
                isMainchain = milestoneChains_.update_best(chainIt);
            } else {
                // new fork
                spdlog::debug("[Verify Thread] A fork created with head {} pointing to the previous forking MS {} --- "
                              "total chains {}",
                              block->GetHash().to_substr(), block->GetMilestoneHash().to_substr(),
                              milestoneChains_.size());
                auto new_fork = std::make_shared<Chain>(*chain, block);
                ProcessMilestone(new_fork, block);
                isMainchain = milestoneChains_.emplace(std::move(new_fork));
            }

            NotifyOnChainUpdated(block, isMainchain);
            if (isMainchain) {
                spdlog::debug("[Verify Thread] Switched to the best chain: head from {} to {}",
                              mainchain->GetChainHead()->GetMilestoneHash().to_substr(),
                              GetBestChain()->GetChainHead()->GetMilestoneHash().to_substr());
            }
            return;
        }
    }
}

void DAGManager::ProcessMilestone(const ChainPtr& chain, const ConstBlockPtr& block) {
    auto newMs = chain->Verify(block);
    msVertices_.emplace(block->GetHash(), newMs);
    chain->AddNewMilestone(*newMs);

    if (EraseDownloading(block->GetHash())) {
        spdlog::debug("[Verify Thread] Size of downloading = {}, removed successfully", downloading_.size());
    }
}

void DAGManager::DeleteFork() {
    auto bestchain         = GetBestChain();
    const auto& milestones = bestchain->GetMilestones();
    if (milestones.size() <= GetParams().deleteForkThreshold) {
        return;
    }
    auto targetChainWork = (*(milestones.end() - GetParams().deleteForkThreshold))->chainwork;
    for (auto chain_it = milestoneChains_.begin(); chain_it != milestoneChains_.end();) {
        if ((*chain_it)->IsMainChain()) {
            chain_it++;
            continue;
        }
        // try delete
        if ((*chain_it)->GetChainHead()->chainwork < targetChainWork) {
            for (auto it = (*chain_it)->GetMilestones().rbegin(); it != (*chain_it)->GetMilestones().rend(); ++it) {
                if (bestchain->GetMsVertexCache((*it)->GetMilestoneHash()) != nullptr) {
                    break;
                }
                msVertices_.erase((*it)->GetMilestoneHash());
            }
            spdlog::info("[Verify Thread] Deleting fork with chain head {} --- total chains {}",
                         (*chain_it)->GetChainHead()->GetMilestoneHash().to_substr(), milestoneChains_.size());
            chain_it = milestoneChains_.erase(chain_it);
        } else {
            chain_it++;
        }
    }
}

VertexPtr DAGManager::GetMsVertex(const uint256& msHash, bool withBlock) const {
    VertexPtr vtx;
    if (msVertices_.get_value(msHash, vtx)) {
        return vtx;
    }

    auto pvtx = STORE->GetVertex(msHash, withBlock);
    if (pvtx && pvtx->snapshot) {
        return pvtx;
    }

    // will happen only for finding ms of nonsolid block
    // may return nullptr when rpc is requesting some non-existing milestones
    auto ms = GetBestChain()->GetVertexCache(msHash);
    spdlog::trace("Milestone with hash {} is not found", msHash.to_substr());
    return nullptr;
}

ChainPtr DAGManager::GetBestChain() const {
    return milestoneChains_.best();
}

void DAGManager::Stop() {
    spdlog::info("Stopping DAG...");
    Wait();
    syncPool_.Stop();
    verifyThread_.Stop();
    storagePool_.Stop();
    spdlog::info("DAG stopped");
}

void DAGManager::Wait() {
    while (!verifyThread_.IsIdle() || !storagePool_.IsIdle() || !syncPool_.IsIdle()) {
        std::this_thread::yield();
    }
}

void DAGManager::FlushTrigger() {
    const auto bestChain = GetBestChain();
    if (bestChain->GetMilestones().size() <= GetParams().punctualityThred) {
        return;
    }
    std::vector<ConcurrentQueue<MilestonePtr>::const_iterator> forks;
    forks.reserve(milestoneChains_.size() - 1);
    for (auto& chain : milestoneChains_) {
        if (chain == bestChain) {
            continue;
        }
        forks.emplace_back(chain->GetMilestones().begin());
    }

    auto cursor = bestChain->GetMilestones().begin();
    for (int i = 0; i < bestChain->GetMilestones().size() - GetParams().punctualityThred &&
                    cursor != bestChain->GetMilestones().end();
         i++, cursor++) {
        if ((*cursor)->stored) {
            for (auto& fork_it : forks) {
                fork_it++;
            }
            continue;
        }

        for (auto& fork_it : forks) {
            if (*cursor != *fork_it) {
                return;
            }
            fork_it++;
        }

        FlushToSTORE(*cursor);
    }

    return;
}

void DAGManager::FlushToSTORE(MilestonePtr ms) {
    spdlog::debug("[Verify Thread] Flushing {} at height {}", ms->GetMilestoneHash().to_substr(), ms->height);

    UpdateStatOnLvsStored(ms);

    // first store data to STORE
    auto [vtxToStore, utxoToStore, utxoToRemove] = GetBestChain()->GetDataToSTORE(ms);

    ms->stored = true;
    storagePool_.Execute([=, vtxToStore = std::move(vtxToStore), utxoToStore = std::move(utxoToStore),
                          utxoToRemove = std::move(utxoToRemove)]() mutable {
        spdlog::debug("[Storage pool] Flushing {} vertices, {} utxos to store, {} utxos to remove", vtxToStore.size(),
                      utxoToStore.size(), utxoToRemove.size());

        std::vector<VertexPtr> blocksToListener;
        blocksToListener.reserve(vtxToStore.size());

        const auto& ms = *vtxToStore.back().lock();
        STORE->StoreLevelSet(vtxToStore);
        STORE->UpdatePrevRedemHashes(ms.snapshot->GetRegChange());

        for (auto& vtx : vtxToStore) {
            blocksToListener.emplace_back(vtx.lock());
            STORE->UnCache((*vtx.lock()).cblock->GetHash());
        }

        for (const auto& [utxoKey, utxoPtr] : utxoToStore) {
            STORE->AddUTXO(utxoKey, utxoPtr);
        }

        for (const auto& utxoKey : utxoToRemove) {
            STORE->RemoveUTXO(utxoKey);
        }
        STORE->SaveHeadHeight(ms.height);

        // notify the listener
        if (onLvsConfirmedCallback_) {
            onLvsConfirmedCallback_(std::move(blocksToListener), utxoToStore, utxoToRemove);
        }

        // then remove the milestone from chains
        std::vector<uint256> vtxHashes{};
        vtxHashes.reserve(vtxToStore.size());
        std::unordered_set<uint256> utxoCreated{};
        utxoCreated.reserve(utxoToStore.size());

        for (auto& vtx : vtxToStore) {
            vtxHashes.emplace_back((*vtx.lock()).cblock->GetHash());
        }

        for (const auto& [key, value] : utxoToStore) {
            utxoCreated.emplace(key);
        }

        TXOC txocToRemove{std::move(utxoCreated), std::move(utxoToRemove)};

        verifyThread_.Execute([=, msHash = ms.cblock->GetHash(), vtxHashes = std::move(vtxHashes),
                               txocToRemove = std::move(txocToRemove)]() {
            spdlog::trace("[Verify Thread] Removing level set {} cache", msHash.to_substr());
            msVertices_.erase(msHash);
            for (auto& chain : milestoneChains_) {
                chain->PopOldest(vtxHashes, txocToRemove);
            }
        });
        spdlog::trace("[Storage Pool] End of flushing {}", ms.cblock->GetHash().to_substr());
    });
}

bool CheckMsPOW(const ConstBlockPtr& b, const MilestonePtr& m) {
    return !(UintToArith256(b->GetProofHash()) > m->milestoneTarget);
}

VertexPtr DAGManager::GetMilestoneHead() const {
    auto bestchain = GetBestChain();
    if (bestchain->GetMilestones().empty()) {
        return STORE->GetMilestoneAt(STORE->GetHeadHeight());
    }

    return bestchain->GetChainHead()->GetMilestone();
}

size_t DAGManager::GetBestMilestoneHeight() const {
    auto bestchain = GetBestChain();
    if (bestchain->GetMilestones().empty()) {
        return STORE->GetHeadHeight();
    }
    return bestchain->GetChainHead()->height;
}

bool DAGManager::IsMainChainMS(const uint256& blkHash) const {
    return GetBestChain()->IsMilestone(blkHash);
}

VertexPtr DAGManager::GetMainChainVertex(const uint256& blkHash) const {
    return GetBestChain()->GetVertex(blkHash);
}

size_t DAGManager::GetHeight(const uint256& blockHash) const {
    auto vtx = GetBestChain()->GetVertexCache(blockHash);
    if (vtx) {
        return vtx->height;
    }
    return STORE->GetHeight(blockHash);
}

std::vector<ConstBlockPtr> DAGManager::GetMainChainLevelSet(size_t height) const {
    std::vector<ConstBlockPtr> lvs;
    const auto bestChain     = GetBestChain();
    size_t leastHeightCached = bestChain->GetLeastHeightCached();

    if (height < leastHeightCached) {
        lvs = STORE->GetLevelSetBlksAt(height);
    } else {
        auto vtcs = bestChain->GetMilestones()[height - leastHeightCached]->GetLevelSet();

        lvs.reserve(vtcs.size());
        for (auto& rwp : vtcs) {
            lvs.push_back((*rwp.lock()).cblock);
        }
    }

    return lvs;
}

std::vector<ConstBlockPtr> DAGManager::GetMainChainLevelSet(const uint256& blockHash) const {
    return GetMainChainLevelSet(GetHeight(blockHash));
}

std::vector<VertexPtr> DAGManager::GetLevelSet(const uint256& blockHash, bool withBlock) const {
    std::vector<VertexPtr> lvs;
    size_t leastHeightCached = GetBestChain()->GetLeastHeightCached();

    auto height = GetHeight(blockHash);
    if (height < leastHeightCached) {
        lvs = STORE->GetLevelSetVtcsAt(height, withBlock);
    } else {
        auto msVer = GetMsVertex(blockHash);
        if (msVer) {
            auto vtcs = msVer->snapshot->GetLevelSet();
            lvs.reserve(vtcs.size());
            for (auto& rwp : vtcs) {
                lvs.push_back(rwp.lock());
            }
        }
    }

    return lvs;
}

VStream DAGManager::GetMainChainRawLevelSet(size_t height) const {
    const auto bestChain     = GetBestChain();
    size_t leastHeightCached = bestChain->GetLeastHeightCached();

    // Find in DB
    if (height < leastHeightCached) {
        return STORE->GetRawLevelSetAt(height);
    }

    // Find in cache
    auto vtcs = bestChain->GetMilestones()[height - leastHeightCached]->GetLevelSet();

    // To make it have the same order as the lvs we get from file (ms goes the first)
    VStream result;
    result << vtcs.back().lock()->cblock;
    for (int i = 0; i < vtcs.size() - 1; i++) {
        result << vtcs[i].lock()->cblock;
    }

    return result;
}

VStream DAGManager::GetMainChainRawLevelSet(const uint256& blockHash) const {
    return GetMainChainRawLevelSet(GetHeight(blockHash));
}

bool DAGManager::ExistsNode(const uint256& h) const {
    for (const auto& chain : milestoneChains_) {
        if (chain->GetVertex(h)) {
            return true;
        }
    }
    return false;
}

void DAGManager::RegisterOnLvsConfirmedCallback(OnLvsConfirmedCallback&& callback_func) {
    onLvsConfirmedCallback_ = callback_func;
}

void DAGManager::RegisterOnChainUpdatedCallback(OnChainUpdatedCallback&& func) {
    onChainUpdatedCallback_ = std::move(func);
}

StatData DAGManager::GetStatData() const {
    std::shared_lock<std::shared_mutex> lk(statLock_);
    return stat_;
}

void DAGManager::UpdateStatOnLvsStored(const MilestonePtr& pms) {
    std::unique_lock<std::shared_mutex> lk(statLock_);
    stat_.nTxCnt += pms->GetNumOfValidTxns();
    stat_.nBlkCnt += pms->GetLevelSet().size();
    if (stat_.tStart == 0) {
        stat_.tStart = pms->GetLevelSet().front().lock()->cblock->GetTime();
    }
}
