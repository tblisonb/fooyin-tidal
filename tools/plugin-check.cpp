/*
 * Loads a fooyin plugin the way fooyin does and reports whether it would load, without starting
 * fooyin. scripts/check.sh runs it inside fooyin's Flatpak, so every symbol the plugin needs is
 * resolved against the fooyin libraries that are actually installed.
 *
 *   plugin-check <plugin.so>
 */

#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QPluginLoader>

#include <cstdio>

int main(int argc, char** argv)
{
    const QCoreApplication app{argc, argv};
    if(argc != 2) {
        std::fprintf(stderr, "usage: plugin-check <plugin.so>\n");
        return 2;
    }

    QPluginLoader loader{QString::fromLocal8Bit(argv[1])};
    loader.setLoadHints(QLibrary::ResolveAllSymbolsHint);

    const QString iid = loader.metaData().value(u"IID").toString();
    if(iid != QLatin1StringView{FOOYIN_PLUGIN_IID}) {
        std::fprintf(stderr, "FAIL: plugin API '%s', fooyin expects '%s'\n", qPrintable(iid), FOOYIN_PLUGIN_IID);
        return 1;
    }

    QObject* root = loader.instance();
    if(!root) {
        std::fprintf(stderr, "FAIL: %s\n", qPrintable(loader.errorString()));
        return 1;
    }
    if(!qobject_cast<Fooyin::Plugin*>(root)) {
        std::fprintf(stderr, "FAIL: does not subclass Fooyin::Plugin\n");
        return 1;
    }

    const auto metadata = loader.metaData().value(u"MetaData").toObject();
    std::printf("OK: %s %s\n", qPrintable(metadata.value(u"Name").toString()),
                qPrintable(metadata.value(u"Version").toString()));
    std::printf("  core plugin: %s\n", qobject_cast<Fooyin::CorePlugin*>(root) ? "yes" : "no");
    return 0;
}
