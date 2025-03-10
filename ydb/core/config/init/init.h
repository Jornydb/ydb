#pragma once

#include <ydb/core/base/event_filter.h>
#include <ydb/core/cms/console/config_item_info.h>
#include <ydb/core/config/init/source_location.h>
#include <ydb/core/driver_lib/run/config.h>
#include <ydb/core/protos/config.pb.h>
#include <ydb/library/actors/core/interconnect.h>
#include <ydb/public/lib/deprecated/kicli/kicli.h>

#include <library/cpp/getopt/small/last_getopt_opts.h>

#include <util/generic/hash.h>
#include <util/generic/vector.h>
#include <util/generic/string.h>
#include <util/datetime/base.h>

namespace NKikimr::NConfig {

struct TCallContext {
    const char* File;
    ui32 Line;

    static TCallContext From(const NCompat::TSourceLocation& location) {
        return TCallContext{location.file_name(), static_cast<ui32>(location.line())};
    }
};

class IEnv {
public:
    virtual ~IEnv() {}
    virtual TString HostName() const = 0;
    virtual TString FQDNHostName() const = 0;
    virtual TString ReadFromFile(const TString& filePath, const TString& fileName, bool allowEmpty = true) const = 0;
    virtual void Sleep(const TDuration& dur) const = 0;
};

class IErrorCollector {
public:
    virtual ~IErrorCollector() {}
    // TODO(Enjection): CFG-UX-0 replace regular throw with just collecting
    virtual void Fatal(TString error) = 0;
};

class IProtoConfigFileProvider {
public:
    virtual ~IProtoConfigFileProvider() {}
    virtual void AddConfigFile(TString optName, TString description) = 0;
    virtual void RegisterCliOptions(NLastGetopt::TOpts& opts) const = 0;
    virtual TString GetProtoFromFile(const TString& path, IErrorCollector& errorCollector) const = 0;
    virtual bool Has(TString optName) = 0;
    virtual TString Get(TString optName) = 0;
};

class IConfigUpdateTracer {
public:
    virtual ~IConfigUpdateTracer() {}
    void AddUpdate(ui32 kind, TConfigItemInfo::EUpdateKind update, const NCompat::TSourceLocation location = NCompat::TSourceLocation::current()) {
        return this->Add(kind, TConfigItemInfo::TUpdate{location.file_name(), location.line(), update});
    }
    void AddUpdate(ui32 kind, TConfigItemInfo::EUpdateKind update, TCallContext ctx) {
        return this->Add(kind, TConfigItemInfo::TUpdate{ctx.File, ctx.Line, update});
    }
    virtual void Add(ui32 kind, TConfigItemInfo::TUpdate update) = 0;
    virtual THashMap<ui32, TConfigItemInfo> Dump() const = 0;
};

class IInitLogger {
public:
    virtual ~IInitLogger() {}
    virtual IOutputStream& Out() const noexcept = 0;
    virtual IOutputStream& Err() const noexcept = 0;
};

// ===

class IMemLogInitializer {
public:
    virtual ~IMemLogInitializer() {}
    virtual void Init(const NKikimrConfig::TMemoryLogConfig& mem) const = 0;
};

// ===

struct TGrpcSslSettings {
    TString PathToGrpcCertFile;
    TString PathToGrpcCaFile;
    TString PathToGrpcPrivateKeyFile;
};

struct TNodeRegistrationSettings {
    TString DomainName;
    TString NodeHost;
    TString NodeAddress;
    TString NodeResolveHost;
    TMaybe<TString> Path;
    bool FixedNodeID;
    ui32 InterconnectPort;
    NActors::TNodeLocation Location;
};

class INodeRegistrationResult {
public:
    virtual ~INodeRegistrationResult() {}
    virtual void Apply(NKikimrConfig::TAppConfig& appConfig, ui32& nodeId, TKikimrScopeId& scopeId) const = 0;
};

class INodeBrokerClient {
public:
    virtual ~INodeBrokerClient() {}
    virtual std::unique_ptr<INodeRegistrationResult> RegisterDynamicNode(
        const TGrpcSslSettings& grpcSettings,
        const TVector<TString>& addrs,
        const TNodeRegistrationSettings& regSettings,
        const IEnv& env,
        IInitLogger& logger) const = 0;
};

// ===

struct TDynConfigSettings {
    ui32 NodeId;
    TString DomainName;
    TString TenantName;
    TString FQDNHostName;
    TString NodeType;
    TString StaffApiUserToken;
};

class IDynConfigClient {
public:
    virtual ~IDynConfigClient() {}
    virtual TMaybe<NKikimr::NClient::TConfigurationResult> GetConfig(
        const TGrpcSslSettings& gs,
        const TVector<TString>& addrs,
        const TDynConfigSettings& settings,
        const IEnv& env,
        IInitLogger& logger) const = 0;
};

// ===

class IInitialConfigurator {
public:
    virtual ~IInitialConfigurator() {};
    virtual void RegisterCliOptions(NLastGetopt::TOpts& opts) = 0;
    virtual void ValidateOptions(const NLastGetopt::TOpts& opts, const NLastGetopt::TOptsParseResult& parseResult) = 0;
    virtual void Parse(const TVector<TString>& freeArgs) = 0;
    virtual void Apply(
        NKikimrConfig::TAppConfig& appConfig,
        ui32& nodeId,
        TKikimrScopeId& scopeId,
        TString& tenantName,
        TBasicKikimrServicesMask& servicesMask,
        TMap<TString, TString>& labels,
        TString& clusterName,
        NKikimrConfig::TAppConfig& initialCmsConfig,
        NKikimrConfig::TAppConfig& initialCmsYamlConfig,
        THashMap<ui32, TConfigItemInfo>& configInitInfo) const = 0;
};

struct TInitialConfiguratorDependencies {
    NConfig::IErrorCollector& ErrorCollector;
    NConfig::IProtoConfigFileProvider& ProtoConfigFileProvider;
    NConfig::IConfigUpdateTracer& ConfigUpdateTracer;
    NConfig::IMemLogInitializer& MemLogInit;
    NConfig::INodeBrokerClient& NodeBrokerClient;
    NConfig::IDynConfigClient& DynConfigClient;
    NConfig::IEnv& Env;
    NConfig::IInitLogger& Logger;
};

std::unique_ptr<IConfigUpdateTracer> MakeDefaultConfigUpdateTracer();
std::unique_ptr<IProtoConfigFileProvider> MakeDefaultProtoConfigFileProvider();
std::unique_ptr<IEnv> MakeDefaultEnv();
std::unique_ptr<IErrorCollector> MakeDefaultErrorCollector();
std::unique_ptr<IMemLogInitializer> MakeDefaultMemLogInitializer();
std::unique_ptr<INodeBrokerClient> MakeDefaultNodeBrokerClient();
std::unique_ptr<IDynConfigClient> MakeDefaultDynConfigClient();
std::unique_ptr<IInitLogger> MakeDefaultInitLogger();

std::unique_ptr<IInitialConfigurator> MakeDefaultInitialConfigurator(TInitialConfiguratorDependencies deps);

class TInitialConfigurator {
public:
    TInitialConfigurator(TInitialConfiguratorDependencies deps)
            : Impl(MakeDefaultInitialConfigurator(deps))
    {}

