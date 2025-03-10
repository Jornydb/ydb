LLVM_BC(
    postgresql/src/backend/access/brin/brin_minmax_multi.c
    postgresql/src/backend/access/gist/gist.c
    postgresql/src/backend/access/heap/heapam_handler.c
    postgresql/src/backend/utils/adt/json.c
    postgresql/src/backend/utils/adt/network.c
    postgresql/src/backend/utils/adt/selfuncs.c
    postgresql/src/backend/utils/adt/uuid.c
    postgresql/src/backend/utils/mb/mbutils.c
    postgresql/src/backend/utils/misc/guc.c
    pg_kernels.16.cpp

    NAME PgFuncs16

    SYMBOLS
    arrow_PG_char_to_encoding
    arrow_PG_encoding_to_char
    arrow_array_to_json
    arrow_array_to_json_pretty
    arrow_brin_minmax_multi_add_value
    arrow_brin_minmax_multi_consistent
    arrow_brin_minmax_multi_distance_date
    arrow_brin_minmax_multi_distance_float4
    arrow_brin_minmax_multi_distance_float8
    arrow_brin_minmax_multi_distance_inet
    arrow_brin_minmax_multi_distance_int2
    arrow_brin_minmax_multi_distance_int4
    arrow_brin_minmax_multi_distance_int8
    arrow_brin_minmax_multi_distance_interval
    arrow_brin_minmax_multi_distance_macaddr
    arrow_brin_minmax_multi_distance_macaddr8
    arrow_brin_minmax_multi_distance_numeric
    arrow_brin_minmax_multi_distance_pg_lsn
    arrow_brin_minmax_multi_distance_tid
    arrow_brin_minmax_multi_distance_time
    arrow_brin_minmax_multi_distance_timestamp
    arrow_brin_minmax_multi_distance_timetz
    arrow_brin_minmax_multi_distance_uuid
    arrow_brin_minmax_multi_opcinfo
    arrow_brin_minmax_multi_options
    arrow_brin_minmax_multi_summary_in
    arrow_brin_minmax_multi_summary_out
    arrow_brin_minmax_multi_summary_recv
    arrow_brin_minmax_multi_summary_send
    arrow_brin_minmax_multi_union
    arrow_cidr_abbrev
    arrow_cidr_in
    arrow_cidr_out
    arrow_cidr_recv
    arrow_cidr_send
    arrow_cidr_set_masklen
    arrow_eqjoinsel
    arrow_eqsel
    arrow_gen_random_uuid
    arrow_getdatabaseencoding
    arrow_gisthandler
    arrow_hashinet
    arrow_hashinetextended
    arrow_heap_tableam_handler
    arrow_inet_abbrev
    arrow_inet_client_addr
    arrow_inet_client_port
    arrow_inet_in
    arrow_inet_merge
    arrow_inet_out
    arrow_inet_recv
    arrow_inet_same_family
    arrow_inet_send
    arrow_inet_server_addr
    arrow_inet_server_port
    arrow_inet_set_masklen
    arrow_inet_to_cidr
    arrow_inetand
    arrow_inetmi
    arrow_inetmi_int8
    arrow_inetnot
    arrow_inetor
    arrow_inetpl
    arrow_json_agg_finalfn
    arrow_json_agg_transfn
    arrow_json_build_array_noargs
    arrow_json_build_object_noargs
    arrow_json_in
    arrow_json_object
    arrow_json_object_agg_finalfn
    arrow_json_object_agg_transfn
    arrow_json_object_two_arg
    arrow_json_out
    arrow_json_recv
    arrow_json_send
    arrow_json_typeof
    arrow_length_in_encoding
    arrow_matchingjoinsel
    arrow_matchingsel
    arrow_neqjoinsel
    arrow_neqsel
    arrow_network_broadcast
    arrow_network_cmp
    arrow_network_eq
    arrow_network_family
    arrow_network_ge
    arrow_network_gt
    arrow_network_host
    arrow_network_hostmask
    arrow_network_larger
    arrow_network_le
    arrow_network_lt
    arrow_network_masklen
    arrow_network_ne
    arrow_network_netmask
    arrow_network_network
    arrow_network_overlap
    arrow_network_show
    arrow_network_smaller
    arrow_network_sortsupport
    arrow_network_sub
    arrow_network_subeq
    arrow_network_subset_support
    arrow_network_sup
    arrow_network_supeq
    arrow_pg_client_encoding
    arrow_pg_convert
    arrow_pg_convert_from
    arrow_pg_convert_to
    arrow_pg_encoding_max_length_sql
    arrow_row_to_json
    arrow_row_to_json_pretty
    arrow_scalargejoinsel
    arrow_scalargesel
    arrow_scalargtjoinsel
    arrow_scalargtsel
    arrow_scalarlejoinsel
    arrow_scalarlesel
    arrow_scalarltjoinsel
    arrow_scalarltsel
    arrow_set_config_by_name
    arrow_show_config_by_name
    arrow_show_config_by_name_missing_ok
    arrow_to_json
    arrow_uuid_cmp
    arrow_uuid_eq
    arrow_uuid_ge
    arrow_uuid_gt
    arrow_uuid_hash
    arrow_uuid_hash_extended
    arrow_uuid_in
    arrow_uuid_le
    arrow_uuid_lt
    arrow_uuid_ne
    arrow_uuid_out
    arrow_uuid_recv
    arrow_uuid_send
    arrow_uuid_sortsupport
)
