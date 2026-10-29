#include "PrivateModelDirectory.h"
#include <algorithm>
namespace webserver::function3 {
std::vector<AssistantModelEntry>PrivateModelDirectory::list()const{auto result=models_.configured();std::erase_if(result,[](const auto&e){return e.runtime!="onnx-inprocess"&&e.runtime!="onnx-grpc";});return result;}
}
