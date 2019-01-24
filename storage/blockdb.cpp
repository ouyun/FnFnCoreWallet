// Copyright (c) 2017-2019 The Multiverse developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockdb.h"
#include "walleve/stream/datastream.h"

using namespace std;
using namespace multiverse::storage;

//////////////////////////////
// CBlockDB

CBlockDB::CBlockDB()
{
}

CBlockDB::~CBlockDB()
{
}

bool CBlockDB::DBPoolInitialize(const CMvDBConfig& config,int nMaxDBConn)
{

    if (!dbPool.Initialize(config,nMaxDBConn))
    {
        return false;
    }

    return true;
}

bool CBlockDB::Initialize(const boost::filesystem::path& pathData)
{
    if (!dbUnspent.Initialize(pathData))
    {
        return false;
    }

    if (!dbDelegate.Initialize(pathData))
    {
        return false;
    }
    
    if (!CreateTable())
    {
        return false;
    }

    return LoadFork();
}

void CBlockDB::Deinitialize()
{
    dbPool.Deinitialize();
    dbUnspent.Deinitialize();
    dbDelegate.Deinitialize();
}

bool CBlockDB::RemoveAll()
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }

    {
        CMvDBTxn txn(*db);
        txn.Query("TRUNCATE TABLE forkcontext");
        txn.Query("TRUNCATE TABLE fork");
        txn.Query("TRUNCATE TABLE block");
        txn.Query("TRUNCATE TABLE transaction");
        if (!txn.Commit())
        {
            return false;
        }
    }

    dbUnspent.Clear();
    dbDelegate.Clear();

    return true;
}

bool CBlockDB::AddNewForkContext(const CForkContext& ctxt)
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }
    ostringstream oss;
    oss << "INSERT INTO forkcontext (name,symbol,hash,parent,joint,txid,version,flag,mintreward,mintxfee,owner,jointheight) "
        <<    "VALUE("
        <<           "\'" << db->ToEscString(ctxt.strName) << "\',"
        <<           "\'" << db->ToEscString(ctxt.strSymbol) << "\',"
        <<           "\'" << db->ToEscString(ctxt.hashFork) << "\',"
        <<           "\'" << db->ToEscString(ctxt.hashParent) << "\',"
        <<           "\'" << db->ToEscString(ctxt.hashJoint) << "\',"
        <<           "\'" << db->ToEscString(ctxt.txidEmbedded) << "\',"
        <<           ctxt.nVersion << ","
        <<           ctxt.nFlag << ","
        <<           ctxt.nMintReward << ","
        <<           ctxt.nMinTxFee << ","
        <<           "\'" << db->ToEscString(ctxt.destOwner) << "\',"
        <<           ctxt.nJointHeight << ")";

    return db->Query(oss.str());
}

bool CBlockDB::RetrieveForkContext(const uint256& hash,CForkContext& ctxt)
{
    ctxt.SetNull();

    CMvDBInst db(&dbPool);
    if (db.Available())
    {
        ctxt.hashFork = hash;

        ostringstream oss;
        oss << "SELECT name,symbol,parent,joint,txid,version,flag,mintreward,mintxfee,owner,jointheight FROM forkcontext WHERE hash = "
            <<            "\'" << db->ToEscString(hash) << "\'";
        CMvDBRes res(*db,oss.str());
        return (res.GetRow()
                && res.GetField(0,ctxt.strName) && res.GetField(1,ctxt.strSymbol) 
                && res.GetField(2,ctxt.hashParent) && res.GetField(3,ctxt.hashJoint) 
                && res.GetField(4,ctxt.txidEmbedded) && res.GetField(5,ctxt.nVersion) 
                && res.GetField(6,ctxt.nFlag) && res.GetField(7,ctxt.nMintReward) 
                && res.GetField(8,ctxt.nMinTxFee) && res.GetField(9,ctxt.destOwner)
                && res.GetField(10,ctxt.nJointHeight));
    }

    return false;
}

