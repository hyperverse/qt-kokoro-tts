// SPDX-License-Identifier: MIT
#include "kokoroengine.h"

#include <QAudioSink>
#include <QByteArray>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QtEndian>

#include <cmath>

using namespace Qt::StringLiterals;

namespace {

constexpr int kTickMs = 40;
constexpr qsizetype kMinChunkChars = 80;
constexpr qsizetype kMaxChunkChars = 350;
// Time to first audio is dominated by the opening chunk, so keep it short and
// let the later, larger chunks be synthesized while it plays.
constexpr qsizetype kFirstChunkChars = 120;

QString envOr(const char *key, const QString &fallback)
{
    const QByteArray value = qgetenv(key);
    return value.isEmpty() ? fallback : QString::fromLocal8Bit(value);
}

QString paramOr(const QVariantMap &params, const QString &key, const QString &fallback)
{
    const auto it = params.constFind(key);
    return it == params.constEnd() ? fallback : it->toString();
}

struct VoiceSpec {
    const char *id;
    const char *name;
    QVoice::Gender gender;
};

// Kokoro v1.0's American English voices. The en-gb voices are deliberately
// omitted: the upstream `koko` server phonemizes them via espeak-ng's "en-gb"
// code, which yields empty output and a ~0.3 s clip for every British voice.
constexpr VoiceSpec kVoices[] = {
    {"af_heart",   "Heart",   QVoice::Female},
    {"af_alloy",   "Alloy",   QVoice::Female},
    {"af_aoede",   "Aoede",   QVoice::Female},
    {"af_bella",   "Bella",   QVoice::Female},
    {"af_jessica", "Jessica", QVoice::Female},
    {"af_kore",    "Kore",    QVoice::Female},
    {"af_nicole",  "Nicole",  QVoice::Female},
    {"af_nova",    "Nova",    QVoice::Female},
    {"af_river",   "River",   QVoice::Female},
    {"af_sarah",   "Sarah",   QVoice::Female},
    {"af_sky",     "Sky",     QVoice::Female},
    {"am_adam",    "Adam",    QVoice::Male},
    {"am_echo",    "Echo",    QVoice::Male},
    {"am_eric",    "Eric",    QVoice::Male},
    {"am_fenrir",  "Fenrir",  QVoice::Male},
    {"am_liam",    "Liam",    QVoice::Male},
    {"am_michael", "Michael", QVoice::Male},
    {"am_onyx",    "Onyx",    QVoice::Male},
    {"am_puck",    "Puck",    QVoice::Male},
    {"am_santa",   "Santa",   QVoice::Male},
};

} // namespace

KokoroEngine::KokoroEngine(const QVariantMap &parameters, QObject *parent)
    : QTextToSpeechEngine(parent)
{
    const QString url = paramOr(parameters, u"url"_s,
                                envOr("QT_TTS_KOKORO_URL",
                                      u"http://127.0.0.1:8099/v1/audio/speech"_s));
    m_endpoint = QUrl(url);
    m_model = paramOr(parameters, u"model"_s, envOr("QT_TTS_KOKORO_MODEL", u"kokoro"_s));
    m_apiKey = paramOr(parameters, u"apiKey"_s, envOr("QT_TTS_KOKORO_API_KEY", QString()));

    m_locale = QLocale(QLocale::English, QLocale::UnitedStates);
    m_locales = {m_locale};

    const QString preferred = paramOr(parameters, u"voice"_s,
                                      envOr("QT_TTS_KOKORO_VOICE", u"af_heart"_s));
    for (const VoiceSpec &spec : kVoices) {
        const QVoice v = createVoice(QString::fromLatin1(spec.name), m_locale, spec.gender,
                                     QVoice::Adult, QString::fromLatin1(spec.id));
        m_voices.append(v);
        if (QString::fromLatin1(spec.id) == preferred)
            m_voice = v;
    }
    if (m_voice.name().isEmpty() && !m_voices.isEmpty())
        m_voice = m_voices.constFirst();

    m_net = new QNetworkAccessManager(this);

    m_ticker = new QTimer(this);
    m_ticker->setInterval(kTickMs);
    connect(m_ticker, &QTimer::timeout, this, &KokoroEngine::tick);

    if (!m_endpoint.isValid()) {
        m_state = QTextToSpeech::Error;
        m_errorReason = QTextToSpeech::ErrorReason::Configuration;
        m_errorString = u"Invalid Kokoro endpoint: "_s + url;
    } else {
        m_state = QTextToSpeech::Ready;
    }
}

