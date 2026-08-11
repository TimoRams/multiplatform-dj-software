#include "controllers/midi/ParameterStore.h"

#include <iostream>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main()
{
    bool ok = true;
    ParameterStore store;

    ok &= require(store.getParameter(QStringLiteral("deckA_vol")) == 1.0f
                  && store.getParameter(QStringLiteral("deckB_vol")) == 1.0f,
                  "channel faders default to unity before passive hardware moves");
    ok &= require(store.getParameter(QStringLiteral("deckA_eqHigh")) == 0.5f
                  && store.getParameter(QStringLiteral("deckB_eqLow")) == 0.5f,
                  "bipolar mixer controls default to their normalized centre");

    int changes = 0;
    QString changedId;
    float changedValue = -1.0f;
    QObject::connect(&store, &ParameterStore::parameterChanged,
                     [&changes, &changedId, &changedValue](const QString& id, float value)
    {
        ++changes;
        changedId = id;
        changedValue = value;
    });

    store.setParameter(QStringLiteral("new_zero_control"), 0.0f);
    ok &= require(changes == 1
                  && changedId == QStringLiteral("new_zero_control")
                  && changedValue == 0.0f,
                  "the first real zero-valued hardware event is emitted");
    store.setParameter(QStringLiteral("new_zero_control"), 0.0f);
    ok &= require(changes == 1, "an unchanged established value remains deduplicated");

    return ok ? 0 : 1;
}
