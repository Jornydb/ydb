LIBRARY()

SRCS(
    yql_yt_datasink_constraints.cpp
    yql_yt_datasink_exec.cpp
    yql_yt_datasink_finalize.cpp
    yql_yt_datasink_trackable.cpp
    yql_yt_datasink_type_ann.cpp
    yql_yt_datasink.cpp
    yql_yt_datasource_constraints.cpp
    yql_yt_datasource_exec.cpp
    yql_yt_datasource_type_ann.cpp
    yql_yt_datasource.cpp
    yql_yt_epoch.cpp
    yql_yt_gateway.cpp
    yql_yt_horizontal_join.cpp
    yql_yt_helpers.cpp
    yql_yt_intent_determination.cpp
    yql_yt_io_discovery.cpp
    yql_yt_io_discovery_walk_folders.cpp
    yql_yt_join_impl.cpp
    yql_yt_join_reorder.cpp
    yql_yt_key.cpp
    yql_yt_load_table_meta.cpp
    yql_yt_load_columnar_stats.cpp
    yql_yt_logical_optimize.cpp
    yql_yt_mkql_compiler.cpp
    yql_yt_op_hash.cpp
    yql_yt_op_settings.cpp
    yql_yt_optimize.cpp
    yql_yt_peephole.cpp
    yql_yt_physical_finalizing.cpp
    yql_yt_physical_optimize.cpp
    yql_yt_provider_impl.cpp
    yql_yt_provider.cpp
    yql_yt_provider.h
    yql_yt_provider_impl.h
    yql_yt_table_desc.cpp
    yql_yt_table.cpp
    yql_yt_dq_integration.cpp
    yql_yt_dq_optimize.cpp
    yql_yt_dq_hybrid.cpp
    yql_yt_wide_flow.cpp
)

PEERDIR(
    library/cpp/yson/node
    library/cpp/disjoint_sets
    yt/cpp/mapreduce/common
    yt/cpp/mapreduce/interface
    ydb/library/yql/ast
    ydb/library/yql/core/extract_predicate
    ydb/library/yql/public/udf
    ydb/library/yql/public/udf/tz
    ydb/library/yql/sql
    ydb/library/yql/utils
    ydb/library/yql/utils/log
    ydb/library/yql/core
    ydb/library/yql/core/expr_nodes
    ydb/library/yql/core/issue
    ydb/library/yql/core/issue/protos
    ydb/library/yql/core/peephole_opt
    ydb/library/yql/core/type_ann
    ydb/library/yql/core/file_storage
    ydb/library/yql/core/url_lister/interface
    ydb/library/yql/dq/integration
    ydb/library/yql/dq/opt
    ydb/library/yql/dq/type_ann
    ydb/library/yql/minikql
    ydb/library/yql/providers/common/codec
    ydb/library/yql/providers/common/config
    ydb/library/yql/providers/common/dq
    ydb/library/yql/providers/common/mkql
    ydb/library/yql/providers/common/proto
    ydb/library/yql/providers/common/activation
    ydb/library/yql/providers/common/provider
    ydb/library/yql/providers/common/schema/expr
    ydb/library/yql/providers/common/transform
    ydb/library/yql/providers/dq/common
    ydb/library/yql/providers/dq/expr_nodes
    ydb/library/yql/providers/result/expr_nodes
    ydb/library/yql/providers/stat/expr_nodes
    ydb/library/yql/providers/yt/common
    ydb/library/yql/providers/yt/expr_nodes
    ydb/library/yql/providers/yt/lib/expr_traits
    ydb/library/yql/providers/yt/lib/graph_reorder
    ydb/library/yql/providers/yt/lib/hash
    ydb/library/yql/providers/yt/lib/key_filter
    ydb/library/yql/providers/yt/lib/mkql_helpers
    ydb/library/yql/providers/yt/lib/res_pull
    ydb/library/yql/providers/yt/lib/row_spec
    ydb/library/yql/providers/yt/lib/schema
    ydb/library/yql/providers/yt/lib/skiff
    ydb/library/yql/providers/yt/lib/yson_helpers
    ydb/library/yql/providers/yt/opt
)

YQL_LAST_ABI_VERSION()

GENERATE_ENUM_SERIALIZATION(yql_yt_op_settings.h)

END()

RECURSE_FOR_TESTS(
    ut
)
