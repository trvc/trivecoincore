// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "instantx.h"
#include "key.h"
#include "validation.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "messagesigner.h"
#include "net.h"
#include "protocol.h"
#include "spork.h"
#include "sync.h"
#include "txmempool.h"
#include "util.h"
#include "consensus/validation.h"

#include <boost/algorithm/string/replace.hpp>
#include <boost/thread.hpp>

extern CWallet* pwalletMain;
extern CTxMemPool mempool;

bool fEnableDirectSend = true;
int nDirectSendDepth = DEFAULT_DIRECTSEND_DEPTH;
int nCompleteTXLocks;

CDirectSend directsend;

// Transaction Locks
//
// step 1) Some node announces intention to lock transaction inputs via "txlreg" message
// step 2) Top COutPointLock::SIGNATURES_TOTAL masternodes per each spent outpoint push "txvote" message
// step 3) Once there are COutPointLock::SIGNATURES_REQUIRED valid "txvote" messages per each spent outpoint
//         for a corresponding "txlreg" message, all outpoints from that tx are treated as locked

//
// CDirectSend
//

void CDirectSend::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if(fLiteMode) return; // disable all TriveCoin specific functionality
    if(!sporkManager.IsSporkActive(SPORK_2_DIRECTSEND_ENABLED)) return;

    // Ignore any DirectSend messages until masternode list is synced
    if(!masternodeSync.IsMasternodeListSynced()) return;

    // NOTE: NetMsgType::TXLOCKREQUEST is handled via ProcessMessage() in main.cpp

    if (strCommand == NetMsgType::TXLOCKVOTE) // DirectSend Transaction Lock Consensus Votes
    {
        if(pfrom->nVersion < MIN_DIRECTSEND_PROTO_VERSION) return;

        CTxLockVote vote;
        vRecv >> vote;

        LOCK(cs_main);
#ifdef ENABLE_WALLET
        if (pwalletMain)
            LOCK(pwalletMain->cs_wallet);
#endif
        LOCK(cs_directsend);

        uint256 nVoteHash = vote.GetHash();

        pfrom->setAskFor.erase(nVoteHash);

        if(mapTxLockVotes.count(nVoteHash)) return;
        mapTxLockVotes.insert(std::make_pair(nVoteHash, vote));

        ProcessTxLockVote(pfrom, vote, connman);

        return;
    }
}

bool CDirectSend::ProcessTxLockRequest(const CTxLockRequest& txLockRequest, CConnman& connman)
{
    LOCK2(cs_main, cs_directsend);

    uint256 txHash = txLockRequest.GetHash();

    // Check to see if we conflict with existing completed lock
    BOOST_FOREACH(const CTxIn& txin, txLockRequest.vin) {
        std::map<COutPoint, uint256>::iterator it = mapLockedOutpoints.find(txin.prevout);
        if(it != mapLockedOutpoints.end() && it->second != txLockRequest.GetHash()) {
            // Conflicting with complete lock, proceed to see if we should cancel them both
            LogPrintf("CDirectSend::ProcessTxLockRequest -- WARNING: Found conflicting completed Transaction Lock, txid=%s, completed lock txid=%s\n",
                    txLockRequest.GetHash().ToString(), it->second.ToString());
        }
    }

    // Check to see if there are votes for conflicting request,
    // if so - do not fail, just warn user
    BOOST_FOREACH(const CTxIn& txin, txLockRequest.vin) {
        std::map<COutPoint, std::set<uint256> >::iterator it = mapVotedOutpoints.find(txin.prevout);
        if(it != mapVotedOutpoints.end()) {
            BOOST_FOREACH(const uint256& hash, it->second) {
                if(hash != txLockRequest.GetHash()) {
                    LogPrint("directsend", "CDirectSend::ProcessTxLockRequest -- Double spend attempt! %s\n", txin.prevout.ToStringShort());
                    // do not fail here, let it go and see which one will get the votes to be locked
                    // TODO: notify zmq+script
                }
            }
        }
    }

    if(!CreateTxLockCandidate(txLockRequest)) {
        // smth is not right
        LogPrintf("CDirectSend::ProcessTxLockRequest -- CreateTxLockCandidate failed, txid=%s\n", txHash.ToString());
        return false;
    }
    LogPrintf("CDirectSend::ProcessTxLockRequest -- accepted, txid=%s\n", txHash.ToString());

    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    CTxLockCandidate& txLockCandidate = itLockCandidate->second;
    Vote(txLockCandidate, connman);
    ProcessOrphanTxLockVotes(connman);

    // Masternodes will sometimes propagate votes before the transaction is known to the client.
    // If this just happened - lock inputs, resolve conflicting locks, update transaction status
    // forcing external script notification.
    TryToFinalizeLockCandidate(txLockCandidate);

    return true;
}

bool CDirectSend::CreateTxLockCandidate(const CTxLockRequest& txLockRequest)
{
    if(!txLockRequest.IsValid()) return false;

    LOCK(cs_directsend);

    uint256 txHash = txLockRequest.GetHash();

    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if(itLockCandidate == mapTxLockCandidates.end()) {
        LogPrintf("CDirectSend::CreateTxLockCandidate -- new, txid=%s\n", txHash.ToString());

        CTxLockCandidate txLockCandidate(txLockRequest);
        // all inputs should already be checked by txLockRequest.IsValid() above, just use them now
        BOOST_REVERSE_FOREACH(const CTxIn& txin, txLockRequest.vin) {
            txLockCandidate.AddOutPointLock(txin.prevout);
        }
        mapTxLockCandidates.insert(std::make_pair(txHash, txLockCandidate));
    } else if (!itLockCandidate->second.txLockRequest) {
        // i.e. empty Transaction Lock Candidate was created earlier, let's update it with actual data
        itLockCandidate->second.txLockRequest = txLockRequest;
        if (itLockCandidate->second.IsTimedOut()) {
            LogPrintf("CDirectSend::CreateTxLockCandidate -- timed out, txid=%s\n", txHash.ToString());
            return false;
        }
        LogPrintf("CDirectSend::CreateTxLockCandidate -- update empty, txid=%s\n", txHash.ToString());

        // all inputs should already be checked by txLockRequest.IsValid() above, just use them now
        BOOST_REVERSE_FOREACH(const CTxIn& txin, txLockRequest.vin) {
            itLockCandidate->second.AddOutPointLock(txin.prevout);
        }
    } else {
        LogPrint("directsend", "CDirectSend::CreateTxLockCandidate -- seen, txid=%s\n", txHash.ToString());
    }

    return true;
}

