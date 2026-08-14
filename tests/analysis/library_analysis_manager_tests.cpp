#include "analysis/AnalysisJobQueue.h"
#include "analysis/AnalysisProgress.h"

#include <iostream>
#include <chrono>

namespace {
bool require(bool value, const char* message)
{
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
analysis::AnalysisJob job(QString key, analysis::AnalysisPriority priority)
{
    analysis::AnalysisJob value;
    value.key = std::move(key);
    value.filePath = value.key;
    value.trackId = value.key;
    value.priority = priority;
    return value;
}
}

int main()
{
    using analysis::AnalysisPriority;
    bool ok = true;
    ok &= require(analysis::aggregateProgress(0, 4, 0.0) == 0.0,
                  "empty aggregate progress failed");
    ok &= require(analysis::aggregateProgress(1, 4, 0.5) == 0.375,
                  "current-track aggregate progress failed");
    ok &= require(analysis::aggregateProgress(4, 4, 0.0) == 1.0,
                  "completed aggregate progress failed");
    ok &= require(analysis::aggregateProgress(10, 4, 2.0) == 1.0,
                  "aggregate progress must be clamped");
    analysis::AnalysisJobQueue queue(3, 2);
    ok &= require(queue.push(job("b1", AnalysisPriority::BackgroundLibrary)), "enqueue b1");
    ok &= require(queue.push(job("b2", AnalysisPriority::BackgroundLibrary)), "enqueue b2");
    ok &= require(queue.push(job("deck", AnalysisPriority::LoadedDeck)), "enqueue deck");
    ok &= require(!queue.push(job("overflow", AnalysisPriority::Maintenance)), "capacity not bounded");
    ok &= require(queue.pop()->key == "deck", "priority ordering failed");
    ok &= require(queue.pop()->key == "b1", "FIFO ordering failed");
    ok &= require(queue.pop()->key == "b2", "FIFO tail failed");

    queue.clear();
    queue.push(job("same", AnalysisPriority::BackgroundLibrary));
    queue.push(job("same", AnalysisPriority::UserSelected));
    ok &= require(queue.size() == 1, "deduplication failed");
    ok &= require(queue.pop()->priority == AnalysisPriority::UserSelected, "reprioritization failed");

    queue.clear();
    queue.push(job("background", AnalysisPriority::BackgroundLibrary));
    queue.push(job("i1", AnalysisPriority::UserSelected));
    queue.push(job("i2", AnalysisPriority::UserSelected));
    ok &= require(queue.pop()->key == "i1", "interactive order failed");
    ok &= require(queue.pop()->key == "i2", "interactive second failed");
    ok &= require(queue.pop()->key == "background", "background fairness failed");

    for (const int count : {10, 100, 1000, 10000}) {
        analysis::AnalysisJobQueue measured(static_cast<std::size_t>(count));
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < count; ++i)
            measured.push(job(QString::number(i), AnalysisPriority::BackgroundLibrary));
        while (measured.pop()) {}
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count();
        std::cout << "queue " << count << " enqueue+dequeue: " << micros << " us\n";
    }
    return ok ? 0 : 1;
}
