// Copyright (c) 2017-2019 The Multiverse developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "netchn.h"
#include "schedule.h"
#include <boost/bind.hpp>

using namespace std;
using namespace walleve;
using namespace multiverse;
using boost::asio::ip::tcp;

#define PUSHTX_TIMEOUT		(1000)

//////////////////////////////
// CNetChannelPeer
void CNetChannelPeer::CNetChannelPeerFork::AddKnownTx(const vector<uint256>& vTxHash)
{
    ClearExpiredTx(vTxHash.size());
    BOOST_FOREACH(const uint256& txid,vTxHash)
    {
        setKnownTx.insert(CPeerKnownTx(txid));
    }
}

void CNetChannelPeer::CNetChannelPeerFork::ClearExpiredTx(size_t nReserved)
{
    CPeerKnownTxSetByTime& index = setKnownTx.get<1>();
    int64 nExpiredAt = GetTime() - NETCHANNEL_KNOWNINV_EXPIREDTIME;
    size_t nMaxSize = NETCHANNEL_KNOWNINV_MAXCOUNT - nReserved;
    CPeerKnownTxSetByTime::iterator it = index.begin();
    while (it != index.end() && ((*it).nTime <= nExpiredAt || index.size() > nMaxSize))
    {
        index.erase(it++);
    }
}

bool CNetChannelPeer::IsSynchronized(const uint256& hashFork) const
{
    map<uint256,CNetChannelPeerFork>::const_iterator it = mapSubscribedFork.find(hashFork);
    if (it != mapSubscribedFork.end())
    {
        return (*it).second.fSynchronized;
    }
    return false;
}

bool CNetChannelPeer::SetSyncStatus(const uint256& hashFork,bool fSync,bool& fInverted)
{
    map<uint256,CNetChannelPeerFork>::iterator it = mapSubscribedFork.find(hashFork);
    if (it != mapSubscribedFork.end())
    {
        fInverted = ((*it).second.fSynchronized != fSync);
        (*it).second.fSynchronized = fSync;
        return true;
    }
    return false;
}

void CNetChannelPeer::AddKnownTx(const uint256& hashFork,const vector<uint256>& vTxHash)
{
    map<uint256,CNetChannelPeerFork>::iterator it = mapSubscribedFork.find(hashFork);
    if (it != mapSubscribedFork.end())
    {
        (*it).second.AddKnownTx(vTxHash);
    }
}

void CNetChannelPeer::MakeTxInv(const uint256& hashFork,const vector<uint256>& vTxPool,
                                                        vector<network::CInv>& vInv,size_t nMaxCount)
{
    map<uint256,CNetChannelPeerFork>::iterator it = mapSubscribedFork.find(hashFork);
    if (it != mapSubscribedFork.end())
    {
        vector<uint256> vTxHash;
        CNetChannelPeerFork& peerFork = (*it).second;
        BOOST_FOREACH(const uint256& txid,vTxPool)
        {
            if (vInv.size() >= nMaxCount)
            {
                break;
            }
            else if (!(*it).second.IsKnownTx(txid))
            {
                vInv.push_back(network::CInv(network::CInv::MSG_TX,txid));
                vTxHash.push_back(txid);
            }
        }
        peerFork.AddKnownTx(vTxHash);
    }
}

//////////////////////////////
// CNetChannel 

CNetChannel::CNetChannel()
{
    pPeerNet = NULL;
    pCoreProtocol = NULL;
    pWorldLine = NULL;
    pTxPool = NULL;
    pService = NULL;
    pDispatcher = NULL;
}

CNetChannel::~CNetChannel()
{
}

bool CNetChannel::WalleveHandleInitialize()
{
    if (!WalleveGetObject("peernet",pPeerNet))
    {
        WalleveError("Failed to request peer net\n");
        return false;
    }

    if (!WalleveGetObject("coreprotocol",pCoreProtocol))
    {
        WalleveError("Failed to request coreprotocol\n");
        return false;
    }

    if (!WalleveGetObject("worldline",pWorldLine))
    {
        WalleveError("Failed to request worldline\n");
        return false;
    }

    if (!WalleveGetObject("txpool",pTxPool))
    {
        WalleveError("Failed to request txpool\n");
        return false;
    }

    if (!WalleveGetObject("service",pService))
    {
        WalleveError("Failed to request service\n");
        return false;
    }

    if (!WalleveGetObject("dispatcher",pDispatcher))
    {
        WalleveError("Failed to request dispatcher\n");
        return false;
    }

    return true;
}

