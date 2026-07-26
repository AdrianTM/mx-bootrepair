#include "cli/controller.h"

#include <QCoreApplication>
#include <QVariant>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setProperty("cliMode", QVariant(true));
    CliController controller;
    return controller.run();
}
