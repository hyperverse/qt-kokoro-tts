// SPDX-License-Identifier: MIT
#ifndef KOKOROENGINE_H
#define KOKOROENGINE_H

#include <QtTextToSpeech/QTextToSpeechEngine>

#include <QAudioFormat>
#include <QList>
#include <QLocale>
#include <QString>
#include <QUrl>
#include <QVariantMap>

QT_FORWARD_DECLARE_CLASS(QAudioSink)
QT_FORWARD_DECLARE_CLASS(QIODevice)
QT_FORWARD_DECLARE_CLASS(QNetworkAccessManager)
QT_FORWARD_DECLARE_CLASS(QNetworkReply)
QT_FORWARD_DECLARE_CLASS(QTimer)

// Speaks through an OpenAI-compatible /v1/audio/speech endpoint backed by
// Kokoro (the `koko` server, or a Lemonade instance on the LAN).
//
// The utterance is split into sentence-sized chunks that are fetched ahead of
// playback, so synthesis of chunk N+1 overlaps playback of chunk N. Word
// boundaries are estimated from each chunk's measured audio duration, which is
// what lets the engine advertise WordByWordProgress.
class KokoroEngine : public QTextToSpeechEngine
{
    Q_OBJECT

public:
    explicit KokoroEngine(const QVariantMap &parameters, QObject *parent = nullptr);
    ~KokoroEngine() override;

    QTextToSpeech::Capabilities capabilities() const override;
    QList<QLocale> availableLocales() const override;
    QList<QVoice> availableVoices() const override;

    void say(const QString &text) override;
    void synthesize(const QString &text) override;
    void stop(QTextToSpeech::BoundaryHint boundaryHint) override;
    void pause(QTextToSpeech::BoundaryHint boundaryHint) override;
    void resume() override;

    double rate() const override;
    bool setRate(double rate) override;
    double pitch() const override;
    bool setPitch(double pitch) override;
    QLocale locale() const override;
    bool setLocale(const QLocale &locale) override;
    double volume() const override;
    bool setVolume(double volume) override;
    QVoice voice() const override;
    bool setVoice(const QVoice &voice) override;
    QTextToSpeech::State state() const override;
    QTextToSpeech::ErrorReason errorReason() const override;
    QString errorString() const override;

private:
    struct Chunk {
        QString text;
        qsizetype offset = 0;   // character offset into the full utterance
        QByteArray audio;       // decoded samples, in m_format
        bool requested = false;
        bool received = false;
    };

    struct WordSpan {
        QString word;
        qsizetype start = 0;    // character offset into the full utterance
        qsizetype length = 0;
        qint64 atUs = -1;       // playback position, assigned once its chunk is queued
        bool announced = false;
    };

    void beginUtterance(const QString &text, bool speak);
    void requestChunk(int index);
    void onChunkReceived(int index, const QByteArray &payload);
    void queueReadyChunks();
    void writeToSink();
    void tick();
    void finishUtterance();
    void reset();
    void setState(QTextToSpeech::State state);
    void fail(QTextToSpeech::ErrorReason reason, const QString &message);

    static QList<Chunk> splitIntoChunks(const QString &text);
    static QList<WordSpan> splitIntoWords(const QString &text);
    // Parses a RIFF/WAVE payload into raw samples plus their format.
    static bool parseWav(const QByteArray &wav, QAudioFormat *format, QByteArray *samples);

    QNetworkAccessManager *m_net = nullptr;
    QAudioSink *m_sink = nullptr;
    QIODevice *m_sinkDevice = nullptr;
    QTimer *m_ticker = nullptr;

    QUrl m_endpoint;
    QString m_model;
    QString m_apiKey;
    int m_lookahead = 2;

    QAudioFormat m_format;
    QString m_text;
    QList<Chunk> m_chunks;
    QList<WordSpan> m_words;
    int m_nextToQueue = 0;      // first chunk not yet handed to the sink
    int m_nextWord = 0;
    qint64 m_queuedUs = 0;      // total audio duration handed to the sink
    QByteArray m_pending;       // written to the sink as buffer space frees up
    bool m_speaking = false;    // false while serving synthesize()
    bool m_synthesizing = false;

    QList<QLocale> m_locales;
    QList<QVoice> m_voices;
    QVoice m_voice;
    QLocale m_locale;
    double m_rate = 0.0;
    double m_pitch = 0.0;
    double m_volume = 1.0;

    QTextToSpeech::State m_state = QTextToSpeech::Error;
    QTextToSpeech::ErrorReason m_errorReason = QTextToSpeech::ErrorReason::NoError;
    QString m_errorString;
};

#endif // KOKOROENGINE_H
