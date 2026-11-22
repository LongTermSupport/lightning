#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace stalltrace {

bool enabled();

void install(int thresholdMsOverride = -1);

void shutdown();

int stallCount();
qint64 lastStallMs();
QByteArray lastStallCategory();

// Counts heavy work by category only when it runs on the application's
// thread, so tests can assert where it ran; always on and cheap (one call per
// operation). `bytes` is what the operation read, decoded or wrote.
void noteGuiWork(const char *category, qint64 bytes);
int guiWorkCalls(const char *category);
qint64 guiWorkBytes(const char *category);
void resetGuiWork();

// RAII category marker for heavy GUI-thread sections; the innermost active one
// is logged with a stall. Inert off the GUI thread. The argument must be a
// string literal (it is stored, not copied).
class Scope
{
public:
    explicit Scope(const char *category);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    const char *m_previous = nullptr;
    bool m_active = false;
};

}
