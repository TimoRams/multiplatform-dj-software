#include "stems/StemsController.h"

#include "app/SettingsManager.h"
#include "library/LibraryDatabase.h"
#include "stems/StemInference.h"
#include "stems/StemSeparationService.h"
#include "stems/Utf8Path.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDebug>
#include <QUrl>

#include <algorithm>
#include <thread>

namespace {
constexpr auto kModelFileName = "htdemucs_fp16weights.onnx";

int inferenceThreads()
{
    // Measured on a 14-core laptop: 4 intra-op threads is the sweet spot; more
    // threads get slower and steal cores from audio and UI.
    const auto cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return std::clamp(cores / 2, 1, 4);
}
} // namespace

const char* StemsController::modelSha256() noexcept
{
    return "d05c269d0178d2a72ad484b10b11dd370193fc923201c3b27a99f848745db70a";
}

const char* StemsController::modelUrl() noexcept
{
    return "https://huggingface.co/StemSplitio/htdemucs-onnx/resolve/main/htdemucs_fp16weights.onnx";
}

StemsController::StemsController(SettingsManager* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
    const auto overridePath = qEnvironmentVariable("BROCKDJ_STEM_MODEL");
    m_modelPath = !overridePath.isEmpty()
        ? overridePath
        : QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
              .filePath(QStringLiteral("models/%1").arg(QLatin1String(kModelFileName)));
    refreshModelState();

    if (!supported())
        return;

    brockdj::stems::StemSeparationService::Config config;
    config.cacheDir = brockdj::stems::pathFromUtf8(cacheDir().toStdString());
    const auto modelPath = m_modelPath.toStdString();
    brockdj::stems::StemInferenceOptions inferenceOptions;
    inferenceOptions.preferGpu = m_settings && m_settings->stemsGpuAcceleration();
    auto epLibrary = qEnvironmentVariable("BROCKDJ_STEM_EP_LIBRARY");
    if (epLibrary.isEmpty()) {
#if defined(_WIN32)
        constexpr auto pluginName = "onnxruntime_providers_openvino_plugin.dll";
#else
        constexpr auto pluginName = "libonnxruntime_providers_openvino_plugin.so";
#endif
        const auto candidate = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .filePath(QStringLiteral("stems-providers/openvino/%1").arg(QLatin1String(pluginName)));
        if (QFileInfo::exists(candidate))
            epLibrary = candidate;
    }
    inferenceOptions.openVinoLibraryPath = epLibrary.toStdString();
    m_service = std::make_unique<brockdj::stems::StemSeparationService>(
        config, [modelPath, inferenceOptions](std::string& error) {
            return brockdj::stems::createOnnxStemInference(modelPath, inferenceThreads(), error, inferenceOptions);
        });
    connect(&m_cacheWatcher, &QFileSystemWatcher::directoryChanged, this,
            [this] { m_cacheInventoryDirty = true; });
    refreshCacheSize();
    m_monitor.setInterval(100);
    connect(&m_monitor, &QTimer::timeout, this, &StemsController::pollService);
    m_monitor.start();
    if (m_modelInstalled && (!m_settings || m_settings->stemsEnabled()))
        m_service->preload();
}

StemsController::~StemsController() { shutdown(); }

bool StemsController::supported() const noexcept { return brockdj::stems::onnxStemInferenceAvailable(); }

void StemsController::shutdown()
{
    m_monitor.stop();
    cancelPreanalysis();
    cancelDownload();
    if (m_service)
        m_service->shutdown();
}

QString StemsController::cacheDir() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("stems"));
}

double StemsController::cacheSizeMb() const
{
    qint64 total = 0;
    QDirIterator it(cacheDir(), { QStringLiteral("*.bstem") }, QDir::Files);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return static_cast<double>(total) / (1024.0 * 1024.0);
}

void StemsController::clearCache()
{
    cancelPreanalysis();
    QDirIterator it(cacheDir(), { QStringLiteral("*.bstem") }, QDir::Files);
    while (it.hasNext()) {
        const auto path = it.next();
        if (!QFile::remove(path))
            setError(tr("Cannot remove stem cache file %1").arg(path));
    }
    refreshCacheSize();
}

void StemsController::setLibraryDatabase(LibraryDatabase* database) { m_database = database; }

void StemsController::refreshCacheSize()
{
    m_cachedFiles.clear();
    if (m_service) {
        m_observedCacheRevision = m_service->cacheRevision();
        for (const auto& name : m_service->cachedFileNames())
            m_cachedFiles.insert(QString::fromStdString(name));
        if (QDir(cacheDir()).exists() && !m_cacheWatcher.directories().contains(cacheDir())
            && !m_cacheWatcher.addPath(cacheDir()))
            qWarning() << "[stems] Cannot monitor the stem cache directory";
    }
    m_cacheInventoryDirty = false;
    ++m_cacheRevision;
    emit cacheChanged();
}

