cmake_minimum_required(VERSION 3.16)
project(AegisyQt6ReleaseGateFixture LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Widgets
    Network
    Sql
    WebSockets
    WebChannel
    WebEngineWidgets)
