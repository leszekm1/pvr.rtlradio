if(FAAD2_HDC_ROOT)
  find_path(FAAD2_HDC_INCLUDE_DIRS NAMES neaacdec.h
            PATHS "${FAAD2_HDC_ROOT}/include"
            NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  find_library(FAAD2_HDC_LIBRARIES NAMES faad_hdc
               PATHS "${FAAD2_HDC_ROOT}/lib"
               NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
else()
  find_path(FAAD2_HDC_INCLUDE_DIRS NAMES neaacdec.h)
  find_library(FAAD2_HDC_LIBRARIES NAMES faad_hdc)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FAAD2_HDC
                                  REQUIRED_VARS FAAD2_HDC_LIBRARIES
                                                FAAD2_HDC_INCLUDE_DIRS)

mark_as_advanced(FAAD2_HDC_INCLUDE_DIRS FAAD2_HDC_LIBRARIES)
