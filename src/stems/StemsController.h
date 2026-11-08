#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QSet>
#include <QVariantList>
#include <QFileSystemWatcher>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;
class QCryptographicHash;
class SettingsManager;
class LibraryDatabase;

namespace brockdj::stems {
class StemSeparationService;
struct StemJobStatus;
}

// App-level owner of real-time stems: the HTDemucs model file (downloaded on
// demand, sha256 verified) and the shared separation worker used by all decks.
class StemsController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool modelInstalled READ modelInstalled NOTIFY modelChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadChanged)
    Q_PROPERTY(double downloadProgress READ downloadProgress NOTIFY downloadChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString modelPath READ modelPath CONSTANT)
    Q_PROPERTY(double cacheSizeMb READ cacheSizeMb NOTIFY cacheChanged)
    // True when stems can actually be produced right now.
    Q_PROPERTY(bool available READ available NOTIFY modelChanged)
    Q_PROPERTY(bool preanalysisRunning READ preanalysisRunning NOTIFY preanalysisChanged)
    Q_PROPERTY(int preanalysisTotal READ preanalysisTotal NOTIFY preanalysisChanged)
    Q_PROPERTY(int preanalysisCompleted READ preanalysisCompleted NOTIFY preanalysisChanged)
    Q_PROPERTY(int preanalysisFailed READ preanalysisFailed NOTIFY preanalysisChanged)
    Q_PROPERTY(double preanalysisProgress READ preanalysisProgress NOTIFY preanalysisChanged)
    Q_PROPERTY(QString preanalysisTitle READ preanalysisTitle NOTIFY preanalysisChanged)
    Q_PROPERTY(qulonglong cacheRevision READ cacheRevision NOTIFY cacheChanged)
    Q_PROPERTY(QString backend READ backend NOTIFY backendChanged)
    Q_PROPERTY(QString acceleratorWarning READ acceleratorWarning NOTIFY backendChanged)

public:
    explicit StemsController(SettingsManager* settings, QObject* parent = nullptr);
    ~StemsController() override;

    static constexpr qint64 kModelBytes = 165612636;
    static const char* modelSha256() noexcept;
    static const char* modelUrl() noexcept;

    bool supported() const noexcept;
    bool modelInstalled() const noexcept { return m_modelInstalled; }
    bool available() const noexcept { return supported() && m_modelInstalled; }
    bool downloading() const noexcept { return m_reply != nullptr; }
    double downloadProgress() const noexcept { return m_downloadProgress; }
    QString error() const { return m_error; }
    QString modelPath() const { return m_modelPath; }
    double cacheSizeMb() const;
    bool preanalysisRunning() const noexcept { return m_preanalysisRunning; }
    int preanalysisTotal() const noexcept { return m_preanalysisTotal; }
    int preanalysisCompleted() const noexcept { return m_preanalysisCompleted; }
    int preanalysisFailed() const noexcept { return m_preanalysisFailed; }
    double preanalysisProgress() const noexcept;
    QString preanalysisTitle() const { return m_preanalysisTitle; }
    qulonglong cacheRevision() const noexcept { return m_cacheRevision; }
    QString backend() const { return m_backend; }
    QString acceleratorWarning() const { return m_acceleratorWarning; }
    void setLibraryDatabase(LibraryDatabase* database);

    Q_INVOKABLE void downloadModel();
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void clearCache();
    Q_INVOKABLE void refreshCacheSize();
    Q_INVOKABLE bool hasCachedStems(const QString& filePath) const;
    Q_INVOKABLE void preanalyzeTracks(const QVariantList& tracks);
    Q_INVOKABLE void preanalyzeTrack(const QString& filePath, const QString& title = {});
    Q_INVOKABLE void preanalyzeAll();
    Q_INVOKABLE void preanalyzePlaylist(const QString& playlistId);
    Q_INVOKABLE void cancelPreanalysis();

    // nullptr when stems are unsupported in this build.
    brockdj::stems::StemSeparationService* service() const noexcept { return m_service.get(); }
    void shutdown();

signals:
    void modelChanged();
    void downloadChanged();
    void errorChanged();
    void cacheChanged();
    void preanalysisChanged();
    void backendChanged();

private:
    void setError(const QString& error);
    void refreshModelState();
    void finishDownload();
    QString cacheDir() const;
    void pollService();
    void startNextPreanalysis();

    QPointer<SettingsManager> m_settings;
    QPointer<LibraryDatabase> m_database;
    QString m_modelPath;
    bool m_modelInstalled = false;
    QString m_error;
    std::unique_ptr<brockdj::stems::StemSeparationService> m_service;
    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;
    std::unique_ptr<QSaveFile> m_partFile;
    std::unique_ptr<QCryptographicHash> m_hash;
    double m_downloadProgress = 0.0;
    QTimer m_monitor;
    QFileSystemWatcher m_cacheWatcher;
    bool m_cacheInventoryDirty = false;
    QString m_backend;
    QString m_acceleratorWarning;
    QVariantList m_preanalysisQueue;
    QSet<QString> m_batchPaths;
    QSet<QString> m_cachedFiles;
    std::shared_ptr<brockdj::stems::StemJobStatus> m_preanalysisJob;
    bool m_preanalysisRunning = false;
    int m_preanalysisTotal = 0;
    int m_preanalysisCompleted = 0;
    int m_preanalysisFailed = 0;
    double m_currentPreanalysisProgress = 0.0;
    QString m_preanalysisTitle;
    qulonglong m_cacheRevision = 0;
    std::uint64_t m_observedCacheRevision = 0;
};