void CNetChannel::WalleveHandleDeinitialize()
{
    pPeerNet = NULL;
    pCoreProtocol = NULL;
    pWorldLine = NULL;
    pTxPool = NULL;
    pService = NULL;
    pDispatcher = NULL;
}

bool CNetChannel::WalleveHandleInvoke()
{
    {
        boost::unique_lock<boost::mutex> lock(mtxPushTx);
        nTimerPushTx = 0;
    }
    return network::IMvNetChannel::WalleveHandleInvoke(); 
}

void CNetChannel::WalleveHandleHalt()
{
    {
        boost::unique_lock<boost::mutex> lock(mtxPushTx);
        if (nTimerPushTx != 0)
        {
            WalleveCancelTimer(nTimerPushTx);
            nTimerPushTx = 0;
        }
        setPushTxFork.clear(); 
    }
    
    network::IMvNetChannel::WalleveHandleHalt();
    {
        boost::recursive_mutex::scoped_lock scoped_lock(mtxSched);
        mapSched.clear();
    }
}

int CNetChannel::GetPrimaryChainHeight()
{
    uint256 hashBlock = uint64(0);
    int nHeight = 0;
    int64 nTime = 0;
    if (pWorldLine->GetLastBlock(pCoreProtocol->GetGenesisBlockHash(),hashBlock,nHeight,nTime))
    {
        return nHeight;
    }
    return 0;
}

bool CNetChannel::IsForkSynchronized(const uint256& hashFork) const
{
    boost::shared_lock<boost::shared_mutex> rlock(rwNetPeer);
    map<uint256, set<uint64> >::const_iterator it = mapUnsync.find(hashFork);
    return (it == mapUnsync.end() || (*it).second.empty());
}

void CNetChannel::BroadcastBlockInv(const uint256& hashFork,const uint256& hashBlock,const set<uint64>& setKnownPeer)
{
    network::CMvEventPeerInv eventInv(0,hashFork);
    eventInv.data.push_back(network::CInv(network::CInv::MSG_BLOCK,hashBlock));
    {
        boost::shared_lock<boost::shared_mutex> rlock(rwNetPeer);
        for (map<uint64,CNetChannelPeer>::iterator it = mapPeer.begin();it != mapPeer.end();++it)
        {
            uint64 nNonce = (*it).first;
            if (!setKnownPeer.count(nNonce) && (*it).second.IsSubscribed(hashFork))
            {
                eventInv.nNonce = nNonce;
                pPeerNet->DispatchEvent(&eventInv);
            }
        }
    }
}

void CNetChannel::BroadcastTxInv(const uint256& hashFork)
{
    boost::unique_lock<boost::mutex> lock(mtxPushTx);
    if (nTimerPushTx == 0)
    {
        if (!PushTxInv(hashFork))
        {
            setPushTxFork.insert(hashFork);
        }
        nTimerPushTx = WalleveSetTimer(PUSHTX_TIMEOUT,boost::bind(&CNetChannel::PushTxTimerFunc,this,_1));
    }
    else
    {
        setPushTxFork.insert(hashFork);
    }
}

void CNetChannel::SubscribeFork(const uint256& hashFork)
{
    {
        boost::recursive_mutex::scoped_lock scoped_lock(mtxSched);
        if (!mapSched.insert(make_pair(hashFork,CSchedule())).second)
        {
            return;
        }
    }

    network::CMvEventPeerSubscribe eventSubscribe(0ULL,pCoreProtocol->GetGenesisBlockHash());
    eventSubscribe.data.push_back(hashFork);

    {
        boost::shared_lock<boost::shared_mutex> rlock(rwNetPeer);    
        for (map<uint64,CNetChannelPeer>::iterator it = mapPeer.begin();it != mapPeer.end();++it)
        {
            eventSubscribe.nNonce = (*it).first;
            pPeerNet->DispatchEvent(&eventSubscribe);
            DispatchGetBlocksEvent((*it).first,hashFork);
        }
    }
}

