#pragma once

#include <QString>
#include <QHash>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <algorithm>

namespace analysis {

enum class AnalysisPriority : std::uint8_t {
    LoadedDeck = 0, UserSelected, VisibleLibrary, BackgroundLibrary, Maintenance, Count
};

struct AnalysisJob {
    QString trackId;
    QString filePath;
    QString title;
    QString key;
    AnalysisPriority priority = AnalysisPriority::BackgroundLibrary;
    std::uint64_t sequence = 0;
};

struct AnalysisQueueStats {
    std::uint64_t queued = 0;
    std::uint64_t deduplicated = 0;
    std::uint64_t reprioritized = 0;
    std::uint64_t dropped = 0;
    std::uint64_t dequeued = 0;
};

class AnalysisJobQueue final
{
public:
    explicit AnalysisJobQueue(std::size_t capacity = 4096,
                              std::size_t fairnessInterval = 16)
        : m_capacity(capacity), m_fairnessInterval(fairnessInterval) {}

    bool push(AnalysisJob job)
    {
        if (job.key.isEmpty()) job.key = job.filePath;
        const auto existing = m_pendingPriority.constFind(job.key);
        if (existing != m_pendingPriority.cend()) {
            const std::size_t p = static_cast<std::size_t>(existing.value());
            auto& queue = m_queues[p];
            for (auto it = queue.begin(); it != queue.end(); ++it) {
                if (it->key != job.key) continue;
                ++m_stats.deduplicated;
                const auto newPriority = index(job.priority);
                if (newPriority < p) {
                    AnalysisJob promoted = std::move(*it);
                    promoted.priority = job.priority;
                    queue.erase(it);
                    m_queues[newPriority].push_back(std::move(promoted));
                    m_pendingPriority[job.key] = static_cast<int>(newPriority);
                    ++m_stats.reprioritized;
                }
                return true;
            }
            // Defensive repair: the index must never retain a missing entry.
            m_pendingPriority.remove(job.key);
        }
        if (size() >= m_capacity) { ++m_stats.dropped; return false; }
        job.sequence = ++m_sequence;
        const auto priority = index(job.priority);
        const QString key = job.key;
        m_queues[priority].push_back(std::move(job));
        m_pendingPriority.insert(key, static_cast<int>(priority));
        ++m_stats.queued;
        return true;
    }

    std::optional<AnalysisJob> pop()
    {
        if (empty()) return std::nullopt;
        std::size_t selected = m_queues.size();
        if (m_sinceBackground >= m_fairnessInterval) {
            for (std::size_t p = index(AnalysisPriority::BackgroundLibrary);
                 p < m_queues.size(); ++p) {
                if (!m_queues[p].empty()) { selected = p; break; }
            }
        }
        if (selected == m_queues.size()) {
            for (std::size_t p = 0; p < m_queues.size(); ++p) {
                if (!m_queues[p].empty()) { selected = p; break; }
            }
        }
        auto& queue = m_queues[selected];
        AnalysisJob result = std::move(queue.front());
        queue.pop_front();
        m_pendingPriority.remove(result.key);
        m_sinceBackground = selected >= index(AnalysisPriority::BackgroundLibrary)
            ? 0 : m_sinceBackground + 1;
        ++m_stats.dequeued;
        return result;
    }

    void clear() { for (auto& queue : m_queues) queue.clear(); m_pendingPriority.clear(); m_sinceBackground = 0; }
    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] std::size_t size() const
    { std::size_t total = 0; for (const auto& q : m_queues) total += q.size(); return total; }
    [[nodiscard]] AnalysisQueueStats stats() const { return m_stats; }

private:
    static constexpr std::size_t index(AnalysisPriority value)
    { return static_cast<std::size_t>(value); }
    static constexpr std::size_t kCount = static_cast<std::size_t>(AnalysisPriority::Count);
    std::array<std::deque<AnalysisJob>, kCount> m_queues;
    QHash<QString, int> m_pendingPriority;
    std::size_t m_capacity;
    std::size_t m_fairnessInterval;
    std::size_t m_sinceBackground = 0;
    std::uint64_t m_sequence = 0;
    AnalysisQueueStats m_stats;
};

} // namespace analysis
