#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
#include <cstdlib>
#include <iostream>
using namespace webserver::function3;
int main(int argc,char**argv){try{
    if(argc!=4||std::string_view(argv[1])!="--bootstrap"){std::cerr<<"Usage: function3-site-admin --bootstrap EXISTING_USERNAME REASON\nRead connection from WEB_PHASE11_DATABASE; no password in arguments.\n";return 2;}
    const auto *connection=std::getenv("WEB_PHASE11_DATABASE");if(!connection||!*connection)throw std::runtime_error("WEB_PHASE11_DATABASE required");
    const std::string reason=argv[3];if(reason.empty()||reason.size()>800)throw std::runtime_error("reason required (up to 200 Chinese characters)");
    SqlContext sql({connection,1,std::chrono::milliseconds(2000)});
    sql.transact([&]{sql.query("SELECT pg_advisory_xact_lock(3700300)");
        if(!sql.query("SELECT 1 FROM site_roles r JOIN users u ON u.id=r.user_id WHERE r.role='administrator' AND NOT u.disabled LIMIT 1").empty())throw std::runtime_error("an active administrator already exists; use the audited admin API");
        auto user=sql.query("SELECT id FROM users WHERE username=$1 AND NOT disabled AND NOT ai_account FOR UPDATE",{argv[2]});if(user.empty())throw std::runtime_error("active human account required");
        sql.query("INSERT INTO site_roles VALUES($1,'administrator') ON CONFLICT(user_id) DO UPDATE SET role='administrator'",{user[0][0]});
        sql.query("UPDATE user_sessions SET revoked_at=CURRENT_TIMESTAMP WHERE user_id=$1 AND revoked_at IS NULL",{user[0][0]});
        sql.query("INSERT INTO site_audit(actor_id,action,resource,reason) VALUES($1,'bootstrap_administrator',$2,$3)",{user[0][0],"user:"+user[0][0],reason});
    });
    std::cout<<"First site administrator assigned. Log in again to use the admin console.\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