void CDirectSend::CreateEmptyTxLockCandidate(const uint256& txHash)
{
    if (mapTxLockCandidates.find(txHash) != mapTxLockCandidates.end())
        return;
    LogPrintf("CDirectSend::CreateEmptyTxLockCandidate -- new, txid=%s\n", txHash.ToString());
    const CTxLockRequest txLockRequest = CTxLockRequest();
    mapTxLockCandidates.insert(std::make_pair(txHash, CTxLockCandidate(txLockRequest)));
}

void CDirectSend::Vote(CTxLockCandidate& txLockCandidate, CConnman& connman)
{
    if(!fMasterNode) return;
    if(!sporkManager.IsSporkActive(SPORK_2_DIRECTSEND_ENABLED)) return;

    LOCK2(cs_main, cs_directsend);

    uint256 txHash = txLockCandidate.GetHash();
    // check if we need to vote on this candidate's outpoints,
    // it's possible that we need to vote for several of them
    std::map<COutPoint, COutPointLock>::iterator itOutpointLock = txLockCandidate.mapOutPointLocks.begin();
    while(itOutpointLock != txLockCandidate.mapOutPointLocks.end()) {

        int nPrevoutHeight = GetUTXOHeight(itOutpointLock->first);
        if(nPrevoutHeight == -1) {
            LogPrint("directsend", "CDirectSend::Vote -- Failed to find UTXO %s\n", itOutpointLock->first.ToStringShort());
            return;
        }

        int nLockInputHeight = nPrevoutHeight + 4;

        int nRank;
        if(!mnodeman.GetMasternodeRank(activeMasternode.outpoint, nRank, nLockInputHeight, MIN_DIRECTSEND_PROTO_VERSION)) {
            LogPrint("directsend", "CDirectSend::Vote -- Can't calculate rank for masternode %s\n", activeMasternode.outpoint.ToStringShort());
            ++itOutpointLock;
            continue;
        }

        int nSignaturesTotal = COutPointLock::SIGNATURES_TOTAL;
        if(nRank > nSignaturesTotal) {
            LogPrint("directsend", "CDirectSend::Vote -- Masternode not in the top %d (%d)\n", nSignaturesTotal, nRank);
            ++itOutpointLock;
            continue;
        }

        LogPrint("directsend", "CDirectSend::Vote -- In the top %d (%d)\n", nSignaturesTotal, nRank);

        std::map<COutPoint, std::set<uint256> >::iterator itVoted = mapVotedOutpoints.find(itOutpointLock->first);

        // Check to see if we already voted for this outpoint,
        // refuse to vote twice or to include the same outpoint in another tx
        bool fAlreadyVoted = false;
        if(itVoted != mapVotedOutpoints.end()) {
            BOOST_FOREACH(const uint256& hash, itVoted->second) {
                std::map<uint256, CTxLockCandidate>::iterator it2 = mapTxLockCandidates.find(hash);
                if(it2->second.HasMasternodeVoted(itOutpointLock->first, activeMasternode.outpoint)) {
                    // we already voted for this outpoint to be included either in the same tx or in a competing one,
                    // skip it anyway
                    fAlreadyVoted = true;
                    LogPrintf("CDirectSend::Vote -- WARNING: We already voted for this outpoint, skipping: txHash=%s, outpoint=%s\n",
                            txHash.ToString(), itOutpointLock->first.ToStringShort());
                    break;
                }
            }
        }
        if(fAlreadyVoted) {
            ++itOutpointLock;
            continue; // skip to the next outpoint
        }

        // we haven't voted for this outpoint yet, let's try to do this now
        CTxLockVote vote(txHash, itOutpointLock->first, activeMasternode.outpoint);

        if(!vote.Sign()) {
            LogPrintf("CDirectSend::Vote -- Failed to sign consensus vote\n");
            return;
        }
        if(!vote.CheckSignature()) {
            LogPrintf("CDirectSend::Vote -- Signature invalid\n");
            return;
        }

        // vote constructed sucessfully, let's store and relay it
        uint256 nVoteHash = vote.GetHash();
        mapTxLockVotes.insert(std::make_pair(nVoteHash, vote));
        if(itOutpointLock->second.AddVote(vote)) {
            LogPrintf("CDirectSend::Vote -- Vote created successfully, relaying: txHash=%s, outpoint=%s, vote=%s\n",
                    txHash.ToString(), itOutpointLock->first.ToStringShort(), nVoteHash.ToString());

            if(itVoted == mapVotedOutpoints.end()) {
                std::set<uint256> setHashes;
                setHashes.insert(txHash);
                mapVotedOutpoints.insert(std::make_pair(itOutpointLock->first, setHashes));
            } else {
                mapVotedOutpoints[itOutpointLock->first].insert(txHash);
                if(mapVotedOutpoints[itOutpointLock->first].size() > 1) {
                    // it's ok to continue, just warn user
                    LogPrintf("CDirectSend::Vote -- WARNING: Vote conflicts with some existing votes: txHash=%s, outpoint=%s, vote=%s\n",
                            txHash.ToString(), itOutpointLock->first.ToStringShort(), nVoteHash.ToString());
                }
            }

            vote.Relay(connman);
        }

        ++itOutpointLock;
    }
}

