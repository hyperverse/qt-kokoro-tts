// SPDX-License-Identifier: MIT
#include "kokoroplugin.h"

#include "kokoroengine.h"

QTextToSpeechEngine *KokoroPlugin::createTextToSpeechEngine(const QVariantMap &parameters,
                                                            QObject *parent,
                                                            QString *errorString) const
{
    auto *engine = new KokoroEngine(parameters, parent);
    if (engine->state() == QTextToSpeech::Error) {
        if (errorString)
            *errorString = engine->errorString();
        delete engine;
        return nullptr;
    }
    return engine;
}
