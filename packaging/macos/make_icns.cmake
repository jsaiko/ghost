# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

# cmake -DINPUT=app-icon.png -DOUTPUT=spectre.icns -DWORK=<dir> -P make_icns.cmake
#
# Builds a macOS .icns from one large square PNG with the two tools every
# Mac ships: sips resizes into an .iconset directory holding the sizes
# iconutil expects, and iconutil packs that into the .icns.
foreach(var INPUT OUTPUT WORK)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "make_icns.cmake: ${var} is required")
  endif()
endforeach()

set(iconset "${WORK}/app.iconset")
file(REMOVE_RECURSE "${iconset}")
file(MAKE_DIRECTORY "${iconset}")
foreach(size 16 32 128 256 512)
  math(EXPR size2x "${size} * 2")
  foreach(pair "${size};icon_${size}x${size}.png" "${size2x};icon_${size}x${size}@2x.png")
    list(GET pair 0 px)
    list(GET pair 1 name)
    execute_process(
      COMMAND sips -z ${px} ${px} "${INPUT}" --out "${iconset}/${name}"
      OUTPUT_QUIET
      RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "make_icns.cmake: sips failed for ${name}")
    endif()
  endforeach()
endforeach()
execute_process(COMMAND iconutil -c icns "${iconset}" -o "${OUTPUT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "make_icns.cmake: iconutil failed")
endif()
