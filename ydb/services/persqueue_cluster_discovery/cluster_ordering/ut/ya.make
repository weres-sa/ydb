UNITTEST_FOR(ydb/services/persqueue_cluster_discovery/cluster_ordering)

OWNER(
    radix
    g:kikimr
    g:logbroker 
)

SRCS(
    weighed_ordering_ut.cpp
)

PEERDIR(
    ydb/services/persqueue_cluster_discovery/cluster_ordering
)

END()
