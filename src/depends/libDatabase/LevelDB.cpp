/**
* Copyright (c) 2018 Zilliqa
* This source code is being disclosed to you solely for the purpose of your participation in
* testing Zilliqa. You may view, compile and run the code for that purpose and pursuant to
* the protocols and algorithms that are programmed into, and intended by, the code. You may
* not do anything else with the code without express permission from Zilliqa Research Pte. Ltd.,
* including modifying or publishing the code (or any part of it), and developing or forming
* another public or private blockchain network. This source code is provided ‘as is’ and no
* warranties are given as to title or non-infringement, merchantability or fitness for purpose
* and, to the extent permitted by law, all liability for your use of the code is disclaimed.
* Some programs in this code are governed by the GNU General Public License v3.0 (available at
* https://www.gnu.org/licenses/gpl-3.0.en.html) (‘GPLv3’). The programs that are governed by
* GPLv3.0 are those programs that are located in the folders src/depends and tests/depends
* and which include a reference to GPLv3 in their program files.
**/

#include <string>

#include <boost/filesystem.hpp>

#include "LevelDB.h"
#include "common/Constants.h"
#include "depends/common/Common.h"
#include "depends/common/CommonData.h"
#include "depends/common/FixedHash.h"

using namespace std;


LevelDB::LevelDB(const string& dbName, const string& path, const string& subdirectory)
{
    this->m_subdirectory = subdirectory;
    this->m_dbName = dbName;

    if(!(boost::filesystem::exists("./"+path)))
    {
        LOG_GENERAL(WARNING, "./"+path+"does not exist");
        return;
    }

    leveldb::Options options;
    options.max_open_files = 256;
    options.create_if_missing = true;

    leveldb::DB* db;
    leveldb::Status status;

    if(m_subdirectory.empty())
    {
        status = leveldb::DB::Open(options, "./" + path + "/" + this->m_dbName, &db);
        LOG_GENERAL(INFO,"./" + path + "/" + this->m_dbName);
    }
    else
    {
        if (!(boost::filesystem::exists("./" + path + "/" + this->m_subdirectory)))
        {
            boost::filesystem::create_directories("./" + path + "/" + this->m_subdirectory);
        }
        status = leveldb::DB::Open(options, 
            "./" + path + "/" + this->m_subdirectory + "/" + this->m_dbName,
            &db);
        LOG_GENERAL(INFO,"./" + path + "/" + this->m_subdirectory + "/" + this->m_dbName);
    }

    if(!status.ok())
    {
        LOG_GENERAL(WARNING, "LevelDB status is not OK. "<<status.ToString());
    }

    m_db.reset(db);
}

LevelDB::LevelDB(const std::string & dbName, const std::string& subdirectory, bool diagnostic)
{
    this->m_subdirectory = subdirectory;
    this->m_dbName = dbName;

    leveldb::Options options;
    options.max_open_files = 256;
    options.create_if_missing = true;

    leveldb::DB* db;
    leveldb::Status status;

    // Diagnostic tool provides the option to pass the persistance db_path
    // that might not be the current directory (case when 'diagnostic' is true).
    // Its default value is false, and the non-diagnostic path is preserved
    // from the original code.
    string db_path = diagnostic ? (m_subdirectory + PERSISTENCE_PATH) : ("./" + PERSISTENCE_PATH + (m_subdirectory.empty() ? "" : "/" + m_subdirectory));
    if (!boost::filesystem::exists(db_path))
    {
        boost::filesystem::create_directories(db_path);
    }

    status = leveldb::DB::Open(options, db_path + "/" + this->m_dbName, &db);
    if(!status.ok())
    {
        // throw exception();
        LOG_GENERAL(WARNING, "LevelDB status is not OK.");
    }

    m_db.reset(db);
}

leveldb::Slice toSlice(boost::multiprecision::uint256_t num)
{
    dev::FixedHash<32> h;
    dev::bytesRef ref(h.data(), 32);
    dev::toBigEndian(num, ref);
    return (leveldb::Slice)h.ref();
}

string LevelDB::GetDBName()
{
        if (LOOKUP_NODE_MODE)
        {
            return m_dbName;
        }
        else
        {
            return m_dbName + (m_subdirectory.size() > 0 ? "/" : "") + m_subdirectory;
        }
}

string LevelDB::Lookup(const std::string & key) const
{
    string value;
    leveldb::Status s = m_db->Get(leveldb::ReadOptions(), key, &value);
    if (!s.ok())
    {
        // TODO
        return "";
    }

    return value;
}

