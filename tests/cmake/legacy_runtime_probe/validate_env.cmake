foreach(required
        "C:/olr-probe/qt/bin"
        "C:/olr-probe/ffmpeg/bin"
        "C:/olr-probe/srt/bin"
        "OLR_PATH_SENTINEL")
    string(FIND "$ENV{PATH}" "${required}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR "PATH lacks ${required}: $ENV{PATH}")
    endif()
endforeach()

string(FIND "$ENV{PATH}" "C:/olr-probe/qt/bin" qt_path_offset)
string(FIND "$ENV{PATH}" "C:/olr-probe/ffmpeg/bin" ffmpeg_path_offset)
string(FIND "$ENV{PATH}" "C:/olr-probe/srt/bin" srt_path_offset)
string(FIND "$ENV{PATH}" "OLR_PATH_SENTINEL" inherited_path_offset)
if(NOT qt_path_offset EQUAL 0 OR
   NOT qt_path_offset LESS ffmpeg_path_offset OR
   NOT ffmpeg_path_offset LESS srt_path_offset OR
   NOT srt_path_offset LESS inherited_path_offset)
    message(FATAL_ERROR "controlled runtime PATH ordering is wrong: $ENV{PATH}")
endif()

string(FIND "$ENV{QT_PLUGIN_PATH}" "C:/olr-probe/qt/plugins" qt_plugin_offset)
string(FIND "$ENV{QT_PLUGIN_PATH}" "OLR_PLUGIN_SENTINEL" inherited_plugin_offset)
if(NOT qt_plugin_offset EQUAL 0 OR NOT qt_plugin_offset LESS inherited_plugin_offset)
    message(FATAL_ERROR "controlled Qt plugin directory is not first: $ENV{QT_PLUGIN_PATH}")
endif()

if(NOT "$ENV{OLR_EXISTING}" STREQUAL "kept")
    message(FATAL_ERROR "later custom test environment was not retained")
endif()
