#include "TreeTrainingHttp.h"

namespace webserver::function3 {
void TreeTrainingHttp::registerRoutes(Router& router) {
    auto scope = http::executor(router, executor_);
    router.GET("/api/tree/posts/:post/training-consent", [this](RequestContext& ctx) {
        return http::guarded(ctx, [&] {
            const auto actor = auth_.authorizeRequest(ctx).user.id;
            const bool allowed = service_.consent(actor, http::path(ctx, "post"));
            http::reply(ctx, std::string("{\"allowed\":") + (allowed ? "true}" : "false}"));
        });
    });
    router.PATCH("/api/tree/posts/:post/training-consent", [this](RequestContext& ctx) {
        return http::guarded(ctx, [&] {
            const auto actor = auth_.authorizeRequest(ctx, true).user.id;
            const auto post = http::path(ctx, "post");
            service_.setConsent(actor, post, http::boolean(http::field(http::body(ctx), "allowed")), http::now());
            const bool allowed = service_.consent(actor, post);
            http::reply(ctx, std::string("{\"allowed\":") + (allowed ? "true}" : "false}"));
        });
    });
    router.GET("/api/admin/tree/training", [this](RequestContext& ctx) {
        return http::guarded(ctx, [&] {
            const auto actor = auth_.authorizeRequest(ctx).user.id;
            const auto before = http::number(http::query(ctx, "before", "0"), true);
            const bool approved = http::boolean(http::query(ctx, "approved", "false"));
            const auto rows = service_.page(actor, before, approved);
            http::reply(ctx, http::array(rows, [](const auto& row) {
                return "{\"postId\":" + http::id(row.post) + ",\"text\":" + http::quote(row.text) +
                    ",\"reviewed\":" + (row.reviewed ? "true}" : "false}");
            }));
        });
    });
    router.POST("/api/admin/tree/training/:post/review", [this](RequestContext& ctx) {
        return http::guarded(ctx, [&] {
            const auto actor = auth_.authorizeRequest(ctx, true).user.id;
            service_.review(actor, http::path(ctx, "post"), http::field(http::body(ctx), "text"), http::now());
            http::reply(ctx, "{\"ok\":true}");
        });
    });
}
}
