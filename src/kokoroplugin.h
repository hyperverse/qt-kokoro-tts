// SPDX-License-Identifier: MIT
#ifndef KOKOROPLUGIN_H
#define KOKOROPLUGIN_H

#include <QtTextToSpeech/QTextToSpeechPlugin>

#include <QObject>

class KokoroPlugin : public QObject, public QTextToSpeechPlugin
{
    Q_OBJECT
    Q_INTERFACES(QTextToSpeechPlugin)
// Qt hardcodes "speechd" as the default engine on Unix and QTextToSpeech's
// default constructor offers no way to override it. Applications that always
// ask for the default engine (sioyek among them) can therefore only reach this
// plugin if it claims that key -- see KOKORO_TAKEOVER_SPEECHD in the README.
#ifdef KOKORO_TAKEOVER_SPEECHD
    Q_PLUGIN_METADATA(IID "org.qt-project.qt.speech.tts.plugin/6.0" FILE "speechd_plugin.json")
#else
    Q_PLUGIN_METADATA(IID "org.qt-project.qt.speech.tts.plugin/6.0" FILE "kokoro_plugin.json")
#endif

public:
    QTextToSpeechEngine *createTextToSpeechEngine(const QVariantMap &parameters,
                                                  QObject *parent,
                                                  QString *errorString) const override;
};

#endif // KOKOROPLUGIN_H