void CNetChannel::UnsubscribeFork(const uint256& hashFork)
{
    {
        boost::recursive_mutex::scoped_lock scoped_lock(mtxSched);
        if (!mapSched.erase(hashFork))
        {
            return;
        }
    }

    network::CMvEventPeerUnsubscribe eventUnsubscribe(0ULL,pCoreProtocol->GetGenesisBlockHash());
    eventUnsubscribe.data.push_back(hashFork);

    {
        boost::shared_lock<boost::shared_mutex> rlock(rwNetPeer);    
        for (map<uint64,CNetChannelPeer>::iterator it = mapPeer.begin();it != mapPeer.end();++it)
        {
            eventUnsubscribe.nNonce = (*it).first;
            pPeerNet->DispatchEvent(&eventUnsubscribe);
        }
    }
}

bool CNetChannel::HandleEvent(network::CMvEventPeerActive& eventActive)
{
    uint64 nNonce = eventActive.nNonce;
    if ((eventActive.data.nService & network::NODE_NETWORK))
    {
        DispatchGetBlocksEvent(nNonce,pCoreProtocol->GetGenesisBlockHash());
        
        network::CMvEventPeerSubscribe eventSubscribe(nNonce,pCoreProtocol->GetGenesisBlockHash());
        {
            boost::recursive_mutex::scoped_lock scoped_lock(mtxSched);
            for (map<uint256,CSchedule>::iterator it = mapSched.begin();it != mapSched.end();++it)
            {
                if ((*it).first != pCoreProtocol->GetGenesisBlockHash())
                {
                    eventSubscribe.data.push_back((*it).first);
                }
            }
        }
        if (!eventSubscribe.data.empty())
        {
            pPeerNet->DispatchEvent(&eventSubscribe);
        }
    }
    {
        boost::unique_lock<boost::shared_mutex> wlock(rwNetPeer);
        mapPeer[nNonce] = CNetChannelPeer(eventActive.data.nService,pCoreProtocol->GetGenesisBlockHash());
        mapUnsync[pCoreProtocol->GetGenesisBlockHash()].insert(nNonce);
    }
    NotifyPeerUpdate(nNonce,true,eventActive.data);
    return true;
}

bool CNetChannel::HandleEvent(network::CMvEventPeerDeactive& eventDeactive)
{
    uint64 nNonce = eventDeactive.nNonce;
    {
        boost::recursive_mutex::scoped_lock scoped_lock(mtxSched);
        for (map<uint256,CSchedule>::iterator it = mapSched.begin();it != mapSched.end();++it)
        {
            CSchedule& sched = (*it).second;
            set<uint64> setSchedPeer;
            sched.RemovePeer(nNonce,setSchedPeer);

            BOOST_FOREACH(const uint64 nNonceSched,setSchedPeer)
            {
                SchedulePeerInv(nNonceSched,(*it).first,sched);
            }
        }
    }
    {
        boost::unique_lock<boost::shared_mutex> wlock(rwNetPeer);

        map<uint64,CNetChannelPeer>::iterator it = mapPeer.find(nNonce);
        if (it != mapPeer.end())
        {
            for (auto& subFork: (*it).second.mapSubscribedFork)
            {
                mapUnsync[subFork.first].erase(nNonce);
            }
            mapPeer.erase(nNonce);
        }
    }
    NotifyPeerUpdate(nNonce,false,eventDeactive.data);    

    return true;
}

bool CNetChannel::HandleEvent(network::CMvEventPeerSubscribe& eventSubscribe)
{
    uint64 nNonce = eventSubscribe.nNonce;
    uint256& hashFork = eventSubscribe.hashFork;
    if (hashFork == pCoreProtocol->GetGenesisBlockHash())
    {
        boost::unique_lock<boost::shared_mutex> wlock(rwNetPeer);
        map<uint64,CNetChannelPeer>::iterator it = mapPeer.find(nNonce);
        if (it != mapPeer.end())
        {
            BOOST_FOREACH(const uint256& hash,eventSubscribe.data)
            {
                (*it).second.Subscribe(hash);
                mapUnsync[hash].insert(nNonce);

                {
                    boost::recursive_mutex::scoped_lock scoped_lock(mtxSched);
                    if (mapSched.count(hash))
                    {
                        DispatchGetBlocksEvent(nNonce,hash);
                    }
                }
            }
        } 
    }
    else
    {
        DispatchMisbehaveEvent(nNonce,CEndpointManager::DDOS_ATTACK,"eventSubscribe");
    }

    return true;
}

