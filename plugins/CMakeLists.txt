cmake_minimum_required(VERSION 3.25)

add_subdirectory(chrono-kvs)

# TODO(#517): Re-enable ChronoStream plugin build/install once the plugin is cleaned up
# and properly integrated/packaged. It is temporarily disabled to reduce noise and
# maintenance burden in the build + install flow.
# add_subdirectory(chrono-stream)