#include "chipcae/coupling/residual_decimator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chipcae::coupling {
namespace {
io::ResidualBatchPoint batch_point(const ResidualRecord& record) {
    return {record.solver,
            record.field,
            record.iteration,
            record.norm,
            record.flags,
            record.linear_iterations,
            0};
}
} // namespace

ResidualDecimator::ResidualDecimator(ResidualBatchSink sink, ResidualDecimatorOptions options)
    : sink_(std::move(sink)), options_(options) {
    if (!sink_)
        throw std::invalid_argument("residual batch sink must not be empty");
}

ResidualDecimator::Sequence& ResidualDecimator::sequence_for(const ResidualRecord& record) {
    const auto key = std::pair{record.solver, record.field};
    const auto found = sequence_lookup_.find(key);
    if (found != sequence_lookup_.end())
        return sequences_[found->second];

    sequences_.emplace_back();
    try {
        sequence_lookup_.emplace(key, sequences_.size() - 1);
    } catch (...) {
        sequences_.pop_back();
        throw;
    }
    return sequences_.back();
}

void ResidualDecimator::on_iteration(const ResidualRecord& record) {
    auto& sequence = sequence_for(record);
    if (sequence.iteration_calls == std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("residual window iteration count exceeds uint32_t");
    sequence.points.push_back(batch_point(record));
    ++sequence.iteration_calls;
}

void ResidualDecimator::on_special(SpecialPoint point) {
    auto& sequence = sequence_for(point.record);
    const auto flags = point.record.flags | static_cast<std::uint32_t>(point.kind);
    if (!sequence.points.empty() && sequence.points.back().iteration == point.record.iteration) {
        sequence.points.back().flags |= flags;
    } else {
        auto appended = batch_point(point.record);
        appended.flags = flags;
        sequence.points.push_back(std::move(appended));
    }
}

void ResidualDecimator::on_window_end(const WindowSummary& summary) {
    if (previous_window_ && summary.window <= *previous_window_)
        throw std::invalid_argument("residual window numbers must increase strictly");
    if (!std::isfinite(summary.time))
        throw std::invalid_argument("residual window time must be finite");

    io::ResidualBatch batch{summary.window, summary.time, {}, 0};
    for (const auto& sequence : sequences_) {
        // Work on local points so failed assembly leaves all pending records untouched.
        auto points = sequence.points;
        if (points.empty())
            continue;
        points.front().flags |= residual_flag::window_first;
        points.back().flags |= residual_flag::window_last;
        points.back().window_iterations = sequence.iteration_calls;

        const auto eligible = static_cast<std::size_t>(std::count_if(
            points.begin(), points.end(), [](const auto& point) { return point.flags == 0; }));
        const auto retained = std::min(eligible, std::size_t{options_.max_intermediate});
        const auto dropped = eligible - retained;
        if (dropped > std::numeric_limits<std::uint32_t>::max() - batch.dropped_intermediate)
            throw std::overflow_error("residual dropped intermediate count exceeds uint32_t");
        batch.dropped_intermediate += static_cast<std::uint32_t>(dropped);

        // floor((2j+1)*k/(2M)): start at k/(2M), then add k/M using
        // quotient/remainder steps. Only the bounded remainders are doubled;
        // no product involving k or j can overflow or lose integer precision.
        const auto denominator = std::uint64_t{2} * options_.max_intermediate;
        const auto step = retained != 0 ? eligible / retained : 0;
        const auto remainder_step = retained != 0 ? std::uint64_t{2} * (eligible % retained) : 0;
        auto next = denominator != 0 ? eligible / denominator : 0;
        auto remainder = denominator != 0 ? eligible % denominator : 0;
        auto remaining = retained;
        std::size_t candidate = 0;
        for (auto& point : points) {
            if (point.flags != 0) {
                batch.points.push_back(std::move(point));
                continue;
            }
            if (eligible == retained || (remaining != 0 && candidate == next)) {
                batch.points.push_back(std::move(point));
                if (eligible != retained && --remaining != 0) {
                    next += step;
                    remainder += remainder_step;
                    if (remainder >= denominator) {
                        ++next;
                        remainder -= denominator;
                    }
                }
            }
            ++candidate;
        }
    }

    sequences_.clear();
    sequence_lookup_.clear();
    previous_window_ = summary.window;
    if (!batch.points.empty()) {
        sink_(batch);
        ++batches_emitted_;
    }
}

std::uint64_t ResidualDecimator::batches_emitted() const noexcept {
    return batches_emitted_;
}
} // namespace chipcae::coupling