bool CBlockDB::FilterForkContext(CForkContextFilter& filter)
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }

    string strQuery = "SELECT name,symbol,hash,parent,joint,txid,version,flag,mintreward,mintxfee,owner,jointheight FROM forkcontext";
    if (filter.hashParent != 0)
    {
        strQuery += string(" WHERE parent = \'") + db->ToEscString(filter.hashParent) + "\'";
        if (!filter.strSymbol.empty())
        {
            strQuery += string(" AND symbol = \'") + db->ToEscString(filter.strSymbol) + "\'";
        }
    }
    else if (!filter.strSymbol.empty())
    {
        strQuery += string(" WHERE symbol = \'") + db->ToEscString(filter.strSymbol) + "\'";
    }

    strQuery += " ORDER BY id";

    {
        CMvDBRes res(*db,strQuery,true);
        while (res.GetRow())
        {
            CForkContext ctxt;
            if (   !res.GetField(0,ctxt.strName)     || !res.GetField(1,ctxt.strSymbol)
                || !res.GetField(2,ctxt.hashFork)    || !res.GetField(3,ctxt.hashParent) 
                || !res.GetField(4,ctxt.hashJoint)   || !res.GetField(5,ctxt.txidEmbedded) 
                || !res.GetField(6,ctxt.nVersion)    || !res.GetField(7,ctxt.nFlag) 
                || !res.GetField(8,ctxt.nMintReward) || !res.GetField(9,ctxt.nMinTxFee) 
                || !res.GetField(10,ctxt.destOwner)  || !res.GetField(11,ctxt.nJointHeight)
                || !filter.FoundForkContext(ctxt) )
            {
                return false;
            }
        }
    }
    return true;
}

bool CBlockDB::AddNewFork(const uint256& hash)
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }
  
    string strEscHash = db->ToEscString(hash);
    {
        ostringstream oss;
        oss << "INSERT INTO fork(hash,refblock) "
            <<   "VALUE("
            <<          "\'" << strEscHash << "\',"
            <<          "\'" << db->ToEscString(uint256(uint64(0))) << "\')";
        if (!db->Query(oss.str()))
        {
            return false;
        }
    }

    if (!dbUnspent.AddNew(hash))
    {
        ostringstream del;
        del << "DELETE FROM fork WHERE hash = \'" << strEscHash << "\'";
        db->Query(del.str());
        return false;
    }

    return true;
}

bool CBlockDB::RemoveFork(const uint256& hash)
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }

    if (!dbUnspent.Remove(hash))
    {
        return false;
    }
    
    ostringstream oss;
    oss << "DELETE FROM fork WHERE hash = \'" << db->ToEscString(hash) << "\'";
    if (!db->Query(oss.str()))
    {
        return false;
    }

    return true;
}

bool CBlockDB::RetrieveFork(vector<uint256>& vFork)
{
    vFork.clear();

    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }

    {
        CMvDBRes res(*db,"SELECT refblock FROM fork",true);
        while (res.GetRow())
        {
            uint256 ref;
            if (!res.GetField(0,ref))
            {
                return false;
            }
            vFork.push_back(ref);
        }
    }
    return true;
}

