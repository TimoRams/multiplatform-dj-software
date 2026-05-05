#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_LEAK__)
#include <cstdlib>

namespace {
const char kLsanSuppressions[] =
    "leak:libfontconfig.so\n"
    "leak:libfontconfig.so.1\n"
    "leak:QFontconfigDatabase::fontEngineMulti\n"
    "leak:QFontEngineFT::loadGlyph\n"
    "leak:QTextEngine::shapeTextWithHarfbuzzNG\n"
    "leak:hb_shape_plan_create2\n"
    "leak:hb_buffer_create\n"
    "leak:QQmlDirParser::parse\n"
    "leak:QQmlTypeLoader::qmldirContent\n"
    "leak:QQmlTypeLoader::Blob::Blob\n"
    "leak:QQmlTypeNameCache::add\n"
    "leak:QQmlApplicationEnginePrivate::QQmlApplicationEnginePrivate\n"
    "leak:QQuickBehavior::QQuickBehavior\n"
    "leak:QQuickTextPrivate::updateSize\n"
    "leak:Kirigami::Platform::PlatformTheme::PlatformTheme\n"
    "leak:QMetaObjectBuilder::toMetaObject\n"
    "leak:unknown module\n";
}

extern "C" const char* __lsan_default_suppressions()
{
    const char* disable = std::getenv("RAMSBROCKDJ_LSAN_SUPPRESS");
    if (disable && disable[0] == '0')
        return "";
    return kLsanSuppressions;
}
#endif