bool StemsController::hasCachedStems(const QString& filePath) const
{
    return m_service && !filePath.isEmpty()
        && m_cachedFiles.contains(QString::fromStdString(brockdj::stems::pathToUtf8(
            m_service->cacheFileFor(filePath.toStdString()).filename())));
}

double StemsController::preanalysisProgress() const noexcept
{
    return m_preanalysisTotal > 0
        ? std::clamp((m_preanalysisCompleted + m_currentPreanalysisProgress) / m_preanalysisTotal, 0.0, 1.0)
        : 0.0;
}

void StemsController::preanalyzeTrack(const QString& filePath, const QString& title)
{
    preanalyzeTracks({ QVariantMap {{ QStringLiteral("filePath"), filePath },
                                    { QStringLiteral("title"), title }} });
}

void StemsController::preanalyzeAll()
{
    if (!m_database) {
        setError(tr("The library is unavailable for stem preanalysis."));
        return;
    }
    preanalyzeTracks(m_database->getAllTrackAnalysisItems(true));
}

void StemsController::preanalyzePlaylist(const QString& playlistId)
{
    if (!m_database || playlistId.isEmpty()) {
        setError(tr("Select a playlist before starting stem preanalysis."));
        return;
    }
    preanalyzeTracks(m_database->getPlaylistAnalysisItems(playlistId, true));
}

void StemsController::preanalyzeTracks(const QVariantList& tracks)
{
    if (!available() || !m_service) {
        setError(tr("Install the stem model and ONNX Runtime before starting stem preanalysis."));
        return;
    }
    if (!m_preanalysisRunning) {
        refreshCacheSize();
        m_batchPaths.clear();
        m_preanalysisTotal = m_preanalysisCompleted = m_preanalysisFailed = 0;
        m_currentPreanalysisProgress = 0.0;
    }
    setError({});
    for (const auto& entry : tracks) {
        auto item = entry.toMap();
        QString path = item.value(QStringLiteral("filePath")).toString();
        if (QUrl(path).isLocalFile())
            path = QUrl(path).toLocalFile();
        if (path.isEmpty()) {
            setError(tr("A selected track has no local audio file."));
            continue;
        }
        const QFileInfo info(path);
        const auto canonical = info.canonicalFilePath();
        path = canonical.isEmpty() ? info.absoluteFilePath() : canonical;
        if (m_batchPaths.contains(path))
            continue;
        if (m_preanalysisTotal >= 4096) {
            setError(tr("The stem preanalysis queue is full (4096 tracks)."));
            break;
        }
        m_batchPaths.insert(path);
        ++m_preanalysisTotal;
        item.insert(QStringLiteral("filePath"), path);
        m_preanalysisQueue.append(item);
    }
    if (m_preanalysisTotal == 0 && m_error.isEmpty())
        setError(tr("No tracks were selected for stem preanalysis."));
    if (!m_preanalysisJob)
        startNextPreanalysis();
    emit preanalysisChanged();
}

void StemsController::startNextPreanalysis()
{
    m_currentPreanalysisProgress = 0.0;
    if (m_preanalysisQueue.isEmpty()) {
        m_preanalysisRunning = false;
        m_preanalysisTitle.clear();
        m_preanalysisJob.reset();
        return;
    }
    const auto item = m_preanalysisQueue.takeFirst().toMap();
    const auto path = item.value(QStringLiteral("filePath")).toString();
    m_preanalysisTitle = item.value(QStringLiteral("title")).toString();
    if (m_preanalysisTitle.isEmpty())
        m_preanalysisTitle = QFileInfo(path).completeBaseName();
    m_preanalysisRunning = true;
    m_preanalysisJob = std::make_shared<brockdj::stems::StemJobStatus>();
    m_service->submitPreanalysis(path.toStdString(), m_preanalysisJob);
}

void StemsController::cancelPreanalysis()
{
    if (m_service)
        m_service->cancelPreanalysis();
    m_preanalysisJob.reset();
    m_preanalysisQueue.clear();
    m_batchPaths.clear();
    m_preanalysisRunning = false;
    m_preanalysisTitle.clear();
    m_preanalysisTotal = m_preanalysisCompleted = m_preanalysisFailed = 0;
    m_currentPreanalysisProgress = 0.0;
    emit preanalysisChanged();
}

