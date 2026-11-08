
#include <windows.h>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <cstdint>
struct LocalPcmQueue {
    std::mutex mutex;
    std::condition_variable spaceAvailable;
    std::vector<BYTE> storage;
    std::size_t readOffset{};
    std::size_t writeOffset{};
    std::size_t sizeBytes{};
    std::uint32_t sampleRate{};
    std::uint32_t channels{};
    std::uint32_t bitsPerChannel{};
    std::uint32_t bytesPerFrame{};
    std::uint32_t formatFlags{};
    std::size_t maximumBytes{};
    std::size_t hardMaximumBytes{512ULL * 1024ULL * 1024ULL};
    bool active{};
    bool abandoned{};
    std::uint64_t writeEpoch{};
    std::uint64_t capturedFrames{};
    std::uint64_t consumedFrames{};
    std::uint64_t droppedFrames{};
    std::uint64_t underrunFrames{};
    std::atomic<bool> announced{};

    void Push(const void* data, std::size_t byteCount) {
        if (!data || !byteCount || !bytesPerFrame || byteCount % bytesPerFrame != 0) return;
        std::unique_lock lock(mutex);
        if (abandoned) return;
        if (storage.empty() && maximumBytes) storage.resize(maximumBytes);
        if (storage.empty() || byteCount > hardMaximumBytes) {
            droppedFrames += byteCount / bytesPerFrame;
            return;
        }
        const auto requiredBytes = sizeBytes + byteCount;
        if (requiredBytes > storage.size() && requiredBytes <= hardMaximumBytes) {
            const auto doubled = storage.size() <= hardMaximumBytes / 2
                ? storage.size() * 2 : hardMaximumBytes;
            const auto expandedSize = std::min(hardMaximumBytes,
                std::max(requiredBytes, doubled));
            try {
                std::vector<BYTE> expanded(expandedSize);
                if (sizeBytes) {
                    const auto firstPart = std::min(sizeBytes, storage.size() - readOffset);
                    std::memcpy(expanded.data(), storage.data() + readOffset, firstPart);
                    if (firstPart < sizeBytes) {
                        std::memcpy(expanded.data() + firstPart, storage.data(),
                                    sizeBytes - firstPart);
                    }
                }
                storage.swap(expanded);
                readOffset = 0;
                writeOffset = sizeBytes;
            } catch (const std::bad_alloc&) {
                droppedFrames += byteCount / bytesPerFrame;
                return;
            }
        }
        if (abandoned || sizeBytes + byteCount > storage.size()) {
            droppedFrames += byteCount / bytesPerFrame;
            return;
        }
        const auto* first = static_cast<const BYTE*>(data);
        const auto firstPart = std::min(byteCount, storage.size() - writeOffset);
        std::memcpy(storage.data() + writeOffset, first, firstPart);
        if (firstPart < byteCount) {
            std::memcpy(storage.data(), first + firstPart, byteCount - firstPart);
        }
        writeOffset = (writeOffset + byteCount) % storage.size();
        sizeBytes += byteCount;
        capturedFrames += byteCount / bytesPerFrame;
    }

    bool Pop(void* destination, std::uint32_t frames,
             std::uint32_t* copiedFrameCount = nullptr) {
        if (!destination || !frames || !bytesPerFrame) return false;
        const auto requestedBytes = static_cast<std::size_t>(frames) * bytesPerFrame;
        std::unique_lock lock(mutex);
        const auto copiedBytes = std::min(requestedBytes, sizeBytes);
        auto* output = static_cast<BYTE*>(destination);
        if (copiedBytes) {
            const auto firstPart = std::min(copiedBytes, storage.size() - readOffset);
            std::memcpy(output, storage.data() + readOffset, firstPart);
            if (firstPart < copiedBytes) {
                std::memcpy(output + firstPart, storage.data(), copiedBytes - firstPart);
            }
            readOffset = (readOffset + copiedBytes) % storage.size();
            sizeBytes -= copiedBytes;
        }
        const auto copiedFrames = static_cast<std::uint32_t>(copiedBytes / bytesPerFrame);
        if (copiedBytes < requestedBytes) {
            std::memset(output + copiedBytes, 0, requestedBytes - copiedBytes);
            underrunFrames += (requestedBytes - copiedBytes) / bytesPerFrame;
        }
        consumedFrames += copiedFrames;
        if (copiedFrameCount) *copiedFrameCount = copiedFrames;
        lock.unlock();
        spaceAvailable.notify_all();
        return copiedBytes == requestedBytes;
    }

    void Activate() {
        std::lock_guard lock(mutex);
        active = true;
        abandoned = false;
    }
    void FlushForSeek() {
        std::lock_guard lock(mutex);
        ++writeEpoch;
        readOffset = 0;
        writeOffset = 0;
        sizeBytes = 0;
        // Seek/reset is a control boundary. Return an oversized allocation;
        // the next producer call reacquires the ordinary ten-second buffer.
        if (storage.size() > maximumBytes) std::vector<BYTE>().swap(storage);
        active = true;
        abandoned = false;
        spaceAvailable.notify_all();
    }
    void Abandon() {
        std::lock_guard lock(mutex);
        ++writeEpoch;
        active = false;
        abandoned = true;
        readOffset = 0;
        writeOffset = 0;
        sizeBytes = 0;
        std::vector<BYTE>().swap(storage);
        spaceAvailable.notify_all();
    }
};
int main(){
 LocalPcmQueue q;q.maximumBytes=192000u*8u*10u;q.bytesPerFrame=8;q.sampleRate=192000;q.channels=2;
 std::vector<BYTE> block(1024*1024,1);
 for(int i=0;i<96;++i)q.Push(block.data(),block.size());
 std::cout<<"{\"phase\":\"backlog\",\"queuedBytes\":"<<q.sizeBytes<<",\"allocatedBytes\":"<<q.storage.size()<<",\"hardLimit\":"<<q.hardMaximumBytes<<"}\n";
 q.FlushForSeek();
 std::cout<<"{\"phase\":\"afterFlush\",\"queuedBytes\":"<<q.sizeBytes<<",\"allocatedBytes\":"<<q.storage.size()<<"}\n";
 if(!q.storage.empty()||q.sizeBytes)return 3;
 q.Push(block.data(),block.size());
 std::vector<BYTE> restored(block.size());
 if(!q.Pop(restored.data(),static_cast<std::uint32_t>(block.size()/q.bytesPerFrame))||restored!=block)return 4;
 std::cout<<"PASS: queue reacquires storage and preserves PCM after seek\n";
 q.Abandon();
 q.Push(block.data(),block.size());
 if(!q.storage.empty()||q.sizeBytes)return 2;
 std::cout<<"{\"phase\":\"afterAbandon\",\"queuedBytes\":"<<q.sizeBytes<<",\"allocatedBytes\":"<<q.storage.size()<<"}\n";
}