bool CNetChannel::HandleEvent(network::CMvEventPeerUnsubscribe& eventUnsubscribe)
{
    uint64 nNonce = eventUnsubscribe.nNonce;
    uint256& hashFork = eventUnsubscribe.hashFork;
    if (hashFork == pCoreProtocol->GetGenesisBlockHash())
    {
        boost::unique_lock<boost::shared_mutex> wlock(rwNetPeer);
        map<uint64,CNetChannelPeer>::iterator it = mapPeer.find(nNonce);
        if (it != mapPeer.end())
        {
            BOOST_FOREACH(const uint256& hash,eventUnsubscribe.data)
            {
                (*it).second.Unsubscribe(hash);
                mapUnsync[hash].erase(nNonce);
            }
        } 
    }
    else
    {
        DispatchMisbehaveEvent(nNonce,CEndpointManager::DDOS_ATTACK,"eventUnsubscribe");
    }

    return true;
}

bool CNetChannel::HandleEvent(network::CMvEventPeerInv& eventInv)
{
    uint64 nNonce = eventInv.nNonce;
    uint256& hashFork = eventInv.hashFork;
    try 
    {
        if (eventInv.data.size() > network::CInv::MAX_INV_COUNT)
        {
            throw runtime_error("Inv count overflow.");
        }

        {
            boost::recursive_mutex::scoped_lock scoped_lock(mtxSched);
            CSchedule& sched = GetSchedule(hashFork);
            vector<uint256> vTxHash;
            BOOST_FOREACH(const network::CInv& inv,eventInv.data)
            {
                if ((inv.nType == network::CInv::MSG_TX && !pTxPool->Exists(inv.nHash)) 
                    || (inv.nType == network::CInv::MSG_BLOCK && !pWorldLine->Exists(inv.nHash)))
                {
                    sched.AddNewInv(inv,nNonce);
                    if (inv.nType == network::CInv::MSG_TX)
                    {
                        vTxHash.push_back(inv.nHash);
                    }
                }
            }
            if (!vTxHash.empty())
            {
                boost::unique_lock<boost::shared_mutex> wlock(rwNetPeer);
                mapPeer[nNonce].AddKnownTx(hashFork,vTxHash);
            }
            SchedulePeerInv(nNonce,hashFork,sched);
        }
    }
    catch (...)
    {
        DispatchMisbehaveEvent(nNonce,CEndpointManager::DDOS_ATTACK,"eventInv");
    }
    return true;
}

bool CNetChannel::HandleEvent(network::CMvEventPeerGetData& eventGetData)
{
    uint64 nNonce = eventGetData.nNonce;
    uint256& hashFork = eventGetData.hashFork;
    BOOST_FOREACH(const network::CInv& inv,eventGetData.data)
    {
        if (inv.nType == network::CInv::MSG_TX)
        {
            network::CMvEventPeerTx eventTx(nNonce,hashFork);
            if (pTxPool->Get(inv.nHash,eventTx.data))
            {   
                pPeerNet->DispatchEvent(&eventTx);
            }
        }
        else if (inv.nType == network::CInv::MSG_BLOCK)
        {
            network::CMvEventPeerBlock eventBlock(nNonce,hashFork);
            if (pWorldLine->GetBlock(inv.nHash,eventBlock.data))
            {
                pPeerNet->DispatchEvent(&eventBlock);
            }
        }
    }
    return true;
}

bool CNetChannel::HandleEvent(network::CMvEventPeerGetBlocks& eventGetBlocks)
{
    uint64 nNonce = eventGetBlocks.nNonce;
    uint256& hashFork = eventGetBlocks.hashFork;
    vector<uint256> vBlockHash;
    if (!pWorldLine->GetBlockInv(hashFork,eventGetBlocks.data,vBlockHash,MAX_GETBLOCKS_COUNT))
    {
        DispatchMisbehaveEvent(nNonce,CEndpointManager::DDOS_ATTACK,"eventGetBlocks");
        return true;
    }
    network::CMvEventPeerInv eventInv(nNonce,hashFork);
    BOOST_FOREACH(const uint256& hash,vBlockHash)
    {
        eventInv.data.push_back(network::CInv(network::CInv::MSG_BLOCK,hash));
    }
    pPeerNet->DispatchEvent(&eventInv);
    return true;
}

