# Naughty Dog engine module: Uncharted 4 (Legacy of Thieves Collection). The gyro turns the active camera control's own
# view (on foot and aiming), inside the game's camera update.
add_library(gyro_naughtydog OBJECT nd_engine.cpp)
target_include_directories(gyro_naughtydog PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_naughtydog PRIVATE gyro_options gyro_core)