KokoroEngine::~KokoroEngine()
{
    reset();
}

QTextToSpeech::Capabilities KokoroEngine::capabilities() const
{
    return QTextToSpeech::Capability::Speak
         | QTextToSpeech::Capability::PauseResume
         | QTextToSpeech::Capability::WordByWordProgress
         | QTextToSpeech::Capability::Synthesize;
}

QList<QLocale> KokoroEngine::availableLocales() const { return m_locales; }
QList<QVoice> KokoroEngine::availableVoices() const { return m_voices; }

// --- text segmentation ------------------------------------------------------

QList<KokoroEngine::Chunk> KokoroEngine::splitIntoChunks(const QString &text)
{
    QList<Chunk> chunks;
    qsizetype start = 0;

    while (start < text.size()) {
        while (start < text.size() && text.at(start).isSpace())
            ++start;
        if (start >= text.size())
            break;

        const qsizetype maxChars = chunks.isEmpty() ? kFirstChunkChars : kMaxChunkChars;
        const qsizetype minChars = qMin(kMinChunkChars, maxChars / 2);
        const qsizetype remaining = text.size() - start;
        qsizetype cut = -1;

        if (remaining <= maxChars) {
            cut = text.size();
        } else {
            // Prefer a sentence end that leaves a reasonably sized chunk.
            for (qsizetype i = start + minChars; i < start + maxChars; ++i) {
                const QChar c = text.at(i);
                if ((c == u'.' || c == u'!' || c == u'?' || c == u';')
                    && i + 1 < text.size() && text.at(i + 1).isSpace()) {
                    cut = i + 1;
                }
            }
            if (cut < 0) {   // no sentence end: fall back to the last word break
                for (qsizetype i = start + maxChars - 1; i > start + minChars; --i) {
                    if (text.at(i).isSpace()) {
                        cut = i;
                        break;
                    }
                }
            }
            if (cut < 0)
                cut = start + maxChars;
        }

        Chunk chunk;
        chunk.offset = start;
        chunk.text = text.mid(start, cut - start).trimmed();
        if (!chunk.text.isEmpty())
            chunks.append(chunk);
        start = cut;
    }

    return chunks;
}

QList<KokoroEngine::WordSpan> KokoroEngine::splitIntoWords(const QString &text)
{
    QList<WordSpan> words;
    qsizetype i = 0;
    while (i < text.size()) {
        while (i < text.size() && text.at(i).isSpace())
            ++i;
        const qsizetype begin = i;
        while (i < text.size() && !text.at(i).isSpace())
            ++i;
        if (i > begin) {
            WordSpan w;
            w.start = begin;
            w.length = i - begin;
            w.word = text.mid(begin, w.length);
            words.append(w);
        }
    }
    return words;
}

// --- WAV decoding -----------------------------------------------------------

bool KokoroEngine::parseWav(const QByteArray &wav, QAudioFormat *format, QByteArray *samples)
{
    if (wav.size() < 44 || !wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE")
        return false;

    quint16 audioFormat = 0, channels = 0, bitsPerSample = 0;
    quint32 sampleRate = 0;
    bool haveFmt = false;

    qsizetype pos = 12;
    while (pos + 8 <= wav.size()) {
        const QByteArray id = wav.mid(pos, 4);
        const quint32 size = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(wav.constData() + pos + 4));
        const qsizetype body = pos + 8;

        if (id == "fmt " && size >= 16 && body + 16 <= wav.size()) {
            const uchar *p = reinterpret_cast<const uchar *>(wav.constData() + body);
            audioFormat   = qFromLittleEndian<quint16>(p);
            channels      = qFromLittleEndian<quint16>(p + 2);
            sampleRate    = qFromLittleEndian<quint32>(p + 4);
            bitsPerSample = qFromLittleEndian<quint16>(p + 14);
            haveFmt = true;
        } else if (id == "data") {
            if (!haveFmt)
                return false;
            const qsizetype available = qMin<qsizetype>(size, wav.size() - body);
            *samples = wav.mid(body, available);

            QAudioFormat fmt;
            fmt.setSampleRate(int(sampleRate));
            fmt.setChannelCount(int(channels));
            if (audioFormat == 3 && bitsPerSample == 32)
                fmt.setSampleFormat(QAudioFormat::Float);
            else if (audioFormat == 1 && bitsPerSample == 16)
                fmt.setSampleFormat(QAudioFormat::Int16);
            else if (audioFormat == 1 && bitsPerSample == 8)
                fmt.setSampleFormat(QAudioFormat::UInt8);
            else if (audioFormat == 1 && bitsPerSample == 32)
                fmt.setSampleFormat(QAudioFormat::Int32);
            else
                return false;
            if (!fmt.isValid())
                return false;
            *format = fmt;
            return true;
        }
        pos = body + size + (size & 1);   // chunks are word-aligned
    }
    return false;
}

