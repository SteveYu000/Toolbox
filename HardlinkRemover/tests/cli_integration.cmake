if(NOT DEFINED CLI_EXECUTABLE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "缺少 CLI_EXECUTABLE 或 TEST_ROOT 测试参数。")
endif()

# 此目录固定在 CMake 二进制目录下，仅存放本测试创建的临时文件。
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

function(fail_test message_text)
    file(REMOVE_RECURSE "${TEST_ROOT}")
    message(FATAL_ERROR "${message_text}")
endfunction()

set(source_file "${TEST_ROOT}/source.txt")
set(first_link "${TEST_ROOT}/first-link.txt")
set(second_link "${TEST_ROOT}/second-link.txt")
set(other_source "${TEST_ROOT}/other-source.txt")
set(other_link "${TEST_ROOT}/other-link.txt")
set(selection_input "${TEST_ROOT}/selection.txt")

execute_process(
    COMMAND "${CLI_EXECUTABLE}" --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_output
    ERROR_VARIABLE help_error
)
if(NOT help_result EQUAL 0)
    fail_test("--help 执行失败：${help_error}")
endif()
string(FIND "${help_output}" "用法：" chinese_help_position)
string(FIND "${help_output}" "Usage:" english_help_position)
if(chinese_help_position EQUAL -1 AND english_help_position EQUAL -1)
    fail_test("--help 没有根据语言环境输出中文或英文说明。")
endif()

file(WRITE "${source_file}" "HardLinkRemover CLI integration test")
file(CREATE_LINK "${source_file}" "${first_link}" RESULT first_link_result)
if(NOT first_link_result STREQUAL "0")
    fail_test("无法创建第一个测试硬链接：${first_link_result}")
endif()
file(CREATE_LINK "${source_file}" "${second_link}" RESULT second_link_result)
if(NOT second_link_result STREQUAL "0")
    fail_test("无法创建第二个测试硬链接：${second_link_result}")
endif()

file(WRITE "${other_source}" "HardLinkRemover second hard-link group")
file(CREATE_LINK "${other_source}" "${other_link}" RESULT other_link_result)
if(NOT other_link_result STREQUAL "0")
    fail_test("无法创建另一组测试硬链接：${other_link_result}")
endif()

execute_process(
    COMMAND "${CLI_EXECUTABLE}" list "${source_file}" "${first_link}" "${other_source}"
    RESULT_VARIABLE list_result
    OUTPUT_VARIABLE list_output
    ERROR_VARIABLE list_error
)
if(NOT list_result EQUAL 0)
    fail_test("list 子命令失败：${list_error}")
endif()
string(FIND "${list_output}" "first-link.txt" first_link_position)
string(FIND "${list_output}" "second-link.txt" second_link_position)
string(FIND "${list_output}" "other-link.txt" other_link_position)
if(first_link_position EQUAL -1 OR second_link_position EQUAL -1 OR other_link_position EQUAL -1)
    fail_test("list 子命令没有输出完整的硬链接列表。")
endif()
string(REGEX MATCHALL "(硬链接组|Hard-link group) #[0-9]+" group_markers "${list_output}")
list(LENGTH group_markers group_marker_count)
if(NOT group_marker_count EQUAL 2)
    fail_test("list 子命令没有用两个独立标识显示两组硬链接，或没有去除重复组。")
endif()

file(WRITE "${selection_input}" "q\n")
execute_process(
    COMMAND "${CLI_EXECUTABLE}" select "${source_file}" "${other_source}"
    INPUT_FILE "${selection_input}"
    RESULT_VARIABLE select_result
    OUTPUT_VARIABLE select_output
    ERROR_VARIABLE select_error
)
if(NOT select_result EQUAL 0)
    fail_test("select 子命令分组展示失败：${select_error}")
endif()
string(REGEX MATCHALL "(硬链接组|Hard-link group) #[0-9]+" select_group_markers "${select_output}")
list(LENGTH select_group_markers select_group_count)
string(FIND "${select_output}" "[5]" fifth_path_position)
if(NOT select_group_count EQUAL 2 OR fifth_path_position EQUAL -1)
    fail_test("select 子命令没有分组显示路径，或跨组编号不连续。")
endif()

execute_process(
    COMMAND "${CLI_EXECUTABLE}" delete --yes "${first_link}" "${second_link}"
    RESULT_VARIABLE delete_result
    OUTPUT_VARIABLE delete_output
    ERROR_VARIABLE delete_error
)
if(NOT delete_result EQUAL 0)
    fail_test("delete 子命令失败：${delete_error}")
endif()
if(EXISTS "${first_link}" OR EXISTS "${second_link}" OR NOT EXISTS "${source_file}")
    fail_test("delete 子命令没有只删除明确指定的两个路径。")
endif()

execute_process(
    COMMAND "${CLI_EXECUTABLE}" delete --yes "${source_file}"
    RESULT_VARIABLE protected_delete_result
    OUTPUT_QUIET
    ERROR_QUIET
)
if(protected_delete_result EQUAL 0 OR NOT EXISTS "${source_file}")
    fail_test("CLI 未能保护普通单链接文件。")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