bool CNetChannel::HandleEvent(network::CMvEventPeerTx& eventTx)
{
    uint64 nNonce = eventTx.nNonce;
    uint256& hashFork = eventTx.hashFork;
    CTransaction& tx = eventTx.data;
    uint256 txid = tx.GetHash();
    
    try
    {
        boost::recursive_mutex::scoped_lock scoped_lock(mtxSched);

        set<uint64> setSchedPeer,setMisbehavePeer;
        CSchedule& sched = GetSchedule(hashFork);

        if (!sched.ReceiveTx(nNonce,txid,tx,setSchedPeer))
        {
            throw runtime_error("Failed to receive tx");
        }
        
        uint256 hashForkAnchor;
        int nHeightAnchor;
        if (pWorldLine->GetBlockLocation(tx.hashAnchor,hashForkAnchor,nHeightAnchor)
            && hashForkAnchor == hashFork)
        {
            set<uint256> setMissingPrevTx;
            if (!GetMissingPrevTx(tx,setMissingPrevTx))
            {
                AddNewTx(hashFork,txid,sched,setSchedPeer,setMisbehavePeer);
            }
            else
            {
                BOOST_FOREACH(const uint256& prev,setMissingPrevTx)
                {
                    sched.AddOrphanTxPrev(txid,prev);
                    network::CInv inv(network::CInv::MSG_TX,prev);
                    if (!sched.Exists(inv))
                    {
                        BOOST_FOREACH(const uint64 nNonceSched,setSchedPeer)
                        {
                            sched.AddNewInv(inv,nNonceSched);
                        }
                    }
                }
            }
        }
        else
        {
            sched.InvalidateTx(txid,setMisbehavePeer);
        } 
        PostAddNew(hashFork,sched,setSchedPeer,setMisbehavePeer);
    }
    catch (...)
    {
        DispatchMisbehaveEvent(nNonce,CEndpointManager::DDOS_ATTACK,"eventTx");
    }
    return true;
}

bool CNetChannel::HandleEvent(network::CMvEventPeerBlock& eventBlock)
{
    uint64 nNonce = eventBlock.nNonce;
    uint256& hashFork = eventBlock.hashFork; 
    CBlock& block = eventBlock.data;
    uint256 hash = block.GetHash();

    try
    {
        boost::recursive_mutex::scoped_lock scoped_lock(mtxSched);

        set<uint64> setSchedPeer,setMisbehavePeer;
        CSchedule& sched = GetSchedule(hashFork);
        
        if (!sched.ReceiveBlock(nNonce,hash,block,setSchedPeer))
        {
            throw runtime_error("Failed to receive block");
        }

        uint256 hashForkPrev;
        int nHeightPrev;
        if (pWorldLine->GetBlockLocation(block.hashPrev,hashForkPrev,nHeightPrev))
        {
            if (hashForkPrev == hashFork)
            {
                AddNewBlock(hashFork,hash,sched,setSchedPeer,setMisbehavePeer);
            }
            else
            {
                sched.InvalidateBlock(hash,setMisbehavePeer);
            }
        }
        else
        {
            sched.AddOrphanBlockPrev(hash,block.hashPrev);
        }

        PostAddNew(hashFork,sched,setSchedPeer,setMisbehavePeer);
    }
    catch (...)
    {
        DispatchMisbehaveEvent(nNonce,CEndpointManager::DDOS_ATTACK,"eventBlock");
    }
    return true;
}

CSchedule& CNetChannel::GetSchedule(const uint256& hashFork)
{
    map<uint256,CSchedule>::iterator it = mapSched.find(hashFork);
    if (it == mapSched.end())
    {
        throw runtime_error("Unknown fork for scheduling.");
    }
    return ((*it).second);
}

void CNetChannel::NotifyPeerUpdate(uint64 nNonce,bool fActive,const network::CAddress& addrPeer)
{
    CNetworkPeerUpdate update;
    update.nPeerNonce = nNonce;
    update.fActive = fActive;
    update.addrPeer = addrPeer;
    pService->NotifyNetworkPeerUpdate(update);
}

void CNetChannel::DispatchGetBlocksEvent(uint64 nNonce,const uint256& hashFork)
{
    network::CMvEventPeerGetBlocks eventGetBlocks(nNonce,hashFork);
    if (pWorldLine->GetBlockLocator(hashFork,eventGetBlocks.data))
    {
        pPeerNet->DispatchEvent(&eventGetBlocks);
    }
}

void CNetChannel::DispatchAwardEvent(uint64 nNonce,CEndpointManager::Bonus bonus)
{
    CWalleveEventPeerNetReward eventReward(nNonce);
    eventReward.data = bonus;
    pPeerNet->DispatchEvent(&eventReward);
}

