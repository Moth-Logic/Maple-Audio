// Self-contained tests for ThreadSafeQueue. No external test framework yet.
#include "Queue/ThreadSafeQueue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

static std::atomic<int> g_failures{0};

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static void TestFifoOrder() {
    ThreadSafeQueue<int> queue;
    for (int i = 0; i < 5; ++i) CHECK(queue.Push(i));

    int value = -1;
    for (int i = 0; i < 5; ++i) {
        CHECK(queue.Pop(value));
        CHECK(value == i);
    }
    CHECK(queue.Size() == 0);
}

static void TestTryPop() {
    ThreadSafeQueue<int> queue;
    int value = 0;
    CHECK(!queue.TryPop(value));

    CHECK(queue.Push(42));
    CHECK(queue.TryPop(value));
    CHECK(value == 42);
    CHECK(!queue.TryPop(value));
}

static void TestPopBlocksUntilPush() {
    ThreadSafeQueue<int> queue;
    std::atomic<bool> gotItem{false};
    int received = 0;

    std::thread consumer([&] {
        int value = 0;
        if (queue.Pop(value)) {
            received = value;
            gotItem = true;
        }
    });

    // Give the consumer a chance to actually block, then release it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!gotItem.load());

    CHECK(queue.Push(7));
    consumer.join();

    CHECK(gotItem.load());
    CHECK(received == 7);
}

static void TestCloseWakesBlockedPop() {
    ThreadSafeQueue<int> queue;
    std::atomic<bool> finished{false};
    bool popResult = true;
    int value = 0;

    std::thread consumer([&] {
        popResult = queue.Pop(value);
        finished = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!finished.load());

    queue.Close();
    consumer.join();

    CHECK(finished.load());
    CHECK(!popResult);
}

static void TestDrainAfterClose() {
    ThreadSafeQueue<int> queue;
    CHECK(queue.Push(1));
    CHECK(queue.Push(2));
    queue.Close();

    int value = 0;
    CHECK(queue.Pop(value));
    CHECK(value == 1);
    CHECK(queue.Pop(value));
    CHECK(value == 2);
    CHECK(!queue.Pop(value));
    CHECK(queue.IsClosed());
}

static void TestPushAfterCloseIsRejected() {
    ThreadSafeQueue<int> queue;
    queue.Close();
    CHECK(!queue.Push(1));
    CHECK(queue.Size() == 0);
}

static void TestMoveOnlyType() {
    ThreadSafeQueue<std::unique_ptr<int>> queue;
    CHECK(queue.Push(std::make_unique<int>(11)));

    std::unique_ptr<int> value;
    CHECK(queue.Pop(value));
    CHECK(value != nullptr);
    CHECK(*value == 11);
}

static void TestManyProducersAndConsumers() {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 25000;

    ThreadSafeQueue<int> queue;
    std::atomic<long long> consumedSum{0};
    std::atomic<int> consumedCount{0};

    std::vector<std::thread> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&] {
            int value = 0;
            while (queue.Pop(value)) {
                consumedSum += value;
                consumedCount += 1;
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                CHECK(queue.Push(p * kPerProducer + i));
            }
        });
    }

    for (auto& producer : producers) producer.join();
    queue.Close();
    for (auto& consumer : consumers) consumer.join();

    const long long total = (long long)kProducers * kPerProducer;
    CHECK(consumedCount.load() == total);
    CHECK(consumedSum.load() == total * (total - 1) / 2);
}

int main() {
    TestFifoOrder();
    TestTryPop();
    TestPopBlocksUntilPush();
    TestCloseWakesBlockedPop();
    TestDrainAfterClose();
    TestPushAfterCloseIsRejected();
    TestMoveOnlyType();
    TestManyProducersAndConsumers();

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures.load());
        return 1;
    }
    std::printf("All queue tests passed\n");
    return 0;
}
