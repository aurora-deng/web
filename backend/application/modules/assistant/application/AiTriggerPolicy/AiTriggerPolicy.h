#pragma once
#include "backend/application/domain/Domain/Domain.h"
namespace webserver::phase11 {
/** AI参与群聊的显式语义，不把普通群消息自动送进推理。 */
class AiTriggerPolicy final{
public:static void validate(std::string_view trigger,std::string_view prompt,const User&ai,bool replyTargetsAi);
};
}