void StemsController::pollService()
{
    if (!m_service)
        return;
    if (m_cacheInventoryDirty || m_service->cacheRevision() != m_observedCacheRevision)
        refreshCacheSize();
    const auto backend = QString::fromStdString(m_service->inferenceBackend());
    const auto warning = QString::fromStdString(m_service->acceleratorWarning());
    if (m_backend != backend || m_acceleratorWarning != warning) {
        m_backend = backend;
        m_acceleratorWarning = warning;
        emit backendChanged();
    }
    if (!m_preanalysisJob)
        return;
    using brockdj::stems::StemJobState;
    const auto state = m_preanalysisJob->state.load();
    if (state == StemJobState::Ready || state == StemJobState::Failed) {
        ++m_preanalysisCompleted;
        if (state == StemJobState::Failed) {
            ++m_preanalysisFailed;
            const auto message = tr("Stem preanalysis failed for %1: %2")
                .arg(m_preanalysisTitle, QString::fromStdString(m_preanalysisJob->error()));
            qWarning().noquote() << message;
            setError(message);
        }
        m_preanalysisJob.reset();
        startNextPreanalysis();
        emit preanalysisChanged();
    } else {
        const double progress = m_preanalysisJob->progress.load();
        if (progress != m_currentPreanalysisProgress) {
            m_currentPreanalysisProgress = progress;
            emit preanalysisChanged();
        }
    }
}

void StemsController::setError(const QString& error)
{
    if (m_error == error)
        return;
    m_error = error;
    emit errorChanged();
}

void StemsController::refreshModelState()
{
    const QFileInfo info(m_modelPath);
    const bool overridden = !qEnvironmentVariable("BROCKDJ_STEM_MODEL").isEmpty();
    const bool installed = info.isFile() && (overridden || info.size() == kModelBytes);
    if (installed == m_modelInstalled)
        return;
    m_modelInstalled = installed;
    emit modelChanged();
}

void StemsController::downloadModel()
{
    if (m_reply || !supported() || m_modelInstalled)
        return;
    setError({});
    if (!QDir().mkpath(QFileInfo(m_modelPath).absolutePath())) {
        setError(tr("Cannot create the stem model directory."));
        return;
    }
    m_partFile = std::make_unique<QSaveFile>(m_modelPath);
    if (!m_partFile->open(QIODevice::WriteOnly)) {
        setError(tr("Cannot write the stem model: %1").arg(m_partFile->errorString()));
        m_partFile.reset();
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    QNetworkRequest request { QUrl(QString::fromLatin1(modelUrl())) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(60000);
    m_reply = m_network->get(request);
    m_downloadProgress = 0.0;
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        const auto chunk = m_reply->readAll();
        if (m_partFile->write(chunk) != chunk.size()) {
            setError(tr("Cannot write the stem model: %1").arg(m_partFile->errorString()));
            cancelDownload();
            return;
        }
        m_hash->addData(chunk);
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        m_downloadProgress = std::clamp(static_cast<double>(received)
                                            / static_cast<double>(total > 0 ? total : kModelBytes),
                                        0.0, 1.0);
        emit downloadChanged();
    });
    connect(m_reply, &QNetworkReply::finished, this, [this] { finishDownload(); });
    emit downloadChanged();
}

void StemsController::finishDownload()
{
    if (!m_reply)
        return;
    auto* reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    const auto rest = reply->readAll();
    bool installed = false;
    if (reply->error() != QNetworkReply::NoError) {
        setError(tr("Download failed: %1").arg(reply->errorString()));
        m_partFile->cancelWriting();
    } else if (m_partFile->write(rest) != rest.size()) {
        setError(tr("Cannot write the stem model: %1").arg(m_partFile->errorString()));
        m_partFile->cancelWriting();
    } else {
        m_hash->addData(rest);
        if (m_hash->result().toHex() != QByteArray(modelSha256())) {
            setError(tr("Downloaded model is corrupt (checksum mismatch)"));
            m_partFile->cancelWriting();
        } else if (!m_partFile->commit()) {
            setError(tr("Cannot install the stem model: %1").arg(m_partFile->errorString()));
        } else
            installed = true;
    }
    m_partFile.reset();
    m_hash.reset();
    m_downloadProgress = 0.0;
    if (installed && m_service) {
        m_service->resetInference();
        if (!m_settings || m_settings->stemsEnabled())
            m_service->preload();
    }
    refreshModelState();
    emit downloadChanged();
}

void StemsController::cancelDownload()
{
    if (!m_reply)
        return;
    auto* reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    if (m_partFile) {
        m_partFile->cancelWriting();
        m_partFile.reset();
    }
    m_hash.reset();
    m_downloadProgress = 0.0;
    emit downloadChanged();
}
