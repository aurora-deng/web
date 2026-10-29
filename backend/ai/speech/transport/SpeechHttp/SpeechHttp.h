#pragma once
#include "backend/application/modules/announcement/application/AnnouncementService/AnnouncementService.h"
#include "backend/ai/speech/application/SpeechService/SpeechService.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
namespace webserver::function3 {
class SpeechHttp final {
public:
    SpeechHttp(phase11::transport::Phase11AuthHttp&auth,phase11::DatabaseExecutor&executor,AnnouncementService&announcements,SpeechService&speech):auth_(auth),executor_(executor),announcements_(announcements),speech_(speech){}
    void registerRoutes(Router&router);
private:phase11::transport::Phase11AuthHttp&auth_;phase11::DatabaseExecutor&executor_;AnnouncementService&announcements_;SpeechService&speech_;
};
}