//received a consensus vote
bool CDirectSend::ProcessTxLockVote(CNode* pfrom, CTxLockVote& vote, CConnman& connman)
{
    // cs_main, cs_wallet and cs_directsend should be already locked
    AssertLockHeld(cs_main);
#ifdef ENABLE_WALLET
    if (pwalletMain)
        AssertLockHeld(pwalletMain->cs_wallet);
#endif
    AssertLockHeld(cs_directsend);

    uint256 txHash = vote.GetTxHash();

    if(!vote.IsValid(pfrom, connman)) {
        // could be because of missing MN
        LogPrint("directsend", "CDirectSend::ProcessTxLockVote -- Vote is invalid, txid=%s\n", txHash.ToString());
        return false;
    }

    // relay valid vote asap
    vote.Relay(connman);

    // Masternodes will sometimes propagate votes before the transaction is known to the client,
    // will actually process only after the lock request itself has arrived

    std::map<uint256, CTxLockCandidate>::iterator it = mapTxLockCandidates.find(txHash);
    if(it == mapTxLockCandidates.end() || !it->second.txLockRequest) {
        if(!mapTxLockVotesOrphan.count(vote.GetHash())) {
            // start timeout countdown after the very first vote
            CreateEmptyTxLockCandidate(txHash);
            mapTxLockVotesOrphan[vote.GetHash()] = vote;
            LogPrint("directsend", "CDirectSend::ProcessTxLockVote -- Orphan vote: txid=%s  masternode=%s new\n",
                    txHash.ToString(), vote.GetMasternodeOutpoint().ToStringShort());
            bool fReprocess = true;
            std::map<uint256, CTxLockRequest>::iterator itLockRequest = mapLockRequestAccepted.find(txHash);
            if(itLockRequest == mapLockRequestAccepted.end()) {
                itLockRequest = mapLockRequestRejected.find(txHash);
                if(itLockRequest == mapLockRequestRejected.end()) {
                    // still too early, wait for tx lock request
                    fReprocess = false;
                }
            }
            if(fReprocess && IsEnoughOrphanVotesForTx(itLockRequest->second)) {
                // We have enough votes for corresponding lock to complete,
                // tx lock request should already be received at this stage.
                LogPrint("directsend", "CDirectSend::ProcessTxLockVote -- Found enough orphan votes, reprocessing Transaction Lock Request: txid=%s\n", txHash.ToString());
                ProcessTxLockRequest(itLockRequest->second, connman);
                return true;
            }
        } else {
            LogPrint("directsend", "CDirectSend::ProcessTxLockVote -- Orphan vote: txid=%s  masternode=%s seen\n",
                    txHash.ToString(), vote.GetMasternodeOutpoint().ToStringShort());
        }

        // This tracks those messages and allows only the same rate as of the rest of the network
        // TODO: make sure this works good enough for multi-quorum

        int nMasternodeOrphanExpireTime = GetTime() + 60*10; // keep time data for 10 minutes
        if(!mapMasternodeOrphanVotes.count(vote.GetMasternodeOutpoint())) {
            mapMasternodeOrphanVotes[vote.GetMasternodeOutpoint()] = nMasternodeOrphanExpireTime;
        } else {
            int64_t nPrevOrphanVote = mapMasternodeOrphanVotes[vote.GetMasternodeOutpoint()];
            if(nPrevOrphanVote > GetTime() && nPrevOrphanVote > GetAverageMasternodeOrphanVoteTime()) {
                LogPrint("directsend", "CDirectSend::ProcessTxLockVote -- masternode is spamming orphan Transaction Lock Votes: txid=%s  masternode=%s\n",
                        txHash.ToString(), vote.GetMasternodeOutpoint().ToStringShort());
                // Misbehaving(pfrom->id, 1);
                return false;
            }
            // not spamming, refresh
            mapMasternodeOrphanVotes[vote.GetMasternodeOutpoint()] = nMasternodeOrphanExpireTime;
        }

        return true;
    }

    CTxLockCandidate& txLockCandidate = it->second;

    if (txLockCandidate.IsTimedOut()) {
        LogPrint("directsend", "CDirectSend::ProcessTxLockVote -- too late, Transaction Lock timed out, txid=%s\n", txHash.ToString());
        return false;
    }

    LogPrint("directsend", "CDirectSend::ProcessTxLockVote -- Transaction Lock Vote, txid=%s\n", txHash.ToString());

    std::map<COutPoint, std::set<uint256> >::iterator it1 = mapVotedOutpoints.find(vote.GetOutpoint());
    if(it1 != mapVotedOutpoints.end()) {
        BOOST_FOREACH(const uint256& hash, it1->second) {
            if(hash != txHash) {
                // same outpoint was already voted to be locked by another tx lock request,
                // let's see if it was the same masternode who voted on this outpoint
                // for another tx lock request
                std::map<uint256, CTxLockCandidate>::iterator it2 = mapTxLockCandidates.find(hash);
                if(it2 !=mapTxLockCandidates.end() && it2->second.HasMasternodeVoted(vote.GetOutpoint(), vote.GetMasternodeOutpoint())) {
                    // yes, it was the same masternode
                    LogPrintf("CDirectSend::ProcessTxLockVote -- masternode sent conflicting votes! %s\n", vote.GetMasternodeOutpoint().ToStringShort());
                    // mark both Lock Candidates as attacked, none of them should complete,
                    // or at least the new (current) one shouldn't even
                    // if the second one was already completed earlier
                    txLockCandidate.MarkOutpointAsAttacked(vote.GetOutpoint());
                    it2->second.MarkOutpointAsAttacked(vote.GetOutpoint());
                    // apply maximum PoSe ban score to this masternode i.e. PoSe-ban it instantly
                    mnodeman.PoSeBan(vote.GetMasternodeOutpoint());
                    // NOTE: This vote must be relayed further to let all other nodes know about such
                    // misbehaviour of this masternode. This way they should also be able to construct
                    // conflicting lock and PoSe-ban this masternode.
                }
            }
        }
        // store all votes, regardless of them being sent by malicious masternode or not
        it1->second.insert(txHash);
    } else {
        std::set<uint256> setHashes;
        setHashes.insert(txHash);
        mapVotedOutpoints.insert(std::make_pair(vote.GetOutpoint(), setHashes));
    }

    if(!txLockCandidate.AddVote(vote)) {
        // this should never happen
        return false;
    }

    int nSignatures = txLockCandidate.CountVotes();
    int nSignaturesMax = txLockCandidate.txLockRequest.GetMaxSignatures();
    LogPrint("directsend", "CDirectSend::ProcessTxLockVote -- Transaction Lock signatures count: %d/%d, vote hash=%s\n",
            nSignatures, nSignaturesMax, vote.GetHash().ToString());

    TryToFinalizeLockCandidate(txLockCandidate);

    return true;
}

void CDirectSend::ProcessOrphanTxLockVotes(CConnman& connman)
{
    LOCK(cs_main);
#ifdef ENABLE_WALLET
    if (pwalletMain)
        LOCK(pwalletMain->cs_wallet);
#endif
    LOCK(cs_directsend);

    std::map<uint256, CTxLockVote>::iterator it = mapTxLockVotesOrphan.begin();
    while(it != mapTxLockVotesOrphan.end()) {
        if(ProcessTxLockVote(NULL, it->second, connman)) {
            mapTxLockVotesOrphan.erase(it++);
        } else {
            ++it;
        }
    }
}