bool CBlockDB::UpdateFork(const uint256& hash,const uint256& hashRefBlock,const uint256& hashForkBased,
                          const vector<pair<uint256,CTxIndex> >& vTxNew,const vector<uint256>& vTxDel,
                          const vector<CTxUnspent>& vAddNew,const vector<CTxOutPoint>& vRemove)
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }
   
    if (!dbUnspent.Exists(hash))
    {
        return false;
    }

    bool fIgnoreTxDel = false;
    if (hashForkBased != hash && hashForkBased != 0)
    {
        if (!dbUnspent.Copy(hashForkBased,hash))
        {
            return false;
        }
        fIgnoreTxDel = true;
    }

    {
        CMvDBTxn txn(*db);
        if (!fIgnoreTxDel)
        {
            for (int i = 0;i < vTxDel.size();i++)
            {
                ostringstream oss;
                oss << "DELETE FROM transaction WHERE txid = " 
                    <<            "\'" << db->ToEscString(vTxDel[i]) << "\'";
                txn.Query(oss.str());
            }
        }
        for (int i = 0;i < vTxNew.size();i++)
        {
            const CTxIndex& txIndex = vTxNew[i].second;
            string strEscTxid = db->ToEscString(vTxNew[i].first);
            ostringstream oss;
            oss << "INSERT INTO transaction(txid,version,type,lockuntil,anchor,sendto,amount,destin,valuein,height,file,offset) "
                      "VALUES("
                <<            "\'" << strEscTxid << "\',"
                <<            txIndex.nVersion << ","
                <<            txIndex.nType << ","
                <<            txIndex.nLockUntil << ","
                <<            "\'" << db->ToEscString(txIndex.hashAnchor) << "\',"
                <<            "\'" << db->ToEscString(txIndex.sendTo) << "\',"
                <<            txIndex.nAmount << ","
                <<            "\'" << db->ToEscString(txIndex.destIn) << "\',"
                <<            txIndex.nValueIn << ","
                <<            txIndex.nBlockHeight << ","
                <<            txIndex.nFile << ","
                <<            txIndex.nOffset << ")"
                << " ON DUPLICATE KEY UPDATE height = VALUES(height),file = VALUES(file),offset = VALUES(offset)";
            txn.Query(oss.str());
        }

        {
            ostringstream oss;
            oss << "UPDATE fork SET refblock = \'" << db->ToEscString(hashRefBlock) << "\' "
                                "WHERE hash = \'" << db->ToEscString(hash) << "\'"; 
            txn.Query(oss.str());
        }

        if (!dbUnspent.Update(hash,vAddNew,vRemove))
        {
            txn.Abort();
            return false;
        }

        if (!txn.Commit()) 
        {
            return false;
        }
    }
    return true;
}

bool CBlockDB::AddNewBlock(const CBlockOutline& outline)
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }
    
    ostringstream oss;
    oss << "INSERT INTO block(hash,prev,txid,minttype,version,type,time,height,beacon,trust,supply,algo,bits,file,offset) "
              "VALUES("
        <<            "\'" << db->ToEscString(outline.hashBlock) << "\',"
        <<            "\'" << db->ToEscString(outline.hashPrev) << "\',"
        <<            "\'" << db->ToEscString(outline.txidMint) << "\',"
        <<            outline.nMintType << ","
        <<            outline.nVersion << ","
        <<            outline.nType << ","
        <<            outline.nTimeStamp << ","
        <<            outline.nHeight << ","
        <<            outline.nRandBeacon << ","
        <<            outline.nChainTrust << ","
        <<            outline.nMoneySupply << ","
        <<            (int)outline.nProofAlgo << ","
        <<            (int)outline.nProofBits << ","
        <<            outline.nFile << ","
        <<            outline.nOffset << ")";
    return db->Query(oss.str());
}

bool CBlockDB::RemoveBlock(const uint256& hash)
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }

    ostringstream oss;
    oss << "DELETE FROM block" << " WHERE hash = \'" << db->ToEscString(hash) << "\'";
    return db->Query(oss.str());
}

bool CBlockDB::UpdateDelegateContext(const uint256& hash,const CDelegateContext& ctxtDelegate)
{
    return dbDelegate.AddNew(hash,ctxtDelegate);
}

bool CBlockDB::WalkThroughBlock(CBlockDBWalker& walker)
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }
    {
        CMvDBRes res(*db,"SELECT hash,prev,txid,minttype,version,type,time,height,beacon,trust,supply,algo,bits,file,offset FROM block ORDER BY id",true);
        while (res.GetRow())
        {
            CBlockOutline outline;
            if (   !res.GetField(0,outline.hashBlock)     || !res.GetField(1,outline.hashPrev)
                || !res.GetField(2,outline.txidMint)      || !res.GetField(3,outline.nMintType)
                || !res.GetField(4,outline.nVersion)      || !res.GetField(5,outline.nType)
                || !res.GetField(6,outline.nTimeStamp)    || !res.GetField(7,outline.nHeight)      
                || !res.GetField(8,outline.nRandBeacon)   || !res.GetField(9,outline.nChainTrust)  
                || !res.GetField(10,outline.nMoneySupply) || !res.GetField(11,outline.nProofAlgo)
                || !res.GetField(12,outline.nProofBits)   || !res.GetField(13,outline.nFile)       
                || !res.GetField(14,outline.nOffset)      || !walker.Walk(outline))
            {
                return false;
            }
        }
    }
    return true;
}

