# The functionality here is based on the work by Ryan Pavlik:
# https://github.com/rpavlik/cmake-modules/blob/main/GetGitRevisionDescription.cmake

if(__get_git_version)
  return()
endif()
set(__get_git_version YES)

# We must run the following at "include" time, not at function call time, to
# find the path to this module rather than the path to a calling list file
get_filename_component(_gitversionmoddir ${CMAKE_CURRENT_LIST_FILE} PATH)

# Function _git_find_closest_git_dir finds the next closest .git directory that
# is part of any directory in the path defined by _start_dir. The result is
# returned in the parent scope variable whose name is passed as variable
# _git_dir_var. If no .git directory can be found, the function returns an empty
# string via _git_dir_var.
#
# Example: Given a path C:/bla/foo/bar and assuming C:/bla/.git exists and
# neither foo nor bar contain a file/directory .git. This wil return C:/bla/.git
#
function(_git_find_closest_git_dir _start_dir _git_dir_var)
  set(cur_dir "${_start_dir}")
  set(git_dir "${_start_dir}/.git")
  while(NOT EXISTS "${git_dir}")
    # .git dir not found, search parent directories
    set(git_previous_parent "${cur_dir}")
    get_filename_component(cur_dir "${cur_dir}" DIRECTORY)
    if(cur_dir STREQUAL git_previous_parent)
      # We have reached the root directory, we are not in git
      set(${_git_dir_var}
          ""
          PARENT_SCOPE)
      return()
    endif()
    set(git_dir "${cur_dir}/.git")
  endwhile()
  set(${_git_dir_var}
      "${git_dir}"
      PARENT_SCOPE)
endfunction()

function(get_git_head_revision _refspecvar _hashvar)
  _git_find_closest_git_dir("${CMAKE_CURRENT_SOURCE_DIR}" GIT_DIR)

  if(NOT "${GIT_DIR}" STREQUAL "")
    file(RELATIVE_PATH _relative_to_source_dir "${CMAKE_SOURCE_DIR}"
         "${GIT_DIR}")
    if("${_relative_to_source_dir}" MATCHES "[.][.]")
      # We've gone above the CMake root dir.
      set(GIT_DIR "")
    endif()
  endif()
  if("${GIT_DIR}" STREQUAL "")
    set(${_refspecvar}
        "GITDIR-NOTFOUND"
        PARENT_SCOPE)
    set(${_hashvar}
        "GITDIR-NOTFOUND"
        PARENT_SCOPE)
    return()
  endif()

  # Check if the current source dir is a git submodule or a worktree. In both
  # cases .git is a file instead of a directory.
  #
  if(NOT IS_DIRECTORY ${GIT_DIR})
    # The following git command will return a non empty string that points to
    # the super project working tree if the current source dir is inside a git
    # submodule. Otherwise the command will return an empty string.
    #
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --show-superproject-working-tree
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE out
      ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT "${out}" STREQUAL "")
      # If out is empty, GIT_DIR/CMAKE_CURRENT_SOURCE_DIR is in a submodule
      file(READ ${GIT_DIR} submodule)
      string(REGEX REPLACE "gitdir: (.*)$" "\\1" GIT_DIR_RELATIVE ${submodule})
      string(STRIP ${GIT_DIR_RELATIVE} GIT_DIR_RELATIVE)
      get_filename_component(SUBMODULE_DIR ${GIT_DIR} PATH)
      get_filename_component(GIT_DIR ${SUBMODULE_DIR}/${GIT_DIR_RELATIVE}
                             ABSOLUTE)
      set(HEAD_SOURCE_FILE "${GIT_DIR}/HEAD")
    else()
      # GIT_DIR/CMAKE_CURRENT_SOURCE_DIR is in a worktree
      file(READ ${GIT_DIR} worktree_ref)
      # The .git directory contains a path to the worktree information directory
      # inside the parent git repo of the worktree.
      #
      string(REGEX REPLACE "gitdir: (.*)$" "\\1" git_worktree_dir
                           ${worktree_ref})
      string(STRIP ${git_worktree_dir} git_worktree_dir)
      _git_find_closest_git_dir("${git_worktree_dir}" GIT_DIR)
      set(HEAD_SOURCE_FILE "${git_worktree_dir}/HEAD")
    endif()
  else()
    set(HEAD_SOURCE_FILE "${GIT_DIR}/HEAD")
  endif()
  set(GIT_DATA "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/git-data")
  if(NOT EXISTS "${GIT_DATA}")
    file(MAKE_DIRECTORY "${GIT_DATA}")
  endif()

  if(NOT EXISTS "${HEAD_SOURCE_FILE}")
    return()
  endif()
  set(HEAD_FILE "${GIT_DATA}/HEAD")
  configure_file("${HEAD_SOURCE_FILE}" "${HEAD_FILE}" COPYONLY)

  configure_file("${_gitversionmoddir}/gitversion.cmake.in"
                 "${GIT_DATA}/grabRef.cmake" @ONLY)
  include("${GIT_DATA}/grabRef.cmake")

  set(${_refspecvar}
      "${HEAD_REF}"
      PARENT_SCOPE)
  set(${_hashvar}
      "${HEAD_HASH}"
      PARENT_SCOPE)
endfunction()

function(get_git_version _major _minor _patch _hash _string)
  if(NOT GIT_FOUND)
    find_package(Git QUIET)
  endif()
  get_git_head_revision(refspec hash)

  set(${_major}
      0
      PARENT_SCOPE)
  set(${_minor}
      0
      PARENT_SCOPE)
  set(${_patch}
      0
      PARENT_SCOPE)
  set(${_hash}
      0
      PARENT_SCOPE)
  set(${_string}
      0.0.0
      PARENT_SCOPE)

  if(NOT GIT_FOUND OR NOT hash)
    return()
  endif()

  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE _version_string
    ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)

  string(REGEX REPLACE "^v?([0-9]+).*" "\\1" local_major "${_version_string}")
  string(REGEX REPLACE "^v?[0-9]+\\.([0-9]+).*" "\\1" local_minor
                       "${_version_string}")

  if(${_version_string} MATCHES ".+-([0-9]+-g[0-9a-z]+).*")
    string(REGEX REPLACE "^v?[0-9]+\\.[0-9]+-([0-9]+).*" "\\1" local_patch
                         "${_version_string}")
    string(REGEX REPLACE "^v?[0-9]+\\.[0-9]+-[0-9]+-g([0-9a-z]+).*" "\\1"
                         local_hash "${_version_string}")
  else()
    set(local_patch 0)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE local_hash
      ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()

  set(${_major}
      ${local_major}
      PARENT_SCOPE)
  set(${_minor}
      ${local_minor}
      PARENT_SCOPE)
  set(${_patch}
      ${local_patch}
      PARENT_SCOPE)
  set(${_hash}
      ${local_hash}
      PARENT_SCOPE)
  set(${_string}
      ${local_major}.${local_minor}.${local_patch}
      PARENT_SCOPE)

  message(
    STATUS
      "project version from git: ${local_major}.${local_minor}.${local_patch}+${local_hash}"
  )
endfunction()
