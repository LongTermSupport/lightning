#pragma once

#include <atomic>

/// The in-place restarts a screen share's capture may take, one of each kind. Messages are
/// stamped with stamp() on whatever thread posts them; the GUI thread asks
/// isStale() of that stamp.
class ShareCaptureRetry
{
public:
    /// GUI thread. The memory-type restart; false once it has been asked for.
    bool begin() { return claim(m_memoryClaimed); }
    /// GUI thread. The serial-to-node-id restart, budgeted apart from begin().
    bool beginPathFallback() { return claim(m_pathClaimed); }
    int requested() const { return m_requested.load(); }
    bool isStale(int stamp) const { return stamp >= 0 && stamp < requested(); }

    int stamp() const { return m_published.load(); }

    static bool mayRestart(bool binTargetPlaying, bool binHasParent)
    {
        return binTargetPlaying && binHasParent;
    }
    /// Restart thread, only after the superseded attempt's streaming thread
    /// has been joined: anything it posted still carries the old stamp.
    void publish() { m_published.store(m_requested.load()); }

private:
    bool claim(std::atomic<bool> &flag)
    {
        bool expected = false;
        if (!flag.compare_exchange_strong(expected, true))
            return false;
        m_requested.fetch_add(1);
        return true;
    }

    std::atomic<bool> m_memoryClaimed{false};
    std::atomic<bool> m_pathClaimed{false};
    std::atomic<int> m_requested{0};
    std::atomic<int> m_published{0};
};