string LevelDB::Lookup(const boost::multiprecision::uint256_t & blockNum) const
{
    string value;
    leveldb::Status s = m_db->Get(leveldb::ReadOptions(), blockNum.convert_to<string>(), &value);

    if (!s.ok())
    {
        // TODO
        return "";
    }

    return value;
}

string LevelDB::Lookup(const dev::h256 & key) const
{
    string value;
    leveldb::Status s = m_db->Get(leveldb::ReadOptions(), leveldb::Slice(key.hex()), &value);
    if (!s.ok())
    {
        // TODO
        return "";
    }

    return value;
}

string LevelDB::Lookup(const dev::bytesConstRef & key) const
{
    string value;
    leveldb::Status s = m_db->Get(leveldb::ReadOptions(), ldb::Slice((char const*)key.data(), 32),
                                  &value);
    if (!s.ok())
    {
        // TODO
        return "";
    }

    return value;
}

std::shared_ptr<leveldb::DB> LevelDB::GetDB()
{
    return this->m_db;
}

int LevelDB::Insert(const dev::h256 & key, dev::bytesConstRef value)
{
    return Insert(key, value.toString());
}

int LevelDB::Insert(const boost::multiprecision::uint256_t & blockNum,
                    const vector<unsigned char> & body)
{
    leveldb::Status s = m_db->Put(leveldb::WriteOptions(),
                                  leveldb::Slice(blockNum.convert_to<string>()),
                                  leveldb::Slice(vector_ref<const unsigned char>(&body[0],
                                                                                 body.size())));

    if (!s.ok())
    {
        return -1;
    }

    return 0;
}

int LevelDB::Insert(const boost::multiprecision::uint256_t & blockNum,
                    const std::string & body)
{
    leveldb::Status s = m_db->Put(leveldb::WriteOptions(),
                                  leveldb::Slice(blockNum.convert_to<string>()),
                                  leveldb::Slice(body.c_str(), body.size()));

    if (!s.ok())
    {
        return -1;
    }

    return 0;
}

int LevelDB::Insert(const string & key, const vector<unsigned char> & body)
{
    return Insert(leveldb::Slice(key), leveldb::Slice(dev::bytesConstRef(&body[0], body.size())));
}

int LevelDB::Insert(const leveldb::Slice & key, dev::bytesConstRef value)
{
    leveldb::Status s = m_db->Put(leveldb::WriteOptions(), key, ldb::Slice(value));
    if (!s.ok())
    {
        return -1;
    }

    return 0;
}

int LevelDB::Insert(const dev::h256 & key, const string & value)
{
    leveldb::Status s = m_db->Put(leveldb::WriteOptions(),
                                  ldb::Slice((char const*)key.data(), key.size),
                                  ldb::Slice(value.data(), value.size()));
    if (!s.ok())
    {
        return -1;
    }

    return 0;
}

int LevelDB::Insert(const dev::h256 & key, const vector<unsigned char> & body)
{
    leveldb::Status s = m_db->Put(leveldb::WriteOptions(), leveldb::Slice(key.hex()),
                                  leveldb::Slice(vector_ref<const unsigned char>(&body[0],
                                                                                 body.size())));
    if (!s.ok())
    {
        return -1;
    }

    return 0;
}

int LevelDB::Insert(const leveldb::Slice & key, const leveldb::Slice & value)
{
    leveldb::Status s = m_db->Put(leveldb::WriteOptions(), key, value);
    if (!s.ok())
    {
        return -1;
    }

    return 0;
}

int LevelDB::BatchInsert(std::unordered_map<dev::h256, std::pair<std::string, unsigned>> & m_main,
                         std::unordered_map<dev::h256, std::pair<dev::bytes, bool>> & m_aux)
{
    ldb::WriteBatch batch;

    for (const auto & i: m_main)
    {
        if (i.second.second)
        {
            batch.Put(leveldb::Slice(i.first.hex()),
                      leveldb::Slice(i.second.first.data(), i.second.first.size()));
        }
    }

    for (const auto & i: m_aux)
    {
        if (i.second.second)
        {
            dev::bytes b = i.first.asBytes();
            b.push_back(255);   // for aux
            batch.Put(dev::bytesConstRef(&b), dev::bytesConstRef(&i.second.first));
        }
    }

    ldb::Status s = m_db->Write(leveldb::WriteOptions(), &batch);

    if (!s.ok())
    {
        return -1;
    }

    return 0;
}

