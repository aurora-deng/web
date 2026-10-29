#pragma once
#include <libpq-fe.h>
#include "backend/application/business/ApplicationError/ApplicationError.h"
#include "backend/application/observability/BusinessMetrics/BusinessMetrics.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
namespace webserver::phase11::pg {
    using Parameters=std::vector<std::optional<std::string>>;
    class Result final {
        public:
        explicit Result(PGresult*value):value_(value){
        }
        ~Result(){
            if(value_)PQclear(value_);
        }
        Result(const Result&)=delete;
        Result(Result&&other)noexcept:value_(std::exchange(other.value_,nullptr)){
        }
        int rows()const{
            return PQntuples(value_);
        }
        bool isNull(int row,int column)const{
            return PQgetisnull(value_,row,column);
        }
        std::string value(int row,int column)const{
            return isNull(row,column)?"":PQgetvalue(value_,row,column);
        }
        private:PGresult*value_;
    };
    /** All repositories receive the existing transaction's PGconn. No pool acquisition here. */
    inline Result query(PGconn*connection,std::string_view sql,const Parameters&parameters={
    }
    ){
        business_metrics::Timer timer{
            business_metrics::Database
        };
        std::vector<const char*>values;
        for(const auto&p:parameters)values.push_back(p?p->c_str():nullptr);
        auto*raw=PQexecParams(connection,std::string(sql).c_str(),static_cast<int>(values.size()),nullptr,values.data(),nullptr,nullptr,0);
        if(!raw)throw ApplicationError(ErrorCode::Unavailable,"database unavailable");
        if(PQresultStatus(raw)!=PGRES_TUPLES_OK&&PQresultStatus(raw)!=PGRES_COMMAND_OK){
            const char*state=PQresultErrorField(raw,PG_DIAG_SQLSTATE);
            const bool conflict=state&&std::string_view(state)=="23505";
            PQclear(raw);
            throw ApplicationError(conflict?ErrorCode::Conflict:ErrorCode::Unavailable,conflict?"resource already exists":"database operation failed");
        }
        return Result(raw);
    }
    inline std::string escapeLike(std::string_view input){
        std::string result;
        for(char c:input){
            if(c=='!'||c=='%'||c=='_')result+='!';
            result+=c;
        }
        return result;
    }
}
