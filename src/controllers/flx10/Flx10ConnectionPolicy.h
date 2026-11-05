#pragma once
#include <QString>
#include <cstdint>

namespace flx10 {
enum class ConnectionFailure { None, Absent, Incompatible, Denied, Ambiguous, Transient };

// Pure retry policy: passive topology observations never open a device.
class ConnectionPolicy {
public:
    void enable(const QString& signature) { m_enabled = true; reset(signature); }
    void disable() { m_enabled = false; m_due = -1; }
    void reset(const QString& signature) { m_signature = signature; m_retries = 0; m_due = -1; m_failure = ConnectionFailure::None; }
    void connected() { m_failure = ConnectionFailure::None; m_retries = 0; m_due = -1; }
    void failed(ConnectionFailure failure, std::int64_t now) {
        m_failure = failure;
        m_due = failure == ConnectionFailure::Transient && m_retries < 2
            ? now + (m_retries == 0 ? 5000 : 15000) : -1;
    }
    bool shouldAttempt(const QString& signature, std::int64_t now) {
        if (!m_enabled) return false;
        if (signature != m_signature) { reset(signature); return !signature.isEmpty(); }
        if (m_due < 0 || now < m_due) return false;
        ++m_retries; m_due = -1; return true;
    }
    ConnectionFailure failure() const { return m_failure; }
    bool retryPending() const { return m_due >= 0; }
private:
    QString m_signature;
    bool m_enabled = false;
    int m_retries = 0;
    std::int64_t m_due = -1;
    ConnectionFailure m_failure = ConnectionFailure::None;
};
}
