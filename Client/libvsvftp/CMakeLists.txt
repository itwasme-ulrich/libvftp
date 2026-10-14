# vs_vftp: the libvftp link over a CSP connection.
add_library(vs_vftp STATIC src/vs_vftp_link.c)
add_library(vsclient::vftp ALIAS vs_vftp)
target_include_directories(vs_vftp PUBLIC include)
target_compile_options(vs_vftp PRIVATE ${VS_WARN})
target_link_libraries(vs_vftp PUBLIC vs_csp vftp_client)

# vs_vftp_cmd: the `fburst` and `fpoll` commands.
add_library(vs_vftp_cmd STATIC cmd/vs_vftp_cmd.c)
add_library(vsclient::vftp_cmd ALIAS vs_vftp_cmd)
target_compile_options(vs_vftp_cmd PRIVATE ${VS_WARN})
target_link_libraries(vs_vftp_cmd PUBLIC vs_vftp vcli)