bool CBlockDB::ExistsTx(const uint256& txid)
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }
    {
        size_t count = 0;
        ostringstream oss;
        oss << "SELECT COUNT(*) FROM transaction WHERE txid = "
            <<            "\'" << db->ToEscString(txid) << "\'";

        CMvDBRes res(*db,oss.str());
        return (res.GetRow() && res.GetField(0,count) && count != 0);
    }
}

bool CBlockDB::RetrieveTxIndex(const uint256& txid,CTxIndex& txIndex)
{
    txIndex.SetNull();

    CMvDBInst db(&dbPool);
    if (db.Available())
    {
        ostringstream oss;
        oss << "SELECT version,type,lockuntil,anchor,sendto,amount,destin,valuein,height,file,offset FROM transaction WHERE txid = "
            <<            "\'" << db->ToEscString(txid) << "\'";
        CMvDBRes res(*db,oss.str());
        if (res.GetRow()
            && res.GetField(0,txIndex.nVersion) && res.GetField(1,txIndex.nType) 
            && res.GetField(2,txIndex.nLockUntil) && res.GetField(3,txIndex.hashAnchor) 
            && res.GetField(4,txIndex.sendTo) && res.GetField(5,txIndex.nAmount)
            && res.GetField(6,txIndex.destIn) && res.GetField(7,txIndex.nValueIn)
            && res.GetField(8,txIndex.nBlockHeight)
            && res.GetField(9,txIndex.nFile) && res.GetField(10,txIndex.nOffset))
        {
            return true;
        }
    }
    return false;
}

bool CBlockDB::RetrieveTxPos(const uint256& txid,uint32& nFile,uint32& nOffset)
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }
    {
        ostringstream oss;
        oss << "SELECT file,offset FROM transaction WHERE txid = "
            <<            "\'" << db->ToEscString(txid) << "\'";
        CMvDBRes res(*db,oss.str());
        return (res.GetRow() && res.GetField(0,nFile) && res.GetField(1,nOffset));
    }
}

bool CBlockDB::RetrieveTxLocation(const uint256& txid,uint256& hashAnchor,int& nBlockHeight)
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }
    {
        ostringstream oss;
        oss << "SELECT anchor,height FROM transaction WHERE txid = "
            <<            "\'" << db->ToEscString(txid) << "\'";
        CMvDBRes res(*db,oss.str());
        return (res.GetRow() && res.GetField(0,hashAnchor) && res.GetField(1,nBlockHeight));
    }
}

bool CBlockDB::RetrieveTxUnspent(const uint256& fork,const CTxOutPoint& out,CTxOutput& unspent)
{
    return dbUnspent.Retrieve(fork,out,unspent);
}

bool CBlockDB::RetrieveDelegate(const uint256& hash,map<CDestination,int64>& mapDelegate)
{
    return dbDelegate.RetrieveDelegatedVote(hash,mapDelegate);
}

bool CBlockDB::RetrieveEnroll(const uint256& hashAnchor,const vector<uint256>& vBlockRange, 
                                                        map<CDestination,CDiskPos>& mapEnrollTxPos)
{
    return dbDelegate.RetrieveEnrollTx(hashAnchor,vBlockRange,mapEnrollTxPos);
}

