cmake_minimum_required(VERSION 3.22)
project(bridge_host_tests C)
enable_testing()
add_executable(bridge_tests test_bridge.c
    ../Core/Src/bridge_protocol.c ../Core/Src/bridge.c
    ../Core/Src/socket.c ../Core/Src/w5500.c ../Core/Src/wizchip_conf.c)
target_include_directories(bridge_tests PRIVATE fakes ../Core/Inc)
target_compile_features(bridge_tests PRIVATE c_std_11)
target_compile_options(bridge_tests PRIVATE -Wall -Wextra -g -O1 -UNDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
target_link_options(bridge_tests PRIVATE -fsanitize=address,undefined -no-pie)
# Do not interpose the host C library's networking/file-descriptor functions.
target_compile_definitions(bridge_tests PRIVATE
    socket=wiz_socket close=wiz_close listen=wiz_listen connect=wiz_connect
    disconnect=wiz_disconnect send=wiz_send recv=wiz_recv sendto=wiz_sendto
    recvfrom=wiz_recvfrom getsockopt=wiz_getsockopt setsockopt=wiz_setsockopt)
add_test(NAME bridge_faults_and_protocol COMMAND bridge_tests)
set_tests_properties(bridge_faults_and_protocol PROPERTIES TIMEOUT 30)
