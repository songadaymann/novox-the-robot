# Applies PATCH to the git checkout in the working directory, once.
#   cmake -DPATCH=<file> -P git-apply-once.cmake
# FetchContent re-runs a dependency's patch step after every update check (each configure), so a
# patch that is already applied must be left alone rather than failing the configure.
find_program(GIT git REQUIRED)
if(NOT EXISTS "${PATCH}")
  message(FATAL_ERROR "No such patch: ${PATCH}")
endif()
execute_process(COMMAND "${GIT}" apply --reverse --check "${PATCH}"
                RESULT_VARIABLE not_applied OUTPUT_QUIET ERROR_QUIET)
if(not_applied)
  execute_process(COMMAND "${GIT}" apply "${PATCH}" RESULT_VARIABLE failed)
  if(failed)
    message(FATAL_ERROR "Could not apply ${PATCH}. After changing the dependency's GIT_TAG, "
                        "refresh the patch for the new sources, or delete its _deps/*-src folder.")
  endif()
  message(STATUS "Applied ${PATCH}")
endif()
