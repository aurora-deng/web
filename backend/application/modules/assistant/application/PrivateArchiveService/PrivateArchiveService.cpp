#include "PrivateArchiveService.h"
#include <openssl/evp.h>
#include <memory>
namespace webserver::function3 {
std::string PrivateArchiveService::digest(const LegacyPrivatePage&page){const auto material=privateArchiveMaterial(page);unsigned char bytes[32];unsigned int size=0;
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);if(!ctx||EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1||EVP_DigestUpdate(ctx.get(),material.data(),material.size())!=1||EVP_DigestFinal_ex(ctx.get(),bytes,&size)!=1)throw BusinessError(Failure::Busy,"archive checksum failed");
    std::string result;for(unsigned i=0;i<size;i++){result+="0123456789abcdef"[bytes[i]>>4];result+="0123456789abcdef"[bytes[i]&15];}return result;
}
LegacyPrivatePage PrivateArchiveService::exportPage(Id actor,std::string kind,Id resource,Id after,Id upper){activeActor(actors_,actor);if(kind!="conversation"&&kind!="knowledge")throw BusinessError(Failure::Invalid,"unknown private archive kind");auto result=archives_.exportPage(actor,std::move(kind),resource,after,upper);result.checksum=digest(result);return result;}
void PrivateArchiveService::purgePage(Id actor,std::string kind,Id resource,Id after,Id through,std::string checksum,bool confirmed){activeActor(actors_,actor);if(!confirmed||checksum.size()!=64||checksum.find_first_not_of("0123456789abcdef")!=std::string::npos||through<after)throw BusinessError(Failure::Invalid,"explicit confirmation and verified checksum required");if(kind!="conversation"&&kind!="knowledge")throw BusinessError(Failure::Invalid,"unknown private archive kind");const bool knowledge=kind=="knowledge";archives_.purgePage(actor,std::move(kind),resource,after,through,std::move(checksum),digest);if(knowledge&&invalidate_)invalidate_(resource);}
}
