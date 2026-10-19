#pragma once

// bw/input/queue_command_sink.h — the ICommandSink seam (F11 D6): adapts any
// bw_core SpscQueue<Command, N> to bw::ICommandSink so the engine's caller can
// push commands without the queue type leaking into the library. Lives in the
// library (not only in wiring) so the drop path is host-testable; it is pure
// bw_core glue — no GPIO, no platform calls.

#include <bw/commands.h>
#include <bw/containers/spsc_queue.h>
#include <bw/providers.h>

namespace bw::input {

// push() on a full queue is refused (Err::Full) and the queue counts the drop
// itself; the sink reports false and never blocks, never crashes (F11 D6).
template <std::size_t N> class QueueCommandSink : public ICommandSink {
public:
    explicit QueueCommandSink(SpscQueue<Command, N>* queue) : queue_(queue) {}

    bool push(const Command& command) override {
        if (queue_ == nullptr) {
            return false;
        }
        return queue_->push(command) == Err::Ok;
    }

private:
    SpscQueue<Command, N>* queue_;
};

}  // namespace bw::input
