// SPDX-License-Identifier: MIT
//
// Smoke test for the kokoro engine: speaks a passage and logs the word
// boundaries as they are reported, which is what drives sioyek's auto-scroll.
//
//   kokoro-speak [--voice Michael] [--rate 0.2] [text...]

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QTextToSpeech>
#include <QTextStream>

using namespace Qt::StringLiterals;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    QCommandLineParser parser;
    parser.setApplicationDescription(u"Speak text through the Kokoro TTS plugin"_s);
    parser.addHelpOption();
    QCommandLineOption voiceOpt({u"voice"_s, u"v"_s}, u"Voice name, e.g. Heart"_s, u"name"_s);
    QCommandLineOption rateOpt({u"rate"_s, u"r"_s}, u"Rate, -1.0 to 1.0"_s, u"rate"_s, u"0"_s);
    parser.addOption(voiceOpt);
    parser.addOption(rateOpt);
    parser.addPositionalArgument(u"text"_s, u"Text to speak"_s);
    parser.process(app);

    QString text = parser.positionalArguments().join(u' ');
    if (text.isEmpty()) {
        text = u"For as long as anyone in the village could remember, the lighthouse had "
               "been keeping its own hours. It woke when the fog came in, and it slept "
               "through clear nights without apology."_s;
    }

    QTextToSpeech tts(u"kokoro"_s);
    if (tts.state() == QTextToSpeech::Error) {
        out << "engine error: " << tts.errorString() << "\n";
        return 1;
    }

    if (parser.isSet(voiceOpt)) {
        const QString wanted = parser.value(voiceOpt);
        bool found = false;
        for (const QVoice &v : tts.availableVoices()) {
            if (v.name().compare(wanted, Qt::CaseInsensitive) == 0) {
                tts.setVoice(v);
                found = true;
                break;
            }
        }
        if (!found) {
            out << "unknown voice: " << wanted << "\n";
            return 1;
        }
    }
    tts.setRate(parser.value(rateOpt).toDouble());

    out << "engine : " << tts.engine() << "\n"
        << "voice  : " << tts.voice().name() << "\n"
        << "words  : " << text.split(u' ', Qt::SkipEmptyParts).size() << "\n\n";
    out.flush();

    auto *timer = new QElapsedTimer;
    int wordCount = 0;

    QObject::connect(&tts, &QTextToSpeech::sayingWord, &app,
                     [&](const QString &word, qsizetype, qsizetype start, qsizetype length) {
        ++wordCount;
        out << QStringLiteral("%1  %2  start=%3 len=%4\n")
                   .arg(timer->elapsed() / 1000.0, 7, u'f', 2)
                   .arg(word, -16)
                   .arg(start).arg(length);
        out.flush();
    });

    QObject::connect(&tts, &QTextToSpeech::stateChanged, &app, [&](QTextToSpeech::State state) {
        if (state == QTextToSpeech::Error) {
            out << "error: " << tts.errorString() << "\n";
            app.exit(1);
        } else if (state == QTextToSpeech::Ready && timer->isValid()) {
            out << "\ndone: " << wordCount << " word events in "
                << QString::number(timer->elapsed() / 1000.0, 'f', 2) << "s\n";
            app.quit();
        }
    });

    timer->start();
    tts.say(text);
    return app.exec();
}