bool CDirectSend::IsEnoughOrphanVotesForTx(const CTxLockRequest& txLockRequest)
{
    // There could be a situation when we already have quite a lot of votes
    // but tx lock request still wasn't received. Let's scan through
    // orphan votes to check if this is the case.
    BOOST_FOREACH(const CTxIn& txin, txLockRequest.vin) {
        if(!IsEnoughOrphanVotesForTxAndOutPoint(txLockRequest.GetHash(), txin.prevout)) {
            return false;
        }
    }
    return true;
}

bool CDirectSend::IsEnoughOrphanVotesForTxAndOutPoint(const uint256& txHash, const COutPoint& outpoint)
{
    // Scan orphan votes to check if this outpoint has enough orphan votes to be locked in some tx.
    LOCK2(cs_main, cs_directsend);
    int nCountVotes = 0;
    std::map<uint256, CTxLockVote>::iterator it = mapTxLockVotesOrphan.begin();
    while(it != mapTxLockVotesOrphan.end()) {
        if(it->second.GetTxHash() == txHash && it->second.GetOutpoint() == outpoint) {
            nCountVotes++;
            if(nCountVotes >= COutPointLock::SIGNATURES_REQUIRED) {
                return true;
            }
        }
        ++it;
    }
    return false;
}

void CDirectSend::TryToFinalizeLockCandidate(const CTxLockCandidate& txLockCandidate)
{
    if(!sporkManager.IsSporkActive(SPORK_2_DIRECTSEND_ENABLED)) return;

    LOCK(cs_main);
#ifdef ENABLE_WALLET
    if (pwalletMain)
        LOCK(pwalletMain->cs_wallet);
#endif
    LOCK(cs_directsend);

    uint256 txHash = txLockCandidate.txLockRequest.GetHash();
    if(txLockCandidate.IsAllOutPointsReady() && !IsLockedDirectSendTransaction(txHash)) {
        // we have enough votes now
        LogPrint("directsend", "CDirectSend::TryToFinalizeLockCandidate -- Transaction Lock is ready to complete, txid=%s\n", txHash.ToString());
        if(ResolveConflicts(txLockCandidate)) {
            LockTransactionInputs(txLockCandidate);
            UpdateLockedTransaction(txLockCandidate);
        }
    }
}

void CDirectSend::UpdateLockedTransaction(const CTxLockCandidate& txLockCandidate)
{
    // cs_wallet and cs_directsend should be already locked
#ifdef ENABLE_WALLET
    if (pwalletMain)
        AssertLockHeld(pwalletMain->cs_wallet);
#endif
    AssertLockHeld(cs_directsend);

    uint256 txHash = txLockCandidate.GetHash();

    if(!IsLockedDirectSendTransaction(txHash)) return; // not a locked tx, do not update/notify

#ifdef ENABLE_WALLET
    if(pwalletMain && pwalletMain->UpdatedTransaction(txHash)) {
        // bumping this to update UI
        nCompleteTXLocks++;
        // notify an external script once threshold is reached
        std::string strCmd = GetArg("-directsendnotify", "");
        if(!strCmd.empty()) {
            boost::replace_all(strCmd, "%s", txHash.GetHex());
            boost::thread t(runCommand, strCmd); // thread runs free
        }
    }
#endif

    GetMainSignals().NotifyTransactionLock(txLockCandidate.txLockRequest);

    LogPrint("directsend", "CDirectSend::UpdateLockedTransaction -- done, txid=%s\n", txHash.ToString());
}

void CDirectSend::LockTransactionInputs(const CTxLockCandidate& txLockCandidate)
{
    if(!sporkManager.IsSporkActive(SPORK_2_DIRECTSEND_ENABLED)) return;

    LOCK(cs_directsend);

    uint256 txHash = txLockCandidate.GetHash();

    if(!txLockCandidate.IsAllOutPointsReady()) return;

    std::map<COutPoint, COutPointLock>::const_iterator it = txLockCandidate.mapOutPointLocks.begin();

    while(it != txLockCandidate.mapOutPointLocks.end()) {
        mapLockedOutpoints.insert(std::make_pair(it->first, txHash));
        ++it;
    }
    LogPrint("directsend", "CDirectSend::LockTransactionInputs -- done, txid=%s\n", txHash.ToString());
}

bool CDirectSend::GetLockedOutPointTxHash(const COutPoint& outpoint, uint256& hashRet)
{
    LOCK(cs_directsend);
    std::map<COutPoint, uint256>::iterator it = mapLockedOutpoints.find(outpoint);
    if(it == mapLockedOutpoints.end()) return false;
    hashRet = it->second;
    return true;
}

bool CDirectSend::ResolveConflicts(const CTxLockCandidate& txLockCandidate)
{
    LOCK2(cs_main, cs_directsend);

    uint256 txHash = txLockCandidate.GetHash();

    // make sure the lock is ready
    if(!txLockCandidate.IsAllOutPointsReady()) return false;

    LOCK(mempool.cs); // protect mempool.mapNextTx

    BOOST_FOREACH(const CTxIn& txin, txLockCandidate.txLockRequest.vin) {
        uint256 hashConflicting;
        if(GetLockedOutPointTxHash(txin.prevout, hashConflicting) && txHash != hashConflicting) {
            // completed lock which conflicts with another completed one?
            // this means that majority of MNs in the quorum for this specific tx input are malicious!
            std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
            std::map<uint256, CTxLockCandidate>::iterator itLockCandidateConflicting = mapTxLockCandidates.find(hashConflicting);
            if(itLockCandidate == mapTxLockCandidates.end() || itLockCandidateConflicting == mapTxLockCandidates.end()) {
                // safety check, should never really happen
                LogPrintf("CDirectSend::ResolveConflicts -- ERROR: Found conflicting completed Transaction Lock, but one of txLockCandidate-s is missing, txid=%s, conflicting txid=%s\n",
                        txHash.ToString(), hashConflicting.ToString());
                return false;
            }
            LogPrintf("CDirectSend::ResolveConflicts -- WARNING: Found conflicting completed Transaction Lock, dropping both, txid=%s, conflicting txid=%s\n",
                    txHash.ToString(), hashConflicting.ToString());
            CTxLockRequest txLockRequest = itLockCandidate->second.txLockRequest;
            CTxLockRequest txLockRequestConflicting = itLockCandidateConflicting->second.txLockRequest;
            itLockCandidate->second.SetConfirmedHeight(0); // expired
            itLockCandidateConflicting->second.SetConfirmedHeight(0); // expired
            CheckAndRemove(); // clean up
            // AlreadyHave should still return "true" for both of them
            mapLockRequestRejected.insert(make_pair(txHash, txLockRequest));
            mapLockRequestRejected.insert(make_pair(hashConflicting, txLockRequestConflicting));

            // TODO: clean up mapLockRequestRejected later somehow
            //       (not a big issue since we already PoSe ban malicious masternodes
            //        and they won't be able to spam)
            // TODO: ban all malicious masternodes permanently, do not accept anything from them, ever

            // TODO: notify zmq+script about this double-spend attempt
            //       and let merchant cancel/hold the order if it's not too late...

            // can't do anything else, fallback to regular txes
            return false;
        } else if (mempool.mapNextTx.count(txin.prevout)) {
            // check if it's in mempool
            hashConflicting = mempool.mapNextTx[txin.prevout].ptx->GetHash();
            if(txHash == hashConflicting) continue; // matches current, not a conflict, skip to next txin
            // conflicts with tx in mempool
            LogPrintf("CDirectSend::ResolveConflicts -- ERROR: Failed to complete Transaction Lock, conflicts with mempool, txid=%s\n", txHash.ToString());
            return false;
        }
    } // FOREACH
    // No conflicts were found so far, check to see if it was already included in block
    CTransaction txTmp;
    uint256 hashBlock;
    if(GetTransaction(txHash, txTmp, Params().GetConsensus(), hashBlock, true) && hashBlock != uint256()) {
        LogPrint("directsend", "CDirectSend::ResolveConflicts -- Done, %s is included in block %s\n", txHash.ToString(), hashBlock.ToString());
        return true;
    }
    // Not in block yet, make sure all its inputs are still unspent
    BOOST_FOREACH(const CTxIn& txin, txLockCandidate.txLockRequest.vin) {
        CCoins coins;
        if(!GetUTXOCoins(txin.prevout, coins)) {
            // Not in UTXO anymore? A conflicting tx was mined while we were waiting for votes.
            LogPrintf("CDirectSend::ResolveConflicts -- ERROR: Failed to find UTXO %s, can't complete Transaction Lock\n", txin.prevout.ToStringShort());
            return false;
        }
    }
    LogPrint("directsend", "CDirectSend::ResolveConflicts -- Done, txid=%s\n", txHash.ToString());

    return true;
}