bool CBlockDB::CreateTable()
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }

    return (
        db->Query("CREATE TABLE IF NOT EXISTS forkcontext ("
                    "id INT NOT NULL AUTO_INCREMENT,"
                    "name VARCHAR(128) NOT NULL UNIQUE KEY,"
                    "symbol VARCHAR(16) NOT NULL,"
                    "hash BINARY(32) NOT NULL UNIQUE KEY,"
                    "parent BINARY(32) NOT NULL,"
                    "joint BINARY(32) NOT NULL,"
                    "txid BINARY(32) NOT NULL,"
                    "version SMALLINT UNSIGNED NOT NULL,"
                    "flag SMALLINT UNSIGNED NOT NULL,"
                    "mintreward BIGINT UNSIGNED NOT NULL,"
                    "mintxfee BIGINT UNSIGNED NOT NULL,"
                    "owner BINARY(33) NOT NULL,"
                    "jointheight INT NOT NULL,"
                    "INDEX(hash),INDEX(id))"
                    "ENGINE=InnoDB")
           &&
        db->Query("CREATE TABLE IF NOT EXISTS fork ("
                    "id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
                    "hash BINARY(32) NOT NULL UNIQUE KEY,"
                    "refblock BINARY(32) NOT NULL)"
                    "ENGINE=InnoDB")
           &&
        db->Query("CREATE TABLE IF NOT EXISTS block("
                    "id INT NOT NULL AUTO_INCREMENT,"
                    "hash BINARY(32) NOT NULL UNIQUE KEY,"
                    "prev BINARY(32) NOT NULL,"
                    "txid BINARY(32) NOT NULL,"
                    "minttype SMALLINT UNSIGNED NOT NULL,"
                    "version SMALLINT UNSIGNED NOT NULL,"
                    "type SMALLINT UNSIGNED NOT NULL,"
                    "time INT UNSIGNED NOT NULL,"
                    "height INT UNSIGNED NOT NULL,"
                    "beacon BIGINT UNSIGNED NOT NULL,"
                    "trust BIGINT UNSIGNED NOT NULL,"
                    "supply BIGINT NOT NULL,"
                    "algo TINYINT UNSIGNED NOT NULL,"
                    "bits TINYINT UNSIGNED NOT NULL,"
                    "file INT UNSIGNED NOT NULL,"
                    "offset INT UNSIGNED NOT NULL,"
                    "INDEX(id))"
                    "ENGINE=InnoDB PARTITION BY KEY(hash) PARTITIONS 16")
           &&
        db->Query("CREATE TABLE IF NOT EXISTS transaction("
                    "id INT NOT NULL AUTO_INCREMENT,"
                    "txid BINARY(32) NOT NULL UNIQUE KEY,"
                    "version SMALLINT UNSIGNED NOT NULL,"
                    "type SMALLINT UNSIGNED NOT NULL,"
                    "lockuntil INT UNSIGNED NOT NULL,"
                    "anchor BINARY(32) NOT NULL,"
                    "sendto BINARY(33) NOT NULL,"
                    "amount BIGINT UNSIGNED NOT NULL,"
                    "destin BINARY(33) NOT NULL,"
                    "valuein BIGINT UNSIGNED NOT NULL,"
                    "height INT NOT NULL,"
                    "file INT UNSIGNED NOT NULL,"
                    "offset INT UNSIGNED NOT NULL,"
                    "INDEX(txid),INDEX(id))"
                    "ENGINE=InnoDB PARTITION BY KEY(txid) PARTITIONS 256")
            );
}

bool CBlockDB::LoadFork()
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }
    {
        CMvDBRes res(*db,"SELECT hash FROM fork",true);
        while (res.GetRow())
        {
            uint256 hash;
            if (!res.GetField(0,hash))
            {
                return false;
            }
            if (!dbUnspent.AddNew(hash))
            {
                return false;
            }
        }
    }
    return true;
}


bool CBlockDB::InnoDB()
{
    CMvDBInst db(&dbPool);
    if (!db.Available())
    {
        return false;
    }

    vector<unsigned char> support;

    CMvDBRes res(*db,"SHOW ENGINES",true);

    while (res.GetRow())
    {
        res.GetField(0,support);

        string engine(support.begin(),support.end());

        if (engine == "InnoDB")
        {
            return true;
        }
    }

    return false;
}

