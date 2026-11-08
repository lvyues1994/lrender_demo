#include <QApplication>
#include <lqtexec/EventLoopContext.h>
#include <lr/runtime/Runtime.h>
#include <lr/render/Renderer.h>
#include <iostream>
#include <stdexcept>
int main(int argc, char **argv) {
    try {
        QApplication app(argc, argv);
        lqtexec::EventLoopContext gui;
        auto runtime = lr::runtime::makeRuntime({});
        lr::render::RendererConfig config;
        config.backend = "cpu";
        config.runtime = runtime.get();
        auto renderer = lr::render::makeRenderer(config);
        if (renderer->deviceInfo().backend != "cpu") {
            throw std::runtime_error("CPU backend was not selected");
        }
        std::cout << "Qt/lqtexec/lrender CPU link smoke passed\n";
        // Standalone link smoke only: product GUI cleanup must be asynchronous (P2/P11).
        return 0;
    } catch (std::exception const &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
