cmake_minimum_required(VERSION 3.20)

find_package(Qt6 REQUIRED COMPONENTS Quick Charts Core)

qt_standard_project_setup()

# ─── Backend C++ Models ──────────────────────────────────────────────────────
add_library(hft_gui_backend STATIC
    backend/order_book_model.cpp
    backend/latency_model.cpp
    backend/sweep_result_model.cpp
)

target_include_directories(hft_gui_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(hft_gui_backend PUBLIC
    Qt6::Quick
    Qt6::Charts
    Qt6::Core
    hft_core
)

# ─── QML Resources ──────────────────────────────────────────────────────────
qt_add_qml_module(hft_gui_backend
    URI HFTEngine
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/OrderBookView.qml
        qml/LatencyChart.qml
        qml/PnLChart.qml
        qml/SweepResultsTable.qml
)

# ─── GUI Executable ──────────────────────────────────────────────────────────
add_executable(hft-dashboard main_window.cpp)

target_link_libraries(hft-dashboard PRIVATE
    hft_gui_backend
    Qt6::Quick
    Qt6::Charts
    Qt6::Core
    hft_core
)
