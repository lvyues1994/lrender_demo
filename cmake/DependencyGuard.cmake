# Revalidate pinned clean source checkouts on every build, not just configure.
if(NOT DEFINED DEMO_DEPENDENCY_ROOT)
 get_filename_component(DEMO_DEPENDENCY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
if(NOT DEFINED DEMO_DEPENDENCY_LOCK)
 set(DEMO_DEPENDENCY_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.json")
endif()
add_custom_target(demo_dependency_guard ALL
 COMMAND "${CMAKE_COMMAND}" "-DDEMO_DEPENDENCY_ROOT=${DEMO_DEPENDENCY_ROOT}"
 "-DDEMO_DEPENDENCY_LOCK=${DEMO_DEPENDENCY_LOCK}" -P "${CMAKE_CURRENT_LIST_DIR}/VerifyDependencies.cmake"
 COMMENT "Verifying locked SHA and clean local sources" VERBATIM)
