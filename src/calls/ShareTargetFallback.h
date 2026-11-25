#pragma once

/// Which PipeWire target a portal screen share captures from. A share granted
/// with a stream serial tries `target-object` first and may fall back to the
/// node id once; a legacy grant uses the node id from the start.
class ShareTargetFallback
{
public:
    enum class Path { Serial, NodeId };

    explicit ShareTargetFallback(bool hasSerial = false)
        : m_path(hasSerial ? Path::Serial : Path::NodeId), m_settled(!hasSerial)
    {
    }

    Path path() const { return m_path; }
    bool settled() const { return m_settled; }

    /// True exactly once: the serial path delivered no frame by its deadline.
    bool fallBack(bool firstFrameSeen)
    {
        if (m_settled)
            return false;
        m_settled = true;
        if (firstFrameSeen)
            return false;
        m_path = Path::NodeId;
        return true;
    }

    void cancel() { m_settled = true; }

    static const char *name(Path path)
    {
        return path == Path::Serial ? "serial" : "node-id";
    }

private:
    Path m_path;
    bool m_settled;
};