// --- utterance lifecycle ----------------------------------------------------

void KokoroEngine::beginUtterance(const QString &text, bool speak)
{
    reset();

    m_text = text;
    m_speaking = speak;
    m_synthesizing = !speak;
    m_chunks = splitIntoChunks(text);
    m_words = splitIntoWords(text);

    if (m_chunks.isEmpty()) {
        setState(QTextToSpeech::Ready);
        return;
    }

    setState(speak ? QTextToSpeech::Speaking : QTextToSpeech::Synthesizing);

    for (int i = 0; i < qMin(m_lookahead + 1, int(m_chunks.size())); ++i)
        requestChunk(i);
    m_ticker->start();
}

void KokoroEngine::say(const QString &text)
{
    beginUtterance(text, /*speak=*/true);
}

void KokoroEngine::synthesize(const QString &text)
{
    beginUtterance(text, /*speak=*/false);
}

void KokoroEngine::requestChunk(int index)
{
    if (index < 0 || index >= m_chunks.size() || m_chunks[index].requested)
        return;
    m_chunks[index].requested = true;

    QJsonObject body{
        {u"model"_s, m_model},
        {u"input"_s, m_chunks[index].text},
        {u"voice"_s, voiceData(m_voice).toString()},
        {u"speed"_s, std::pow(2.0, m_rate)},
        {u"response_format"_s, u"wav"_s},
    };

    QNetworkRequest request(m_endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    if (!m_apiKey.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());

    QNetworkReply *reply = m_net->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, index] {
        reply->deleteLater();
        if (m_state != QTextToSpeech::Speaking && m_state != QTextToSpeech::Synthesizing
            && m_state != QTextToSpeech::Paused) {
            return;   // utterance was stopped while in flight
        }
        if (reply->error() != QNetworkReply::NoError) {
            fail(QTextToSpeech::ErrorReason::Input, reply->errorString());
            return;
        }
        onChunkReceived(index, reply->readAll());
    });
}

void KokoroEngine::onChunkReceived(int index, const QByteArray &payload)
{
    if (index < 0 || index >= m_chunks.size())
        return;

    QAudioFormat format;
    QByteArray samples;
    if (!parseWav(payload, &format, &samples)) {
        fail(QTextToSpeech::ErrorReason::Input,
             u"Kokoro returned audio that could not be decoded"_s);
        return;
    }

    if (!m_format.isValid()) {
        m_format = format;
    } else if (m_format != format) {
        fail(QTextToSpeech::ErrorReason::Input,
             u"Kokoro changed audio format mid-utterance"_s);
        return;
    }

    m_chunks[index].audio = samples;
    m_chunks[index].received = true;
    queueReadyChunks();
}

void KokoroEngine::queueReadyChunks()
{
    while (m_nextToQueue < m_chunks.size() && m_chunks[m_nextToQueue].received) {
        const Chunk &chunk = m_chunks[m_nextToQueue];
        const qint64 durationUs = m_format.durationForBytes(chunk.audio.size());

        // Spread this chunk's words across its measured duration, weighted by
        // how far into the chunk each word starts.
        const qsizetype chunkChars = qMax<qsizetype>(1, chunk.text.size());
        const qsizetype chunkEnd = chunk.offset + chunk.text.size();
        for (WordSpan &w : m_words) {
            if (w.atUs >= 0 || w.start < chunk.offset || w.start >= chunkEnd)
                continue;
            const double frac = double(w.start - chunk.offset) / double(chunkChars);
            w.atUs = m_queuedUs + qint64(frac * double(durationUs));
        }

        if (m_synthesizing)
            emit synthesized(m_format, chunk.audio);
        else
            m_pending.append(chunk.audio);

        m_queuedUs += durationUs;
        m_chunks[m_nextToQueue].audio.clear();
        ++m_nextToQueue;
    }

    // Keep the lookahead window full.
    for (int i = m_nextToQueue; i < qMin(m_nextToQueue + m_lookahead + 1, int(m_chunks.size())); ++i)
        requestChunk(i);

    if (m_speaking)
        writeToSink();
}

