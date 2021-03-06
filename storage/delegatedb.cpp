// Copyright (c) 2017-2019 The Multiverse developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
    
#include "delegatedb.h"
#include "leveldbeng.h"

#include <boost/foreach.hpp>

using namespace std;
using namespace walleve;
using namespace multiverse::storage;
    
//////////////////////////////
// CDelegateDB

bool CDelegateDB::Initialize(const boost::filesystem::path& pathData)
{
    CLevelDBArguments args;
    args.path = (pathData / "delegate").string();
    args.syncwrite = false;
    CLevelDBEngine *engine = new CLevelDBEngine(args);

    if (!Open(engine))
    {
        delete engine;
        return false;
    }

    return true;
}

void CDelegateDB::Deinitialize()
{
    cacheDelegate.Clear();
    Close();
}

bool CDelegateDB::AddNew(const uint256& hashBlock,const CDelegateContext& ctxtDelegate)
{
    if (!Write(hashBlock,ctxtDelegate))
    {
        return false;
    }

    cacheDelegate.AddNew(hashBlock,ctxtDelegate);
    return true; 
}

bool CDelegateDB::Remove(const uint256& hashBlock)
{
    cacheDelegate.Remove(hashBlock);
    return Erase(hashBlock);
}

bool CDelegateDB::Retrieve(const uint256& hashBlock,CDelegateContext& ctxtDelegate)
{
    if (cacheDelegate.Retrieve(hashBlock,ctxtDelegate))
    {
        return true;
    }
    
    if (!Read(hashBlock,ctxtDelegate))
    {
        return false;
    }

    cacheDelegate.AddNew(hashBlock,ctxtDelegate);
    return true;
}

bool CDelegateDB::RetrieveDelegatedVote(const uint256& hashBlock,map<CDestination,int64>& mapVote)
{
    CDelegateContext ctxtDelegate;
    if (!Retrieve(hashBlock,ctxtDelegate))
    {
        return false;
    }
    mapVote = ctxtDelegate.mapVote;
    return true;
}

bool CDelegateDB::RetrieveEnrollTx(const uint256& hashAnchor,const vector<uint256>& vBlockRange,
                                   map<CDestination,CDiskPos>& mapEnrollTxPos)
{
    BOOST_REVERSE_FOREACH(const uint256& hash,vBlockRange)
    {
        CDelegateContext ctxtDelegate;
        if (!Retrieve(hash,ctxtDelegate))
        {
            return false;
        }

        map<uint256,map<CDestination,CDiskPos> >::iterator it = ctxtDelegate.mapEnrollTx.find(hashAnchor);
        if (it != ctxtDelegate.mapEnrollTx.end())
        {
            mapEnrollTxPos.insert((*it).second.begin(),(*it).second.end());
        }
    }
    return true;
}

void CDelegateDB::Clear()
{
    cacheDelegate.Clear();
    RemoveAll();
}

