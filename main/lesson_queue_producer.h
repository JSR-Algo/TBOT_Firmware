#pragma once
#include <atomic>

// Shutdown closes admission before waiting for producers that can touch the queue.
class LessonQueueProducer {
public:
    LessonQueueProducer(std::atomic<unsigned>& readers, const std::atomic<bool>& stopped)
        : readers_(readers) {
        readers_.fetch_add(1);
        admitted_ = !stopped.load();
    }
    ~LessonQueueProducer() { readers_.fetch_sub(1); }
    explicit operator bool() const { return admitted_; }
    LessonQueueProducer(const LessonQueueProducer&) = delete;
    LessonQueueProducer& operator=(const LessonQueueProducer&) = delete;
private:
    std::atomic<unsigned>& readers_;
    bool admitted_;
};
