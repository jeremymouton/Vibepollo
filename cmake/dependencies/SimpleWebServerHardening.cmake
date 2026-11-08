include_guard(GLOBAL)

# Keep the vendored checkout untouched while applying the request-size fix to
# both HTTP and HTTPS (server_https.hpp includes its sibling server_http.hpp).
# Returns an include root containing Simple-Web-Server/*.hpp.
function(sunshine_prepare_simple_web_server repo_root output_root output_variable)
    get_filename_component(_source_dir "${repo_root}/third-party/Simple-Web-Server" ABSOLUTE)
    get_filename_component(_include_root "${output_root}" ABSOLUTE)
    set(_output_dir "${_include_root}/Simple-Web-Server")
    if(_output_dir STREQUAL _source_dir)
        message(FATAL_ERROR "Simple-Web-Server hardening output must not be the vendor source directory")
    endif()

    file(READ "${_source_dir}/server_http.hpp" _server_http)
    string(CONCAT _old_check
        "if(chunk_size + session->request->content_streambuf.size() > "
        "session->request->content_streambuf.max_size()) {")
    string(CONCAT _new_check
        "if(chunk_size > session->request->content_streambuf.max_size() - "
        "session->request->content_streambuf.size()) {")
    string(FIND "${_server_http}" "${_old_check}" _old_position)
    string(FIND "${_server_http}" "${_new_check}" _new_position)
    if(NOT _old_position EQUAL -1)
        string(REPLACE "${_old_check}" "${_new_check}" _server_http "${_server_http}")
    elseif(_new_position EQUAL -1)
        message(FATAL_ERROR
            "Simple-Web-Server chunk-size check has changed; review the hardening patch before building")
    endif()

    file(MAKE_DIRECTORY "${_output_dir}")
    file(GLOB _headers "${_source_dir}/*.hpp")
    foreach(_header IN LISTS _headers)
        get_filename_component(_name "${_header}" NAME)
        if(_name STREQUAL "server_http.hpp")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_header}")
            # Substitution occurs once, preserving any C++ tokens in the source.
            # file(CONFIGURE) also avoids rebuilding when content is unchanged.
            file(CONFIGURE OUTPUT "${_output_dir}/${_name}" CONTENT "@_server_http@" @ONLY)
        else()
            configure_file("${_header}" "${_output_dir}/${_name}" COPYONLY)
        endif()
    endforeach()
    set(${output_variable} "${_include_root}" PARENT_SCOPE)
endfunction()
