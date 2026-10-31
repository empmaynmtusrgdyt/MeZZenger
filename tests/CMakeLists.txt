# Тесты: быстрые C++-тесты (unit), интеграционные сценарии на Python (integration)
# и обвязка настоящего клиента для них (gui). Запуск: ctest --output-on-failure

set(TEST_INCLUDES ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/common ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/client)

function(mezzenger_unit_test name)
    add_executable(${name} unit/${name}.cpp ${ARGN})
    target_include_directories(${name} PRIVATE ${TEST_INCLUDES})
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

mezzenger_unit_test(test_rate_limiter)
mezzenger_unit_test(test_database_server ${CMAKE_SOURCE_DIR}/server/DatabaseServer.cpp ${CMAKE_SOURCE_DIR}/server/DatabaseServer.h)
mezzenger_unit_test(test_database_client ${CMAKE_SOURCE_DIR}/client/DatabaseClient.cpp ${CMAKE_SOURCE_DIR}/client/DatabaseClient.h)

# Настоящие окна клиента, управляемые скриптом (используются интеграционными тестами)
# Пути в CLIENT_SOURCES заданы от корня проекта, в подкаталоге их нужно сделать абсолютными
list(TRANSFORM CLIENT_SOURCES PREPEND ${CMAKE_SOURCE_DIR}/ OUTPUT_VARIABLE HARNESS_SOURCES)
list(FILTER HARNESS_SOURCES EXCLUDE REGEX "client/main\\.cpp$")
list(FILTER HARNESS_SOURCES EXCLUDE REGEX "\\.qss$")
add_executable(client_harness gui/client_harness.cpp ${HARNESS_SOURCES})
target_include_directories(client_harness PRIVATE ${TEST_INCLUDES})
target_link_libraries(client_harness PRIVATE Qt6::Core Qt6::Network Qt6::Widgets Qt6::Sql)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND)
    foreach(suite tls cli auth delivery presence files limits client_gui)
        add_test(NAME integration_${suite}
                 COMMAND ${Python3_EXECUTABLE} -m unittest -v test_${suite}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/integration)
        set_tests_properties(integration_${suite} PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "MEZZENGER_SERVER=$<TARGET_FILE:mezzenger_server>;MEZZENGER_HARNESS=$<TARGET_FILE:client_harness>;MEZZENGER_SOURCE_DIR=${CMAKE_SOURCE_DIR};QT_QPA_PLATFORM=offscreen")
    endforeach()
else()
    message(WARNING "Python3 не найден, интеграционные тесты не добавлены")
endif()
