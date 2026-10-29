#include "client/storage/SqliteStore/SqliteStore.h"
#include "client/storage/WindowsProtection/WindowsProtection.h"
#include <iostream>
using namespace nebula::client;
void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
int main(){try{
    const auto root=std::filesystem::current_path()/"private-storage-contract";
    createPrivateDirectory(root);
    SqliteStore store(root/"contract.db");store.execute("CREATE TABLE IF NOT EXISTS records(id INTEGER PRIMARY KEY,value TEXT)");store.execute("DELETE FROM records");
    const std::string body="中文资料 '; DROP TABLE records; --";
    store.execute("INSERT INTO records(value)VALUES(?)",{body});const auto rows=store.query("SELECT value FROM records");check(rows.size()==1&&rows[0][0]==body,"binding or UTF8 corrupted");
    store.execute("BEGIN IMMEDIATE");store.execute("DELETE FROM records");store.execute("ROLLBACK");check(store.query("SELECT value FROM records").size()==1,"transaction rollback failed");
    store.transact([&]{store.transact([&]{store.execute("INSERT INTO records(value)VALUES('nested')");});try{store.transact([&]{store.execute("DELETE FROM records");throw std::runtime_error("rollback nested only");});}catch(const std::runtime_error&){};check(store.query("SELECT value FROM records").size()==2,"nested savepoint erased outer writes");});
    try{store.transact([&]{store.transact([&]{store.execute("DELETE FROM records");});throw std::runtime_error("rollback all");});}catch(const std::runtime_error&){};check(store.query("SELECT value FROM records").size()==2,"outer rollback lost nested transaction");
    const auto secret=protectCredential("session-test-secret","site-A/user-1");check(secret.find("session-test-secret")==std::string::npos,"plaintext credential");check(unprotectCredential(secret,"site-A/user-1")=="session-test-secret","DPAPI round trip");
    bool rejected=false;try{unprotectCredential(secret,"site-B/user-1");}catch(...){rejected=true;}check(rejected,"site isolation missing");
    check(sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 mismatch");check(randomToken()!=randomToken(),"random token collision");
    std::cout<<"PASS Windows SQLite "<<store.version()<<", UTF8, bound SQL, rollback, DPAPI and site isolation\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
