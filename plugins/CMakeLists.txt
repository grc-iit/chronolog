cmake_minimum_required(VERSION 3.25)

add_subdirectory(chrono-kvs)
add_subdirectory(chrono-pubsub)
add_subdirectory(chrono-sql)

# LDMS ldmsd store plugin (store_chronolog). Self-gates: builds only when the
# OVIS/LDMS headers are found (set LDMS_PREFIX), otherwise it is skipped.
add_subdirectory(chrono-ldms)

# TODO(#517): Re-enable ChronoStream plugin build/install once the plugin is cleaned up
# and properly integrated/packaged. It is temporarily disabled to reduce noise and
# maintenance burden in the build + install flow.
# add_subdirectory(chrono-stream)