void KokoroEngine::writeToSink()
{
    if (m_pending.isEmpty() || !m_format.isValid())
        return;

    if (!m_sink) {
        m_sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), m_format, this);
        m_sink->setVolume(m_volume);
        m_sinkDevice = m_sink->start();
        if (!m_sinkDevice) {
            fail(QTextToSpeech::ErrorReason::Playback, u"Could not open the audio output"_s);
            return;
        }
    }

    if (m_state == QTextToSpeech::Paused)
        return;

    const qsizetype writable = qMin<qsizetype>(m_pending.size(), m_sink->bytesFree());
    if (writable <= 0)
        return;
    const qint64 written = m_sinkDevice->write(m_pending.constData(), writable);
    if (written > 0)
        m_pending.remove(0, written);
}

void KokoroEngine::tick()
{
    if (m_state == QTextToSpeech::Paused)
        return;

    if (m_speaking) {
        writeToSink();

        const qint64 playedUs = m_sink ? m_sink->processedUSecs() : 0;
        while (m_nextWord < m_words.size()) {
            WordSpan &w = m_words[m_nextWord];
            if (w.atUs < 0 || w.atUs > playedUs)
                break;
            if (!w.announced) {
                w.announced = true;
                emit sayingWord(w.word, w.start, w.length);
            }
            ++m_nextWord;
        }

        const bool drained = m_nextToQueue >= m_chunks.size() && m_pending.isEmpty()
                          && playedUs >= m_queuedUs;
        if (drained)
            finishUtterance();
        return;
    }

    if (m_synthesizing && m_nextToQueue >= m_chunks.size())
        finishUtterance();
}

void KokoroEngine::finishUtterance()
{
    m_ticker->stop();
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
        m_sinkDevice = nullptr;
    }
    m_speaking = false;
    m_synthesizing = false;
    setState(QTextToSpeech::Ready);
}

void KokoroEngine::stop(QTextToSpeech::BoundaryHint)
{
    if (m_state == QTextToSpeech::Ready)
        return;
    reset();
    setState(QTextToSpeech::Ready);
}

void KokoroEngine::pause(QTextToSpeech::BoundaryHint)
{
    if (m_state != QTextToSpeech::Speaking)
        return;
    if (m_sink)
        m_sink->suspend();
    setState(QTextToSpeech::Paused);
}

void KokoroEngine::resume()
{
    if (m_state != QTextToSpeech::Paused)
        return;
    if (m_sink)
        m_sink->resume();
    setState(QTextToSpeech::Speaking);
}

void KokoroEngine::reset()
{
    if (m_ticker)
        m_ticker->stop();
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
        m_sinkDevice = nullptr;
    }
    m_chunks.clear();
    m_words.clear();
    m_pending.clear();
    m_text.clear();
    m_format = QAudioFormat();
    m_nextToQueue = 0;
    m_nextWord = 0;
    m_queuedUs = 0;
    m_speaking = false;
    m_synthesizing = false;
}

void KokoroEngine::setState(QTextToSpeech::State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(m_state);
}

void KokoroEngine::fail(QTextToSpeech::ErrorReason reason, const QString &message)
{
    reset();
    m_errorReason = reason;
    m_errorString = message;
    m_state = QTextToSpeech::Error;
    emit stateChanged(m_state);
    emit errorOccurred(reason, message);
}

// --- properties -------------------------------------------------------------

double KokoroEngine::rate() const { return m_rate; }

bool KokoroEngine::setRate(double rate)
{
    m_rate = qBound(-1.0, rate, 1.0);
    return true;
}

double KokoroEngine::pitch() const { return m_pitch; }

bool KokoroEngine::setPitch(double)
{
    return false;   // Kokoro exposes no pitch control
}

QLocale KokoroEngine::locale() const { return m_locale; }

bool KokoroEngine::setLocale(const QLocale &locale)
{
    if (!m_locales.contains(locale))
        return false;
    m_locale = locale;
    return true;
}

double KokoroEngine::volume() const { return m_volume; }

bool KokoroEngine::setVolume(double volume)
{
    m_volume = qBound(0.0, volume, 1.0);
    if (m_sink)
        m_sink->setVolume(m_volume);
    return true;
}

QVoice KokoroEngine::voice() const { return m_voice; }

bool KokoroEngine::setVoice(const QVoice &voice)
{
    if (!m_voices.contains(voice))
        return false;
    m_voice = voice;
    return true;
}

QTextToSpeech::State KokoroEngine::state() const { return m_state; }
QTextToSpeech::ErrorReason KokoroEngine::errorReason() const { return m_errorReason; }
QString KokoroEngine::errorString() const { return m_errorString; }