int64_t CDirectSend::GetAverageMasternodeOrphanVoteTime()
{
    LOCK(cs_directsend);
    // NOTE: should never actually call this function when mapMasternodeOrphanVotes is empty
    if(mapMasternodeOrphanVotes.empty()) return 0;

    std::map<COutPoint, int64_t>::iterator it = mapMasternodeOrphanVotes.begin();
    int64_t total = 0;

    while(it != mapMasternodeOrphanVotes.end()) {
        total+= it->second;
        ++it;
    }

    return total / mapMasternodeOrphanVotes.size();
}

void CDirectSend::CheckAndRemove()
{
    if(!masternodeSync.IsMasternodeListSynced()) return;

    LOCK(cs_directsend);

    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.begin();

    // remove expired candidates
    while(itLockCandidate != mapTxLockCandidates.end()) {
        CTxLockCandidate &txLockCandidate = itLockCandidate->second;
        uint256 txHash = txLockCandidate.GetHash();
        if(txLockCandidate.IsExpired(nCachedBlockHeight)) {
            LogPrintf("CDirectSend::CheckAndRemove -- Removing expired Transaction Lock Candidate: txid=%s\n", txHash.ToString());
            std::map<COutPoint, COutPointLock>::iterator itOutpointLock = txLockCandidate.mapOutPointLocks.begin();
            while(itOutpointLock != txLockCandidate.mapOutPointLocks.end()) {
                mapLockedOutpoints.erase(itOutpointLock->first);
                mapVotedOutpoints.erase(itOutpointLock->first);
                ++itOutpointLock;
            }
            mapLockRequestAccepted.erase(txHash);
            mapLockRequestRejected.erase(txHash);
            mapTxLockCandidates.erase(itLockCandidate++);
        } else {
            ++itLockCandidate;
        }
    }

    // remove expired votes
    std::map<uint256, CTxLockVote>::iterator itVote = mapTxLockVotes.begin();
    while(itVote != mapTxLockVotes.end()) {
        if(itVote->second.IsExpired(nCachedBlockHeight)) {
            LogPrint("directsend", "CDirectSend::CheckAndRemove -- Removing expired vote: txid=%s  masternode=%s\n",
                    itVote->second.GetTxHash().ToString(), itVote->second.GetMasternodeOutpoint().ToStringShort());
            mapTxLockVotes.erase(itVote++);
        } else {
            ++itVote;
        }
    }

    // remove timed out orphan votes
    std::map<uint256, CTxLockVote>::iterator itOrphanVote = mapTxLockVotesOrphan.begin();
    while(itOrphanVote != mapTxLockVotesOrphan.end()) {
        if(itOrphanVote->second.IsTimedOut()) {
            LogPrint("directsend", "CDirectSend::CheckAndRemove -- Removing timed out orphan vote: txid=%s  masternode=%s\n",
                    itOrphanVote->second.GetTxHash().ToString(), itOrphanVote->second.GetMasternodeOutpoint().ToStringShort());
            mapTxLockVotes.erase(itOrphanVote->first);
            mapTxLockVotesOrphan.erase(itOrphanVote++);
        } else {
            ++itOrphanVote;
        }
    }

    // remove invalid votes and votes for failed lock attempts
    itVote = mapTxLockVotes.begin();
    while(itVote != mapTxLockVotes.end()) {
        if(itVote->second.IsFailed()) {
            LogPrint("directsend", "CDirectSend::CheckAndRemove -- Removing vote for failed lock attempt: txid=%s  masternode=%s\n",
                    itVote->second.GetTxHash().ToString(), itVote->second.GetMasternodeOutpoint().ToStringShort());
            mapTxLockVotes.erase(itVote++);
        } else {
            ++itVote;
        }
    }

    // remove timed out masternode orphan votes (DOS protection)
    std::map<COutPoint, int64_t>::iterator itMasternodeOrphan = mapMasternodeOrphanVotes.begin();
    while(itMasternodeOrphan != mapMasternodeOrphanVotes.end()) {
        if(itMasternodeOrphan->second < GetTime()) {
            LogPrint("directsend", "CDirectSend::CheckAndRemove -- Removing timed out orphan masternode vote: masternode=%s\n",
                    itMasternodeOrphan->first.ToStringShort());
            mapMasternodeOrphanVotes.erase(itMasternodeOrphan++);
        } else {
            ++itMasternodeOrphan;
        }
    }
    LogPrintf("CDirectSend::CheckAndRemove -- %s\n", ToString());
}

