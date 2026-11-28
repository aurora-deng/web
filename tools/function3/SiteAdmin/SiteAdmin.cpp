#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
#include "backend/application/modules/shared/domain/TextRules/TextRules.h"
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace webserver::function3;

int main(int argc, char** argv) {
    try {
        // This executable requires an independently authorized local DB owner.
        // No equivalent operator-grant operation is exposed over HTTP.
        const bool operatorGrant = argc == 5 &&
            std::string_view(argv[1]) == "--operator-grant-administrator" &&
            std::string_view(argv[4]) == "--confirm";
        const bool bootstrap = argc == 4 && std::string_view(argv[1]) == "--bootstrap";
        if (!operatorGrant && !bootstrap) {
            std::cerr << "Usage: function3-site-admin --bootstrap EXISTING_USERNAME REASON\n"
                      << "       function3-site-admin --operator-grant-administrator EXISTING_USERNAME REASON --confirm\n"
                      << "Offline database-owner maintenance only; every grant is audited.\n"
                      << "Read connection from WEB_PHASE11_DATABASE; no password in arguments.\n";
            return 2;
        }
        const auto* connection = std::getenv("WEB_PHASE11_DATABASE");
        if (!connection || !*connection) throw std::runtime_error("WEB_PHASE11_DATABASE required");
        const std::string reason = argv[3];
        if (trimText(reason).empty() || !textLength(reason, 200))
            throw std::runtime_error("reason required (up to 200 characters)");
        SqlContext sql({connection, 1, std::chrono::milliseconds(2000)});
        bool changed = false;
        sql.transact([&] {
            sql.query("SELECT pg_advisory_xact_lock(3700300)");
            if (bootstrap && !sql.query("SELECT 1 FROM site_roles r JOIN users u ON u.id=r.user_id "
                    "WHERE r.role='administrator' AND NOT u.disabled LIMIT 1").empty())
                throw std::runtime_error("an active administrator already exists; use the audited admin API");
            const auto user = sql.query("SELECT u.id,COALESCE(r.role,'user') FROM users u "
                "LEFT JOIN site_roles r ON r.user_id=u.id WHERE u.username=$1 "
                "AND NOT u.disabled AND NOT u.ai_account FOR UPDATE OF u", {argv[2]});
            if (user.empty()) throw std::runtime_error("active human account required");
            if (operatorGrant && user[0][1] == "administrator") return;
            sql.query("INSERT INTO site_roles VALUES($1,'administrator') "
                "ON CONFLICT(user_id) DO UPDATE SET role='administrator'", {user[0][0]});
            sql.query("UPDATE user_sessions SET revoked_at=CURRENT_TIMESTAMP "
                "WHERE user_id=$1 AND revoked_at IS NULL", {user[0][0]});
            sql.query("INSERT INTO site_audit(actor_id,action,resource,reason) VALUES($1,$2,$3,$4)",
                {user[0][0], operatorGrant ? "operator_grant_administrator" : "bootstrap_administrator",
                 "user:" + user[0][0], operatorGrant ? "offline database-owner maintenance: " + reason : reason});
            changed = true;
        });
        std::cout << (changed ? "Site administrator assigned. Log in again to use the admin console.\n"
                              : "Account is already a site administrator; no changes made.\n");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
