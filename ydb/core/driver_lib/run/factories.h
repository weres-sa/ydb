#pragma once
#include "log_backend.h"
#include <ydb/core/blobstorage/pdisk/blobstorage_pdisk_util_devicemode.h>
#include <ydb/core/kqp/kqp_query_replay.h>
#include <ydb/core/tx/datashard/export_iface.h>
#include <ydb/core/persqueue/actor_persqueue_client_iface.h>
#include <ydb/core/protos/auth.pb.h>
#include <ydb/core/base/grpc_service_factory.h>

#include <ydb/core/ymq/actor/auth_factory.h>

#include <ydb/library/folder_service/folder_service.h>
#include <ydb/library/folder_service/proto/config.pb.h>
#include <ydb/library/pdisk_io/aio.h>
#include <ydb/core/yq/libs/config/protos/audit.pb.h>

#include <ydb/library/yql/providers/pq/cm_client/interface/client.h>

#include <library/cpp/actors/core/actorsystem.h>

#include <ydb/library/security/ydb_credentials_provider_factory.h>

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NKikimr {

// A way to parameterize YDB binary, we do it via a set of factories
struct TModuleFactories {
    // A way to parameterize log backend
    std::shared_ptr<ILogBackendFactory> LogBackendFactory;
    // A backend factory for Query Replay
    std::shared_ptr<NKqp::IQueryReplayBackendFactory> QueryReplayBackendFactory;
    // 
    std::shared_ptr<NMsgBusProxy::IPersQueueGetReadSessionsInfoWorkerFactory> PQReadSessionsInfoWorkerFactory; 
    // Can be nullptr. In that case there would be no ability to work with Yandex Logbroker in Yandex Query.
    NPq::NConfigurationManager::IConnections::TPtr PqCmConnections;
    // Export implementation for Data Shards
    std::shared_ptr<NDataShard::IExportFactory> DataShardExportFactory;
    // Factory for Simple queue services implementation details
    std::shared_ptr<NSQS::IEventsWriterFactory> SqsEventsWriterFactory;

    IActor*(*CreateTicketParser)(const NKikimrProto::TAuthConfig&);
    IActor*(*FolderServiceFactory)(const NKikimrProto::NFolderService::TFolderServiceConfig&);

    std::function<IActor*(const NYq::NConfig::TAuditConfig& auditConfig)> YqAuditServiceFactory;
    NKikimr::TYdbCredentialsProviderFactory YdbCredentialProviderFactory;
    // Factory for grpc services
    TGrpcServiceFactory GrpcServiceFactory;

    std::shared_ptr<NPQ::IPersQueueMirrorReaderFactory> PersQueueMirrorReaderFactory; 
    /// Factory for pdisk's aio engines
    std::shared_ptr<NPDisk::IIoContextFactory> IoContextFactory;

    std::function<NActors::TMon* (NActors::TMon::TConfig)> MonitoringFactory;
    std::shared_ptr<NSQS::IAuthFactory> SqsAuthFactory;

    ~TModuleFactories();
};

} // NKikimr