bool CDirectSend::AlreadyHave(const uint256& hash)
{
    LOCK(cs_directsend);
    return mapLockRequestAccepted.count(hash) ||
            mapLockRequestRejected.count(hash) ||
            mapTxLockVotes.count(hash);
}

void CDirectSend::AcceptLockRequest(const CTxLockRequest& txLockRequest)
{
    LOCK(cs_directsend);
    mapLockRequestAccepted.insert(make_pair(txLockRequest.GetHash(), txLockRequest));
}

void CDirectSend::RejectLockRequest(const CTxLockRequest& txLockRequest)
{
    LOCK(cs_directsend);
    mapLockRequestRejected.insert(make_pair(txLockRequest.GetHash(), txLockRequest));
}

bool CDirectSend::HasTxLockRequest(const uint256& txHash)
{
    CTxLockRequest txLockRequestTmp;
    return GetTxLockRequest(txHash, txLockRequestTmp);
}

bool CDirectSend::GetTxLockRequest(const uint256& txHash, CTxLockRequest& txLockRequestRet)
{
    LOCK(cs_directsend);

    std::map<uint256, CTxLockCandidate>::iterator it = mapTxLockCandidates.find(txHash);
    if(it == mapTxLockCandidates.end()) return false;
    txLockRequestRet = it->second.txLockRequest;

    return true;
}

bool CDirectSend::GetTxLockVote(const uint256& hash, CTxLockVote& txLockVoteRet)
{
    LOCK(cs_directsend);

    std::map<uint256, CTxLockVote>::iterator it = mapTxLockVotes.find(hash);
    if(it == mapTxLockVotes.end()) return false;
    txLockVoteRet = it->second;

    return true;
}

bool CDirectSend::IsDirectSendReadyToLock(const uint256& txHash)
{
    if(!fEnableDirectSend || fLargeWorkForkFound || fLargeWorkInvalidChainFound ||
        !sporkManager.IsSporkActive(SPORK_2_DIRECTSEND_ENABLED)) return false;

    LOCK(cs_directsend);
    // There must be a successfully verified lock request
    // and all outputs must be locked (i.e. have enough signatures)
    std::map<uint256, CTxLockCandidate>::iterator it = mapTxLockCandidates.find(txHash);
    return it != mapTxLockCandidates.end() && it->second.IsAllOutPointsReady();
}

bool CDirectSend::IsLockedDirectSendTransaction(const uint256& txHash)
{
    if(!fEnableDirectSend || fLargeWorkForkFound || fLargeWorkInvalidChainFound ||
        !sporkManager.IsSporkActive(SPORK_3_DIRECTSEND_BLOCK_FILTERING)) return false;

    LOCK(cs_directsend);

    // there must be a lock candidate
    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if(itLockCandidate == mapTxLockCandidates.end()) return false;

    // which should have outpoints
    if(itLockCandidate->second.mapOutPointLocks.empty()) return false;

    // and all of these outputs must be included in mapLockedOutpoints with correct hash
    std::map<COutPoint, COutPointLock>::iterator itOutpointLock = itLockCandidate->second.mapOutPointLocks.begin();
    while(itOutpointLock != itLockCandidate->second.mapOutPointLocks.end()) {
        uint256 hashLocked;
        if(!GetLockedOutPointTxHash(itOutpointLock->first, hashLocked) || hashLocked != txHash) return false;
        ++itOutpointLock;
    }

    return true;
}

int CDirectSend::GetTransactionLockSignatures(const uint256& txHash)
{
    if(!fEnableDirectSend) return -1;
    if(fLargeWorkForkFound || fLargeWorkInvalidChainFound) return -2;
    if(!sporkManager.IsSporkActive(SPORK_2_DIRECTSEND_ENABLED)) return -3;

    LOCK(cs_directsend);

    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if(itLockCandidate != mapTxLockCandidates.end()) {
        return itLockCandidate->second.CountVotes();
    }

    return -1;
}

int CDirectSend::GetConfirmations(const uint256 &nTXHash)
{
    return IsLockedDirectSendTransaction(nTXHash) ? nDirectSendDepth : 0;
}

bool CDirectSend::IsTxLockCandidateTimedOut(const uint256& txHash)
{
    if(!fEnableDirectSend) return false;

    LOCK(cs_directsend);

    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if (itLockCandidate != mapTxLockCandidates.end()) {
        return !itLockCandidate->second.IsAllOutPointsReady() &&
                itLockCandidate->second.IsTimedOut();
    }

    return false;
}

void CDirectSend::Relay(const uint256& txHash, CConnman& connman)
{
    LOCK(cs_directsend);

    std::map<uint256, CTxLockCandidate>::const_iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if (itLockCandidate != mapTxLockCandidates.end()) {
        itLockCandidate->second.Relay(connman);
    }
}

void CDirectSend::UpdatedBlockTip(const CBlockIndex *pindex)
{
    nCachedBlockHeight = pindex->nHeight;
}