void CNetChannel::DispatchMisbehaveEvent(uint64 nNonce,CEndpointManager::CloseReason reason,const std::string& strCaller)
{
    if (!strCaller.empty())
    {
        WalleveLog("DispatchMisbehaveEvent : %s\n",strCaller.c_str());
    }

    CWalleveEventPeerNetClose eventClose(nNonce);
    eventClose.data = reason;
    pPeerNet->DispatchEvent(&eventClose);
}

void CNetChannel::SchedulePeerInv(uint64 nNonce,const uint256& hashFork,CSchedule& sched)
{
    network::CMvEventPeerGetData eventGetData(nNonce,hashFork);
    bool fMissingPrev = false;
    bool fEmpty = true;
    if (sched.ScheduleBlockInv(nNonce,eventGetData.data,MAX_PEER_SCHED_COUNT,fMissingPrev,fEmpty))
    {
        if (fMissingPrev)
        {
            DispatchGetBlocksEvent(nNonce,hashFork);
        }
        else if (eventGetData.data.empty())
        {
            if (!sched.ScheduleTxInv(nNonce,eventGetData.data,MAX_PEER_SCHED_COUNT))
            {
                DispatchMisbehaveEvent(nNonce,CEndpointManager::DDOS_ATTACK,"SchedulePeerInv1");
            }
        }
        SetPeerSyncStatus(nNonce,hashFork,fEmpty);
    }
    else
    {
        DispatchMisbehaveEvent(nNonce,CEndpointManager::DDOS_ATTACK,"SchedulePeerInv2");
    }
    if (!eventGetData.data.empty())
    {
        pPeerNet->DispatchEvent(&eventGetData);
    }
}

bool CNetChannel::GetMissingPrevTx(CTransaction& tx,set<uint256>& setMissingPrevTx)
{
    setMissingPrevTx.clear();
    BOOST_FOREACH(const CTxIn& txin,tx.vInput)
    {
        const uint256 &prev = txin.prevout.hash;
        if (!setMissingPrevTx.count(prev))
        {
            if (!pTxPool->Exists(prev) && !pWorldLine->ExistsTx(prev))
            {
                setMissingPrevTx.insert(prev);
            }
        }
    }
    return (!setMissingPrevTx.empty());
}

void CNetChannel::AddNewBlock(const uint256& hashFork,const uint256& hash,CSchedule& sched,
                              set<uint64>& setSchedPeer,set<uint64>& setMisbehavePeer)
{
    vector<uint256> vBlockHash;
    vBlockHash.push_back(hash);
    for (size_t i = 0;i < vBlockHash.size();i++)
    {
        uint256 hashBlock = vBlockHash[i];
        uint64 nNonceSender = 0;
        CBlock* pBlock = sched.GetBlock(hashBlock,nNonceSender);
        if (pBlock != NULL)
        {
            MvErr err = pDispatcher->AddNewBlock(*pBlock,nNonceSender);
            if (err == MV_OK)
            {
                BOOST_FOREACH(const CTransaction& tx,pBlock->vtx)
                {
                    uint256 txid = tx.GetHash();
                    sched.RemoveInv(network::CInv(network::CInv::MSG_TX,txid),setSchedPeer);
                }

                set<uint64> setKnownPeer;
                sched.GetNextBlock(hashBlock,vBlockHash);
                sched.RemoveInv(network::CInv(network::CInv::MSG_BLOCK,hashBlock),setKnownPeer);
                DispatchAwardEvent(nNonceSender,CEndpointManager::VITAL_DATA);

                BroadcastBlockInv(hashFork,hashBlock,setKnownPeer); 
                setSchedPeer.insert(setKnownPeer.begin(),setKnownPeer.end());
            }
            else if (err == MV_ERR_ALREADY_HAVE && pBlock->IsVacant())
            {
                set<uint64> setKnownPeer;
                sched.GetNextBlock(hashBlock,vBlockHash);
                sched.RemoveInv(network::CInv(network::CInv::MSG_BLOCK,hashBlock),setKnownPeer);
                setSchedPeer.insert(setKnownPeer.begin(),setKnownPeer.end());
            }
            else
            {
                sched.InvalidateBlock(hashBlock,setMisbehavePeer);
            }
        }
    }
}

