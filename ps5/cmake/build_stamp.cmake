# Dead Space 2 for PS5 - which commit a program was built from, written into
# ds2_build_stamp.h at every build (the app logs it at start: a console run's
# log then says which program it was). The header only changes, and the app
# only recompiles, when the commit does. "+" after the commit: files changed
# since it.
#   cmake -DDS2_REPO=<repo> -DDS2_STAMP=<header> -P build_stamp.cmake
execute_process(COMMAND git -C "${DS2_REPO}" rev-parse --short=7 HEAD
                OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(commit STREQUAL "")
  set(commit "unknown")
else()
  execute_process(COMMAND git -C "${DS2_REPO}" status --porcelain --untracked-files=no
                  OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT changes STREQUAL "")
    string(APPEND commit "+")
  endif()
endif()
set(text "#define DS2_PS5_BUILD \"${commit}\"\n")
if(EXISTS "${DS2_STAMP}")
  file(READ "${DS2_STAMP}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${DS2_STAMP}" "${text}")
endif()