void CDirectSend::SyncTransaction(const CTransaction& tx, const CBlock* pblock)
{
    // Update lock candidates and votes if corresponding tx confirmed
    // or went from confirmed to 0-confirmed or conflicted.

    if (tx.IsCoinBase()) return;

    LOCK2(cs_main, cs_directsend);

    uint256 txHash = tx.GetHash();

    // When tx is 0-confirmed or conflicted, pblock is NULL and nHeightNew should be set to -1
    CBlockIndex* pblockindex = NULL;
    if(pblock) {
        uint256 blockHash = pblock->GetHash();
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if(mi == mapBlockIndex.end() || !mi->second) {
            // shouldn't happen
            LogPrint("directsend", "CTxLockRequest::SyncTransaction -- Failed to find block %s\n", blockHash.ToString());
            return;
        }
        pblockindex = mi->second;
    }
    int nHeightNew = pblockindex ? pblockindex->nHeight : -1;

    LogPrint("directsend", "CDirectSend::SyncTransaction -- txid=%s nHeightNew=%d\n", txHash.ToString(), nHeightNew);

    // Check lock candidates
    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if(itLockCandidate != mapTxLockCandidates.end()) {
        LogPrint("directsend", "CDirectSend::SyncTransaction -- txid=%s nHeightNew=%d lock candidate updated\n",
                txHash.ToString(), nHeightNew);
        itLockCandidate->second.SetConfirmedHeight(nHeightNew);
        // Loop through outpoint locks
        std::map<COutPoint, COutPointLock>::iterator itOutpointLock = itLockCandidate->second.mapOutPointLocks.begin();
        while(itOutpointLock != itLockCandidate->second.mapOutPointLocks.end()) {
            // Check corresponding lock votes
            std::vector<CTxLockVote> vVotes = itOutpointLock->second.GetVotes();
            std::vector<CTxLockVote>::iterator itVote = vVotes.begin();
            std::map<uint256, CTxLockVote>::iterator it;
            while(itVote != vVotes.end()) {
                uint256 nVoteHash = itVote->GetHash();
                LogPrint("directsend", "CDirectSend::SyncTransaction -- txid=%s nHeightNew=%d vote %s updated\n",
                        txHash.ToString(), nHeightNew, nVoteHash.ToString());
                it = mapTxLockVotes.find(nVoteHash);
                if(it != mapTxLockVotes.end()) {
                    it->second.SetConfirmedHeight(nHeightNew);
                }
                ++itVote;
            }
            ++itOutpointLock;
        }
    }

    // check orphan votes
    std::map<uint256, CTxLockVote>::iterator itOrphanVote = mapTxLockVotesOrphan.begin();
    while(itOrphanVote != mapTxLockVotesOrphan.end()) {
        if(itOrphanVote->second.GetTxHash() == txHash) {
            LogPrint("directsend", "CDirectSend::SyncTransaction -- txid=%s nHeightNew=%d vote %s updated\n",
                    txHash.ToString(), nHeightNew, itOrphanVote->first.ToString());
            mapTxLockVotes[itOrphanVote->first].SetConfirmedHeight(nHeightNew);
        }
        ++itOrphanVote;
    }
}

std::string CDirectSend::ToString()
{
    LOCK(cs_directsend);
    return strprintf("Lock Candidates: %llu, Votes %llu", mapTxLockCandidates.size(), mapTxLockVotes.size());
}

//
// CTxLockRequest
//

bool CTxLockRequest::IsValid() const
{
    if(vout.size() < 1) return false;

    if(vin.size() > WARN_MANY_INPUTS) {
        LogPrint("directsend", "CTxLockRequest::IsValid -- WARNING: Too many inputs: tx=%s", ToString());
    }

    LOCK(cs_main);
    if(!CheckFinalTx(*this)) {
        LogPrint("directsend", "CTxLockRequest::IsValid -- Transaction is not final: tx=%s", ToString());
        return false;
    }

    CAmount nValueIn = 0;
    CAmount nValueOut = 0;

    BOOST_FOREACH(const CTxOut& txout, vout) {
        // DirectSend supports normal scripts and unspendable (i.e. data) scripts.
        // TODO: Look into other script types that are normal and can be included
        if(!txout.scriptPubKey.IsNormalPaymentScript() && !txout.scriptPubKey.IsUnspendable()) {
            LogPrint("directsend", "CTxLockRequest::IsValid -- Invalid Script %s", ToString());
            return false;
        }
        nValueOut += txout.nValue;
    }

    BOOST_FOREACH(const CTxIn& txin, vin) {

        CCoins coins;

        if(!GetUTXOCoins(txin.prevout, coins)) {
            LogPrint("directsend", "CTxLockRequest::IsValid -- Failed to find UTXO %s\n", txin.prevout.ToStringShort());
            return false;
        }

        int nTxAge = chainActive.Height() - coins.nHeight + 1;
        // 1 less than the "send IX" gui requires, in case of a block propagating the network at the time
        int nConfirmationsRequired = DIRECTSEND_CONFIRMATIONS_REQUIRED - 1;

        if(nTxAge < nConfirmationsRequired) {
            LogPrint("directsend", "CTxLockRequest::IsValid -- outpoint %s too new: nTxAge=%d, nConfirmationsRequired=%d, txid=%s\n",
                    txin.prevout.ToStringShort(), nTxAge, nConfirmationsRequired, GetHash().ToString());
            return false;
        }

        nValueIn += coins.vout[txin.prevout.n].nValue;
    }

    if(nValueIn > sporkManager.GetSporkValue(SPORK_5_DIRECTSEND_MAX_VALUE)*COIN) {
        LogPrint("directsend", "CTxLockRequest::IsValid -- Transaction value too high: nValueIn=%d, tx=%s", nValueIn, ToString());
        return false;
    }

    if(nValueIn - nValueOut < GetMinFee()) {
        LogPrint("directsend", "CTxLockRequest::IsValid -- did not include enough fees in transaction: fees=%d, tx=%s", nValueOut - nValueIn, ToString());
        return false;
    }

    return true;
}

CAmount CTxLockRequest::GetMinFee() const
{
    CAmount nMinFee = fDIP0001ActiveAtTip ? MIN_FEE / 10 : MIN_FEE;
    return std::max(nMinFee, CAmount(vin.size() * nMinFee));
}

int CTxLockRequest::GetMaxSignatures() const
{
    return vin.size() * COutPointLock::SIGNATURES_TOTAL;
}

//
// CTxLockVote
//

bool CTxLockVote::IsValid(CNode* pnode, CConnman& connman) const
{
    if(!mnodeman.Has(outpointMasternode)) {
        LogPrint("directsend", "CTxLockVote::IsValid -- Unknown masternode %s\n", outpointMasternode.ToStringShort());
        mnodeman.AskForMN(pnode, outpointMasternode, connman);
        return false;
    }

    CCoins coins;
    if(!GetUTXOCoins(outpoint, coins)) {
        LogPrint("directsend", "CTxLockVote::IsValid -- Failed to find UTXO %s\n", outpoint.ToStringShort());
        return false;
    }

    int nLockInputHeight = coins.nHeight + 4;

    int nRank;
    if(!mnodeman.GetMasternodeRank(outpointMasternode, nRank, nLockInputHeight, MIN_DIRECTSEND_PROTO_VERSION)) {
        //can be caused by past versions trying to vote with an invalid protocol
        LogPrint("directsend", "CTxLockVote::IsValid -- Can't calculate rank for masternode %s\n", outpointMasternode.ToStringShort());
        return false;
    }
    LogPrint("directsend", "CTxLockVote::IsValid -- Masternode %s, rank=%d\n", outpointMasternode.ToStringShort(), nRank);

    int nSignaturesTotal = COutPointLock::SIGNATURES_TOTAL;
    if(nRank > nSignaturesTotal) {
        LogPrint("directsend", "CTxLockVote::IsValid -- Masternode %s is not in the top %d (%d), vote hash=%s\n",
                outpointMasternode.ToStringShort(), nSignaturesTotal, nRank, GetHash().ToString());
        return false;
    }

    if(!CheckSignature()) {
        LogPrintf("CTxLockVote::IsValid -- Signature invalid\n");
        return false;
    }

    return true;
}