void CNetChannel::AddNewTx(const uint256& hashFork,const uint256& txid,CSchedule& sched,
                           set<uint64>& setSchedPeer,set<uint64>& setMisbehavePeer)
{
    set<uint256> setTx;
    vector<uint256> vtx;

    vtx.push_back(txid);
    int nAddNewTx = 0;
    for (size_t i = 0;i < vtx.size();i++)
    {
        uint256 hashTx = vtx[i];
        uint64 nNonceSender = 0;
        CTransaction *pTx = sched.GetTransaction(hashTx,nNonceSender);
        if (pTx != NULL)
        {
            MvErr err = pDispatcher->AddNewTx(*pTx,nNonceSender);
            if (err == MV_OK)
            {
                sched.GetNextTx(hashTx,vtx,setTx);
                sched.RemoveInv(network::CInv(network::CInv::MSG_TX,hashTx),setSchedPeer);
                DispatchAwardEvent(nNonceSender,CEndpointManager::MAJOR_DATA);
                nAddNewTx++;
            }
            else if (err != MV_ERR_MISSING_PREV)
            {
                sched.InvalidateTx(hashTx,setMisbehavePeer);
            }
        }
    }
    if (nAddNewTx)
    {
        BroadcastTxInv(hashFork);
    }
}

void CNetChannel::PostAddNew(const uint256& hashFork,CSchedule& sched,
                             set<uint64>& setSchedPeer,set<uint64>& setMisbehavePeer)
{
    BOOST_FOREACH(const uint64 nNonceSched,setSchedPeer)
    {
        if (!setMisbehavePeer.count(nNonceSched))
        {
            SchedulePeerInv(nNonceSched,hashFork,sched);
        }
    }

    BOOST_FOREACH(const uint64 nNonceMisbehave,setMisbehavePeer)
    {
        DispatchMisbehaveEvent(nNonceMisbehave,CEndpointManager::DDOS_ATTACK,"PostAddNew");
    }
}

void CNetChannel::SetPeerSyncStatus(uint64 nNonce,const uint256& hashFork,bool fSync)
{
    bool fInverted = false;
    {
        boost::unique_lock<boost::shared_mutex> wlock(rwNetPeer);    
        CNetChannelPeer& peer = mapPeer[nNonce];
        if (!peer.SetSyncStatus(hashFork,fSync,fInverted))
        {
            return;
        }
    }

    if (fInverted)
    {
        if (fSync)
        {
            mapUnsync[hashFork].erase(nNonce);
            BroadcastTxInv(hashFork);
        }
        else
        {
            mapUnsync[hashFork].insert(nNonce);
        }
    }
}

void CNetChannel::PushTxTimerFunc(uint32 nTimerId)
{
    boost::unique_lock<boost::mutex> lock(mtxPushTx);
    if (nTimerPushTx == nTimerId)
    {
        if (!setPushTxFork.empty())
        {
            set<uint256>::iterator it = setPushTxFork.begin();
            while (it != setPushTxFork.end())
            {
                if (PushTxInv(*it))
                {
                    setPushTxFork.erase(it++);
                }
                else
                {
                    ++it;
                }
            }
            nTimerPushTx = WalleveSetTimer(PUSHTX_TIMEOUT,boost::bind(&CNetChannel::PushTxTimerFunc,this,_1));
        }
        else
        {
            nTimerPushTx = 0;
        }
    }
}

bool CNetChannel::PushTxInv(const uint256& hashFork)
{
    // if (!IsForkSynchronized(hashFork))
    // {
    //     return false;
    // }

    bool fCompleted = true;
    vector<uint256> vTxPool;
    pTxPool->ListTx(hashFork,vTxPool);
    if (!vTxPool.empty() && !mapPeer.empty())
    {
        boost::shared_lock<boost::shared_mutex> rlock(rwNetPeer);    
        for (map<uint64,CNetChannelPeer>::iterator it = mapPeer.begin();it != mapPeer.end();++it)
        {
            CNetChannelPeer& peer = (*it).second;
            if (peer.IsSubscribed(hashFork))
            {
                network::CMvEventPeerInv eventInv((*it).first,hashFork);
                peer.MakeTxInv(hashFork,vTxPool,eventInv.data,network::CInv::MAX_INV_COUNT);
                if (!eventInv.data.empty())
                {
                    pPeerNet->DispatchEvent(&eventInv);
                    if (fCompleted && eventInv.data.size() == network::CInv::MAX_INV_COUNT)
                    {
                        fCompleted = false;
                    }
                }
            }
        }
    }
    return fCompleted;
}
