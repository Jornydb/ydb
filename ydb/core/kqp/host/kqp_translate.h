#pragma once

#include <ydb/core/kqp/provider/yql_kikimr_results.h>
#include <ydb/core/kqp/common/kqp.h>

namespace NKikimr {
namespace NKqp {

class TKqpTranslationSettingsBuilder {
public:
    TKqpTranslationSettingsBuilder(NYql::EKikimrQueryType queryType, ui16 kqpYqlSyntaxVersion, const TString& cluster,
            const TString& queryText, const NSQLTranslation::EBindingsMode& bindingsMode)
        : QueryType(queryType)
        , KqpYqlSyntaxVersion(kqpYqlSyntaxVersion)
        , Cluster(cluster)
        , QueryText(queryText)
        , BindingsMode(bindingsMode)
    {}

    NSQLTranslation::TTranslationSettings Build(NYql::TExprContext& ctx);

    TKqpTranslationSettingsBuilder& SetUsePgParser(const TMaybe<bool> value) {
        UsePgParser = value;
        return *this;
    }

    TKqpTranslationSettingsBuilder& SetKqpTablePathPrefix(const TString& value) {
        KqpTablePathPrefix = value;
        return *this;
    }

    TKqpTranslationSettingsBuilder& SetIsEnableExternalDataSources(bool value) {
        IsEnableExternalDataSources = value;
        return *this;
    }

    TKqpTranslationSettingsBuilder& SetIsEnablePgConstsToParams(bool value) {
        IsEnablePgConstsToParams = value;
        return *this;
    }

    TKqpTranslationSettingsBuilder& SetSqlAutoCommit(bool value) {
        SqlAutoCommit = value;
        return *this;
    }

    TKqpTranslationSettingsBuilder& SetApplicationName(const TMaybe<TString>& value) {
        ApplicationName = value;
        return *this;
    }

    TKqpTranslationSettingsBuilder& SetQueryParameters(const std::shared_ptr<std::map<TString, Ydb::Type>>& value) {
        QueryParameters = value;
        return *this;
    }

    TKqpTranslationSettingsBuilder& SetSqlVersion(const TMaybe<ui16>& value) {
        SqlVersion = value;
        return *this;
    }

    TKqpTranslationSettingsBuilder& SetPerStatementResult(const bool value) {
        PerStatementResult = value;
        return *this;
    }

private:
    const NYql::EKikimrQueryType QueryType;
    const ui16 KqpYqlSyntaxVersion;
    const TString Cluster;
    const TString QueryText;
    const NSQLTranslation::EBindingsMode BindingsMode;

    TMaybe<bool> UsePgParser = {};
    TString KqpTablePathPrefix = {};
    bool IsEnableExternalDataSources = false;
    bool IsEnablePgConstsToParams = false;
    TMaybe<bool> SqlAutoCommit = {};
    TMaybe<TString> ApplicationName = {};
    std::shared_ptr<std::map<TString, Ydb::Type>> QueryParameters = {};
    TMaybe<ui16> SqlVersion;
    bool PerStatementResult = false;
};

NSQLTranslation::EBindingsMode RemapBindingsMode(NKikimrConfig::TTableServiceConfig::EBindingsMode mode);

NYql::EKikimrQueryType ConvertType(NKikimrKqp::EQueryType type);

NYql::TAstParseResult ParseQuery(const TString& queryText, bool isSql, TMaybe<ui16>& sqlVersion, bool& deprecatedSQL,
    NYql::TExprContext& ctx, TKqpTranslationSettingsBuilder& settingsBuilder);

TQueryAst ParseQuery(const TString& queryText, const TMaybe<Ydb::Query::Syntax>& syntax, bool isSql, TKqpTranslationSettingsBuilder& settingsBuilder);

TVector<TQueryAst> ParseStatements(const TString& queryText, const TMaybe<Ydb::Query::Syntax>& syntax, bool isSql, TKqpTranslationSettingsBuilder& settingsBuilder, bool perStatementExecution);

} // namespace NKqp
} // namespace NKikimr