    void RegisterCliOptions(NLastGetopt::TOpts& opts) {
        Impl->RegisterCliOptions(opts);
    }

    void ValidateOptions(const NLastGetopt::TOpts& opts, const NLastGetopt::TOptsParseResult& parseResult) {
        Impl->ValidateOptions(opts, parseResult);
    }

    void Parse(const TVector<TString>& freeArgs) {
        Impl->Parse(freeArgs);
    }

    void Apply(
        NKikimrConfig::TAppConfig& appConfig,
        ui32& nodeId,
        TKikimrScopeId& scopeId,
        TString& tenantName,
        TBasicKikimrServicesMask& servicesMask,
        TMap<TString, TString>& labels,
        TString& clusterName,
        NKikimrConfig::TAppConfig& initialCmsConfig,
        NKikimrConfig::TAppConfig& initialCmsYamlConfig,
        THashMap<ui32, TConfigItemInfo>& configInitInfo) const
    {
        Impl->Apply(
            appConfig,
            nodeId,
            scopeId,
            tenantName,
            servicesMask,
            labels,
            clusterName,
            initialCmsConfig,
            initialCmsYamlConfig,
            configInitInfo);
    }

private:
    std::unique_ptr<IInitialConfigurator> Impl;
};

} // namespace NKikimr::NConfig
