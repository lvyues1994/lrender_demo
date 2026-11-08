#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <QApplication>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    doctest::Context context(argc, argv);
    return context.run();
}