uint256 CTxLockVote::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << txHash;
    ss << outpoint;
    ss << outpointMasternode;
    return ss.GetHash();
}

bool CTxLockVote::CheckSignature() const
{
    std::string strError;
    std::string strMessage = txHash.ToString() + outpoint.ToStringShort();

    masternode_info_t infoMn;

    if(!mnodeman.GetMasternodeInfo(outpointMasternode, infoMn)) {
        LogPrintf("CTxLockVote::CheckSignature -- Unknown Masternode: masternode=%s\n", outpointMasternode.ToString());
        return false;
    }

    if(!CMessageSigner::VerifyMessage(infoMn.pubKeyMasternode, vchMasternodeSignature, strMessage, strError)) {
        LogPrintf("CTxLockVote::CheckSignature -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CTxLockVote::Sign()
{
    std::string strError;
    std::string strMessage = txHash.ToString() + outpoint.ToStringShort();

    if(!CMessageSigner::SignMessage(strMessage, vchMasternodeSignature, activeMasternode.keyMasternode)) {
        LogPrintf("CTxLockVote::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!CMessageSigner::VerifyMessage(activeMasternode.pubKeyMasternode, vchMasternodeSignature, strMessage, strError)) {
        LogPrintf("CTxLockVote::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

void CTxLockVote::Relay(CConnman& connman) const
{
    CInv inv(MSG_TXLOCK_VOTE, GetHash());
    connman.RelayInv(inv);
}

bool CTxLockVote::IsExpired(int nHeight) const
{
    // Locks and votes expire nDirectSendKeepLock blocks after the block corresponding tx was included into.
    return (nConfirmedHeight != -1) && (nHeight - nConfirmedHeight > Params().GetConsensus().nDirectSendKeepLock);
}

bool CTxLockVote::IsTimedOut() const
{
    return GetTime() - nTimeCreated > DIRECTSEND_LOCK_TIMEOUT_SECONDS;
}

bool CTxLockVote::IsFailed() const
{
    return (GetTime() - nTimeCreated > DIRECTSEND_FAILED_TIMEOUT_SECONDS) && !directsend.IsLockedDirectSendTransaction(GetTxHash());
}

//
// COutPointLock
//

bool COutPointLock::AddVote(const CTxLockVote& vote)
{
    if(mapMasternodeVotes.count(vote.GetMasternodeOutpoint()))
        return false;
    mapMasternodeVotes.insert(std::make_pair(vote.GetMasternodeOutpoint(), vote));
    return true;
}

std::vector<CTxLockVote> COutPointLock::GetVotes() const
{
    std::vector<CTxLockVote> vRet;
    std::map<COutPoint, CTxLockVote>::const_iterator itVote = mapMasternodeVotes.begin();
    while(itVote != mapMasternodeVotes.end()) {
        vRet.push_back(itVote->second);
        ++itVote;
    }
    return vRet;
}

bool COutPointLock::HasMasternodeVoted(const COutPoint& outpointMasternodeIn) const
{
    return mapMasternodeVotes.count(outpointMasternodeIn);
}

void COutPointLock::Relay(CConnman& connman) const
{
    std::map<COutPoint, CTxLockVote>::const_iterator itVote = mapMasternodeVotes.begin();
    while(itVote != mapMasternodeVotes.end()) {
        itVote->second.Relay(connman);
        ++itVote;
    }
}

//
// CTxLockCandidate
//

void CTxLockCandidate::AddOutPointLock(const COutPoint& outpoint)
{
    mapOutPointLocks.insert(make_pair(outpoint, COutPointLock(outpoint)));
}

void CTxLockCandidate::MarkOutpointAsAttacked(const COutPoint& outpoint)
{
    std::map<COutPoint, COutPointLock>::iterator it = mapOutPointLocks.find(outpoint);
    if(it != mapOutPointLocks.end())
        it->second.MarkAsAttacked();
}

bool CTxLockCandidate::AddVote(const CTxLockVote& vote)
{
    std::map<COutPoint, COutPointLock>::iterator it = mapOutPointLocks.find(vote.GetOutpoint());
    if(it == mapOutPointLocks.end()) return false;
    return it->second.AddVote(vote);
}

bool CTxLockCandidate::IsAllOutPointsReady() const
{
    if(mapOutPointLocks.empty()) return false;

    std::map<COutPoint, COutPointLock>::const_iterator it = mapOutPointLocks.begin();
    while(it != mapOutPointLocks.end()) {
        if(!it->second.IsReady()) return false;
        ++it;
    }
    return true;
}

bool CTxLockCandidate::HasMasternodeVoted(const COutPoint& outpointIn, const COutPoint& outpointMasternodeIn)
{
    std::map<COutPoint, COutPointLock>::iterator it = mapOutPointLocks.find(outpointIn);
    return it !=mapOutPointLocks.end() && it->second.HasMasternodeVoted(outpointMasternodeIn);
}

int CTxLockCandidate::CountVotes() const
{
    // Note: do NOT use vote count to figure out if tx is locked, use IsAllOutPointsReady() instead
    int nCountVotes = 0;
    std::map<COutPoint, COutPointLock>::const_iterator it = mapOutPointLocks.begin();
    while(it != mapOutPointLocks.end()) {
        nCountVotes += it->second.CountVotes();
        ++it;
    }
    return nCountVotes;
}

bool CTxLockCandidate::IsExpired(int nHeight) const
{
    // Locks and votes expire nDirectSendKeepLock blocks after the block corresponding tx was included into.
    return (nConfirmedHeight != -1) && (nHeight - nConfirmedHeight > Params().GetConsensus().nDirectSendKeepLock);
}

bool CTxLockCandidate::IsTimedOut() const
{
    return GetTime() - nTimeCreated > DIRECTSEND_LOCK_TIMEOUT_SECONDS;
}

void CTxLockCandidate::Relay(CConnman& connman) const
{
    connman.RelayTransaction(txLockRequest);
    std::map<COutPoint, COutPointLock>::const_iterator itOutpointLock = mapOutPointLocks.begin();
    while(itOutpointLock != mapOutPointLocks.end()) {
        itOutpointLock->second.Relay(connman);
        ++itOutpointLock;
    }
}
