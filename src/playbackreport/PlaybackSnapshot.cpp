#include "pch.h"

#include "PlaybackSnapshot.h"

PlaybackEvent PlaybackSnapshot::toEvent(PlaybackEvent::Phase phase,
                                        const QString &sessionToken) const {
    PlaybackEvent ev;
    ev.phase = phase;
    ev.sessionToken = sessionToken;
    ev.identity = identity;
    ev.positionSeconds = positionSeconds;
    ev.durationSeconds = durationSeconds;
    ev.paused = paused;
    ev.metadata = metadata;
    return ev;
}

PlaybackSnapshot GuardedPlaybackSnapshot::copy() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_value;
}

PlaybackSnapshot
GuardedPlaybackSnapshot::update(const std::function<void(PlaybackSnapshot &)> &mutate) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    mutate(m_value);
    return m_value;
}
