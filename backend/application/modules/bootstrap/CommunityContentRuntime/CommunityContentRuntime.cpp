#include "CommunityContentRuntime.h"
#include "backend/application/modules/activities/infrastructure/ActivityPostgresViews/ActivityPostgresViews.h"
#include "backend/application/modules/activities/transport/ActivityHttp/ActivityHttp.h"
#include "backend/application/modules/moderation/infrastructure/ReportPostgresRepository/ReportPostgresRepository.h"
#include "backend/application/modules/shared/infrastructure/Phase11Bridge/Phase11Bridge.h"
#include "backend/application/modules/tree/infrastructure/FfmpegMediaProcessor/FfmpegMediaProcessor.h"
#include "backend/application/modules/tree/infrastructure/TreeMediaPostgresRepository/TreeMediaPostgresRepository.h"
#include "backend/application/modules/tree/infrastructure/TreePostgresRepository/TreePostgresRepository.h"
#include "backend/application/modules/tree/transport/TreeHttp/TreeHttp.h"
#include "backend/application/modules/tree/transport/TreeMediaHttp/TreeMediaHttp.h"

#include <cstdlib>

namespace webserver::function3 {
namespace {
const char* configured(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}
}

struct CommunityContentRuntime::Impl {
    TreePostgresRepository treeRepository;
    ReportPostgresRepository reportRepository;
    TreeService tree;
    ModerationService moderation;
    TreeHttp treeHttp;
    ActivityPostgresViews activityViews;
    ActivityService activities;
    ActivityHttp activityHttp;
    FfmpegMediaProcessor mediaProcessor;
    TreeMediaPostgresRepository mediaRepository;
    TreeMediaService mediaService;
    // Destroy the executor before resources a running upload could use.
    phase11::BoundedExecutor mediaExecutor{1, 16};
    TreeMediaHttp mediaHttp;

    Impl(SqlContext& sql, Phase11Bridge& bridge, IEventWriter& events,
         phase11::transport::Phase11AuthHttp& auth, phase11::BoundedExecutor& executor,
         phase11::IFileBusiness& files, phase11::ISocialBusiness& social)
        : treeRepository(sql), reportRepository(sql),
          tree(sql, bridge, treeRepository, bridge, events, &treeRepository),
          moderation(sql, bridge, treeRepository, reportRepository, events),
          treeHttp(auth, executor, tree, moderation),
          activityViews(sql), activities(sql, bridge, tree, activityViews, activityViews),
          activityHttp(auth, executor, activities, social),
          mediaProcessor(configured("WEB_FUNCTION3_FFMPEG_ROOT",
                                    "/home/pikachu/phase11-deps/ffmpeg/8.1.3/install"),
                         configured("WEB_FUNCTION3_MEDIA_ROOT",
                                    "/home/pikachu/phase11-data/function3/media")),
          mediaRepository(sql), mediaService(tree, files, sql, mediaProcessor, mediaRepository),
          mediaHttp(auth, mediaExecutor, mediaService) {}
};

CommunityContentRuntime::CommunityContentRuntime(
        SqlContext& sql, Phase11Bridge& bridge, IEventWriter& events,
        phase11::transport::Phase11AuthHttp& auth, phase11::BoundedExecutor& executor,
        phase11::IFileBusiness& files, phase11::ISocialBusiness& social)
    : impl_(std::make_unique<Impl>(sql, bridge, events, auth, executor, files, social)) {}

CommunityContentRuntime::~CommunityContentRuntime() { stop(); }
TreeService& CommunityContentRuntime::tree() noexcept { return impl_->tree; }
ModerationService& CommunityContentRuntime::moderation() noexcept { return impl_->moderation; }
void CommunityContentRuntime::setCache(IQueryCache* cache) noexcept { impl_->treeHttp.setCache(cache); }
void CommunityContentRuntime::registerRoutes(Router& router) {
    impl_->activityHttp.registerRoutes(router);
    impl_->treeHttp.registerRoutes(router);
    impl_->mediaHttp.registerRoutes(router);
}
void CommunityContentRuntime::stop() noexcept { impl_->mediaExecutor.shutdown(); }
}