bool LevelDB::BatchInsert(const std::unordered_map<std::string, std::string>& kv_map)
{
    ldb::WriteBatch batch;

    for (const auto & i: kv_map)
    {
        if (!i.second.empty()) {
            batch.Put(leveldb::Slice(i.first),
                      leveldb::Slice(i.second));
        }
    }

    ldb::Status s = m_db->Write(leveldb::WriteOptions(), &batch);

    if (!s.ok())
    {
        return false;
    }

    return true;
}

bool LevelDB::Exists(const dev::h256 & key) const
{
    auto ret = Lookup(key);
    return !ret.empty();
}

bool LevelDB::Exists(const boost::multiprecision::uint256_t & blockNum) const
{
    auto ret = Lookup(blockNum);
    return !ret.empty();
}

bool LevelDB::Exists(const std::string & key) const
{
    auto ret = Lookup(key);
    return !ret.empty();
}

int LevelDB::DeleteKey(const dev::h256 & key)
{
    leveldb::Status s = m_db->Delete(leveldb::WriteOptions(), ldb::Slice(key.hex()));
    if (!s.ok())
    {
        return -1;
    }

    return 0;
}

int LevelDB::DeleteKey(const boost::multiprecision::uint256_t & blockNum)
{
    leveldb::Status s = m_db->Delete(leveldb::WriteOptions(), ldb::Slice(blockNum.convert_to<string>()));
    if (!s.ok())
    {
        return -1;
    }

    return 0;
}

int LevelDB::DeleteKey(const std::string & key)
{
    leveldb::Status s = m_db->Delete(leveldb::WriteOptions(), ldb::Slice(key));
    if(!s.ok())
    {
        return -1;
    }

    return 0;
}

int LevelDB::DeleteDB()
{
    if (LOOKUP_NODE_MODE)
    {
        return DeleteDBForLookupNode();
    }
    else
    {
        return DeleteDBForNormalNode();
    }
}

bool LevelDB::ResetDB()
{
    if (LOOKUP_NODE_MODE)
    {
        return ResetDBForLookupNode();
    }
    else
    {
        return ResetDBForNormalNode();
    }
}

int LevelDB::DeleteDBForNormalNode()
{
    m_db.reset();
    leveldb::Status s = leveldb::DestroyDB("./" + PERSISTENCE_PATH +
        (this->m_subdirectory.size() ? "/" + this->m_subdirectory : "") + "/" + this->m_dbName,
        leveldb::Options());
    if (!s.ok())
    {
        LOG_GENERAL(INFO, "[DeleteDB] Status: " << s.ToString());
        return -1;
    }

    if(this->m_subdirectory.size())
    {
        boost::filesystem::remove_all("./" + PERSISTENCE_PATH + "/" + this->m_subdirectory + "/" + this->m_dbName);
    }

    return 0;
}

bool LevelDB::ResetDBForNormalNode()
{
    if(DeleteDBForNormalNode() == 0 && this->m_subdirectory.empty())
    {
        boost::filesystem::remove_all("./" + PERSISTENCE_PATH + "/" + this->m_dbName);

        leveldb::Options options;
        options.max_open_files = 256;
        options.create_if_missing = true;

        leveldb::DB* db;

        leveldb::Status status = leveldb::DB::Open(options, "./" + PERSISTENCE_PATH + "/" + this->m_dbName, &db);
        if(!status.ok())
        {
            // throw exception();
            LOG_GENERAL(WARNING, "LevelDB status is not OK.");
        }

        m_db.reset(db);
        return true;
    }
    else if(this->m_subdirectory.size())
    {
        LOG_GENERAL(INFO, "DB in subdirectory cannot be reset");
    }
    LOG_GENERAL(WARNING, "Didn't reset DB, investigate why!");
    return false;
}

int LevelDB::DeleteDBForLookupNode()
{
    m_db.reset();
    leveldb::Status s = leveldb::DestroyDB(this->m_dbName, leveldb::Options());
    if (!s.ok())
    {
        LOG_GENERAL(INFO, "[DeleteDB] Status: " << s.ToString());
        return -1;
    }

    return 0;
}


bool LevelDB::ResetDBForLookupNode()
{
    if(DeleteDBForLookupNode()==0)
    {
        boost::filesystem::remove_all("./" + PERSISTENCE_PATH + "/" + this->m_dbName);

        leveldb::Options options;
        options.max_open_files = 256;
        options.create_if_missing = true;

        leveldb::DB* db;

        leveldb::Status status = leveldb::DB::Open(options, "./" + PERSISTENCE_PATH + "/" + this->m_dbName, &db);
        if(!status.ok())
        {
            // throw exception();
            LOG_GENERAL(WARNING, "LevelDB status is not OK.");
        }

        m_db.reset(db);
        return true;
    }
    return false;
}
