qt_add_executable(space-community-test
    SpaceCommunityTest.cpp
    ../src/spaces/SpaceManager.h ../src/spaces/SpaceManager.cpp
    ../src/matrix/MatrixClient.h ../src/matrix/MatrixClient.cpp)
target_include_directories(space-community-test PRIVATE ../src)
target_link_libraries(space-community-test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME space-community COMMAND space-community-test)

qt_add_executable(space-community-qml-test SpaceCommunityQmlTest.cpp)
lightning_configure_qml_test(space-community-qml-test)
target_link_libraries(space-community-qml-test PRIVATE Qt6::Test)
target_include_directories(space-community-qml-test PRIVATE ../src)
add_test(NAME space-community-qml COMMAND space-community-qml-test)
set_tests_properties(space-community-qml PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LIGHTNING_DISABLE_MPRIS=1")
