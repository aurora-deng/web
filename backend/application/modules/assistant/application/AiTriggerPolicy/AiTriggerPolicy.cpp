#include "AiTriggerPolicy.h"
#include "backend/application/business/ApplicationError/ApplicationError.h"
namespace webserver::phase11 {
void AiTriggerPolicy::validate(std::string_view trigger,std::string_view prompt,const User&ai,bool replyTargetsAi){
    if(trigger=="explicit")return;
    if(trigger=="mention"){
        if(prompt.find("@"+ai.username)!=std::string_view::npos||(!ai.displayName.empty()&&prompt.find("@"+ai.displayName)!=std::string_view::npos))return;
        throw ApplicationError(ErrorCode::InvalidArgument,"mention must identify the selected AI member");
    }
    if(trigger=="reply"&&replyTargetsAi)return;
    throw ApplicationError(ErrorCode::InvalidArgument,"AI requires explicit request, targeted mention, or reply to a visible AI message");
}
}
