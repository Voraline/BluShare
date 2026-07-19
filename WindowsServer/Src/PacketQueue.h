#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

class PacketQueue {
public:
    explicit PacketQueue(size_t MaxQueuedPackets = 32) : MaxQueued(MaxQueuedPackets) {}

    void Push(std::vector<uint8_t> Packet) {
        {
            std::lock_guard<std::mutex> Lock(Mtx);
            if (Closed) return;

            Items.push_back(std::move(Packet));
            while (Items.size() > MaxQueued) {
                Items.pop_front();
                ++DroppedCount;
            }
        }
        Cv.notify_one();
    }

    bool Pop(std::vector<uint8_t>& Out) {
        std::unique_lock<std::mutex> Lock(Mtx);
        Cv.wait(Lock, [this] { return !Items.empty() || Closed; });
        if (Items.empty()) return false;

        Out = std::move(Items.front());
        Items.pop_front();
        return true;
    }

    void Close() {
        {
            std::lock_guard<std::mutex> Lock(Mtx);
            Closed = true;
        }
        Cv.notify_all();
    }

    void Reopen() {
        std::lock_guard<std::mutex> Lock(Mtx);
        Closed = false;
        Items.clear();
        DroppedCount = 0;
    }

    uint64_t DroppedSinceReopen() const {
        std::lock_guard<std::mutex> Lock(Mtx);
        return DroppedCount;
    }

private:
    mutable std::mutex Mtx;
    std::condition_variable Cv;
    std::deque<std::vector<uint8_t>> Items;
    size_t MaxQueued;
    bool Closed = false;
    uint64_t DroppedCount = 0;
};