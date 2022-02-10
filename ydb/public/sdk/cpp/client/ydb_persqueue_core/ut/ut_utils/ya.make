LIBRARY()

OWNER(g:logbroker)

SRCS(
    data_plane_helpers.cpp
    sdk_test_setup.h
    test_utils.h
    test_server.h
    test_server.cpp
)

PEERDIR(
    library/cpp/grpc/server
    library/cpp/testing/unittest
    ydb/core/testlib
    ydb/library/persqueue/topic_parser_public
    ydb/public/sdk/cpp/client/ydb_driver
    ydb/public/sdk/cpp/client/ydb_persqueue_core
    ydb/public/sdk/cpp/client/ydb_persqueue_public
    ydb/public/sdk/cpp/client/ydb_table
)

YQL_LAST_ABI_VERSION()

END()
