#include <doctest/doctest.h>
#include <QApplication>
#include <lqtexec/EventLoopContext.h>
#include <lexec/execution.hpp>
#ifndef __cpp_exceptions
#error "The demo requires C++ exceptions"
#endif
TEST_CASE("link/qt-event-loop-host-exists") {
    CHECK(QApplication::instance() != nullptr);
    lqtexec::EventLoopContext gui;
    CHECK(gui.thread() == QApplication::instance()->thread());
}
TEST_CASE("link/unified-lexec-value-sender") {
    auto result = lexec::sync_wait(lexec::just(42));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) == 42);
}
