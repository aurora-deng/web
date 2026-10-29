#include "TreeMediaPostgresRepository.h"
namespace webserver::function3 {
void TreeMediaPostgresRepository::save(const ValidatedMedia&v){sql_.requireTransaction();sql_.query("INSERT INTO tree_media_validation(file_id,kind,bytes,duration_ms,validator,storage_path,media_type) VALUES($1,$2,$3,$4,'ffmpeg-8.1.3',$5,$6) ON CONFLICT(file_id) DO NOTHING",{std::to_string(v.media.file),v.media.kind==MediaKind::Image?"image":"audio",std::to_string(v.media.bytes),std::to_string(v.media.duration),v.path,v.mime});}
std::optional<ValidatedMedia>TreeMediaPostgresRepository::get(Id file)const{auto r=sql_.query("SELECT kind,bytes,duration_ms,storage_path,media_type FROM tree_media_validation WHERE file_id=$1",{std::to_string(file)});if(r.empty())return {};auto&v=r[0];return ValidatedMedia{{file,v[0]=="image"?MediaKind::Image:MediaKind::Audio,std::stoull(v[1]),std::stoll(v[2])},v[3],v[4]};}
}
