// Copyright (c) 2017-2018 The Multiverse developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "forkmanager.h"
#include <boost/foreach.hpp>

using namespace std;
using namespace walleve;
using namespace multiverse;

//////////////////////////////
// CForkManagerFilter

class CForkManagerFilter : public CForkContextFilter
{
public:
    CForkManagerFilter(CForkManager* pForkManagerIn,vector<uint256>& vActiveIn) 
    : pForkManager(pForkManagerIn), vActive(vActiveIn) 
    {
    }
    bool FoundForkContext(const CForkContext& ctxt)
    {
        return pForkManager->AddNewForkContext(ctxt,vActive);
    }
protected:
    CForkManager* pForkManager;
    vector<uint256>& vActive;
}; 

//////////////////////////////
// CForkManager 

CForkManager::CForkManager()
{
    pCoreProtocol = NULL;
    pWorldLine    = NULL;
    fAllowAnyFork = false;
}

CForkManager::~CForkManager()
{
}

bool CForkManager::WalleveHandleInitialize()
{
    if (!WalleveGetObject("coreprotocol",pCoreProtocol))
    {
        WalleveLog("Failed to request coreprotocol\n");
        return false;
    }

    if (!WalleveGetObject("worldline",pWorldLine))
    {
        WalleveLog("Failed to request worldline\n");
        return false;
    }

    return true;
}

void CForkManager::WalleveHandleDeinitialize()
{
    pCoreProtocol = NULL;
    pWorldLine    = NULL;
}

bool CForkManager::WalleveHandleInvoke()
{
    boost::unique_lock<boost::shared_mutex> wlock(rwAccess);

    fAllowAnyFork = ForkConfig()->fAllowAnyFork;
    if (!fAllowAnyFork)
    {
        setForkAllowed.insert(pCoreProtocol->GetGenesisBlockHash());
        BOOST_FOREACH(const string& strFork,ForkConfig()->vFork)
        {
            uint256 hashFork(strFork);
            if (hashFork != 0)
            {
                setForkAllowed.insert(hashFork);
            }
        }
        BOOST_FOREACH(const string& strFork,ForkConfig()->vGroup)
        {
            uint256 hashFork(strFork);
            if (hashFork != 0)
            {
                setGroupAllowed.insert(hashFork);
            }
        }
    }

    return true;
}

void CForkManager::WalleveHandleHalt()
{
    setForkIndex.clear();
    mapForkSched.clear();
    setForkAllowed.clear(); 
    setGroupAllowed.clear(); 
    fAllowAnyFork = false; 
}

bool CForkManager::IsAllowed(const uint256& hashFork) const
{
    boost::shared_lock<boost::shared_mutex> rlock(rwAccess);

    map<uint256,CForkSchedule>::const_iterator it = mapForkSched.find(hashFork);
    return (it != mapForkSched.end() && (*it).second.IsAllowed());
}

bool CForkManager::GetJoint(const uint256& hashFork,uint256& hashParent,uint256& hashJoint,int& nHeight) const
{
    boost::shared_lock<boost::shared_mutex> rlock(rwAccess);

    const CForkLinkSetByFork& idxFork = setForkIndex.get<0>();
    const CForkLinkSetByFork::iterator it = idxFork.find(hashFork);
    if (it != idxFork.end())
    {
        hashParent = (*it).hashParent;
        hashJoint  = (*it).hashJoint;
        nHeight    = (*it).nJointHeight;
        return true;
    }
    return false; 
}

bool CForkManager::LoadForkContext(vector<uint256>& vActive)
{
    boost::unique_lock<boost::shared_mutex> wlock(rwAccess);

    CForkManagerFilter filter(this,vActive);
    return pWorldLine->FilterForkContext(filter);
}

void CForkManager::ForkUpdate(const CWorldLineUpdate& update,vector<uint256>& vActive,vector<uint256>& vDeactive)
{
    boost::unique_lock<boost::shared_mutex> wlock(rwAccess);

    CForkSchedule& sched = mapForkSched[update.hashFork];
    if (!sched.IsJointEmpty())
    {
        BOOST_REVERSE_FOREACH(const CBlockEx& block,update.vBlockAddNew)
        {
            if (!block.IsExtended() && !block.IsVacant())
            {
                sched.RemoveJoint(block.GetHash(),vActive);
                if (sched.IsHalted())
                {
                    vDeactive.push_back(update.hashFork);
                }
            }
        }
    }
    if (update.hashFork == pCoreProtocol->GetGenesisBlockHash())
    {
        BOOST_REVERSE_FOREACH(const CBlockEx& block,update.vBlockAddNew)
        {
            BOOST_FOREACH(const CTransaction& tx,block.vtx)
            {
                CTemplateId tid;
                if (tx.sendTo.GetTemplateId(tid) && tid.GetType() == TEMPLATE_FORK && !tx.vchData.empty())
                {
                    CForkContext ctxt;
                    if (pWorldLine->AddNewForkContext(tx,ctxt) == MV_OK)
                    {
                        AddNewForkContext(ctxt,vActive);
                    }
                }
            }
        }
    }
}

bool CForkManager::AddNewForkContext(const CForkContext& ctxt,vector<uint256>& vActive)
{
    if (IsAllowedFork(ctxt.hashFork,ctxt.hashParent))
    {
        mapForkSched.insert(make_pair(ctxt.hashFork,CForkSchedule(true)));

        if (ctxt.hashFork == pCoreProtocol->GetGenesisBlockHash())
        {
            vActive.push_back(ctxt.hashFork);
            return true;
        }

        uint256 hashParent = ctxt.hashParent;
        uint256 hashJoint  = ctxt.hashJoint;
        uint256 hashFork   = ctxt.hashFork;
        for (;;)
        {
            if (pWorldLine->Exists(hashJoint))
            {
                vActive.push_back(hashFork);
                break;
            }

            CForkSchedule& schedParent = mapForkSched[hashParent];
            if (!schedParent.IsHalted())
            {
                schedParent.AddNewJoint(hashJoint,hashFork);
                break;
            }
            
            CForkContext ctxtParent;
            if (!pWorldLine->GetForkContext(hashParent,ctxtParent))
            {
                return false;
            }
            schedParent.AddNewJoint(hashJoint,hashFork);

            hashParent = ctxtParent.hashParent;
            hashJoint  = ctxtParent.hashJoint;
            hashFork   = ctxtParent.hashFork;
        }        
    }

    setForkIndex.insert(CForkLink(ctxt));

    return true;
}

bool CForkManager::IsAllowedFork(const uint256& hashFork,const uint256& hashParent) const
{
    if (fAllowAnyFork || setForkAllowed.count(hashFork) || setGroupAllowed.count(hashFork))
    {
        return true;
    }
    if (!setGroupAllowed.empty())
    {
        if (setGroupAllowed.count(hashParent))
        {
            return true;
        }
        uint256 hash = hashParent;
        CForkContext ctxtParent;
        while (hash != 0 && pWorldLine->GetForkContext(hash,ctxtParent))
        {
            if (setGroupAllowed.count(ctxtParent.hashParent))
            {
                return true;
            }
            hash = ctxtParent.hashParent;
        } 
    }
    return false;
}
