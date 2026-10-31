#include "backend/application/bootstrap/StorageRuntime/StorageRuntime.h"
#include "backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h"

#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
namespace webserver::bootstrap {
struct StorageRuntime::Impl {
    std::string connection;
    phase11::PostgresStore store;
    std::unique_ptr<phase11::PostgresStore> delivery;
    phase11::BoundedExecutor executor;
    Impl(std::string c,bool separate):connection(std::move(c)),
      store({connection,size("WEB_PHASE11_DB_POOL",4,1,32),std::chrono::milliseconds(size("WEB_PHASE11_DB_ACQUIRE_MS",5000,10,60000))}),
      executor(size("WEB_PHASE11_DB_WORKERS",4,1,32),size("WEB_PHASE11_DB_QUEUE",256,1,65536)){
        if(!store.healthy())throw std::runtime_error("Phase 11 PostgreSQL health check failed");
        store.requireMigrations(24);
        if(separate)delivery=std::make_unique<phase11::PostgresStore>(phase11::PostgresPoolConfig{connection,size("WEB_FUNCTION3_DELIVERY_DB_POOL",2,1,8),std::chrono::milliseconds(2000)});
    }
};
StorageRuntime::StorageRuntime(std::string c,bool d):impl_(std::make_unique<Impl>(std::move(c),d)){}
StorageRuntime::~StorageRuntime()=default;
phase11::PostgresStore& StorageRuntime::store(){return impl_->store;}
phase11::PostgresStore& StorageRuntime::delivery(){return impl_->delivery?*impl_->delivery:impl_->store;}
phase11::PostgresStore* StorageRuntime::deliveryMetrics()noexcept{return impl_->delivery.get();}
phase11::BoundedExecutor& StorageRuntime::executor(){return impl_->executor;}
const std::string& StorageRuntime::connection()const{return impl_->connection;}
void StorageRuntime::drain()noexcept{impl_->executor.shutdown();}
}
