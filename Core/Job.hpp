#pragma once
#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

// ── Resumable machine jobs ───────────────────────────────────────────────
//
// A boot, a preset, a typed program, a power cycle: sequences that drive a
// machine for seconds of emulated time through nested runCycles() loops.
// Written as C++20 coroutines they read exactly like the blocking code they
// replace -- `machine.runCycles(n)` becomes `co_await job::run(machine, n)`
// -- but a Job can be run a slice at a time from a frontend's frame tick
// (a browser can't block its main thread for seconds), or straight through
// with runToCompletion() (the CLIs, the tests, every blocking wrapper).
//
// Equivalence by construction: a run request is never split. A Job hands
// the machine exactly the runCycles() calls the coroutine asked for, in the
// same order, whatever budget step() is given; only where the job pauses
// between them changes. So a job sliced into frame ticks leaves the machine
// in the same state, cycle for cycle, as one run to completion.
//
// Rules for writing one:
//  - Every function that (transitively) runs the machine is a coroutine
//    returning Task<T>, and awaits the ones it calls.
//  - Coroutine parameters are taken by value, except the machine reference
//    and out-pointers: a Task starts lazily, after the full expression that
//    created it -- a reference to a temporary argument would dangle.
//  - Single-threaded: one job runs at a time per thread.
namespace job {

// What a suspended job is waiting for: one runCycles() call.
struct Pending {
    std::function<std::uint64_t()> run;  // the machine call
    std::uint64_t* result = nullptr;     // where its return value goes
    std::coroutine_handle<> resume;      // the coroutine to continue afterwards
    explicit operator bool() const { return static_cast<bool>(resume); }
};

namespace detail {

// The Pending of the job being resumed on this thread.
inline thread_local Pending* tl_pending = nullptr;

struct PromiseBase {
    std::coroutine_handle<> continuation;
    std::exception_ptr exception;

    std::suspend_always initial_suspend() noexcept { return {}; }
    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
            const std::coroutine_handle<> next = h.promise().continuation;
            return next ? next : std::noop_coroutine();
        }
        void await_resume() noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }
    void unhandled_exception() { exception = std::current_exception(); }
};

template <class T>
struct Promise;

} // namespace detail

// A lazily started coroutine producing a T. Await it from another
// coroutine, or hand it to a Job.
template <class T = void>
class [[nodiscard]] Task {
public:
    using promise_type = detail::Promise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    Task() = default;
    explicit Task(Handle h) : m_h(h) {}
    Task(Task&& other) noexcept : m_h(std::exchange(other.m_h, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (m_h) m_h.destroy();
            m_h = std::exchange(other.m_h, {});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() {
        if (m_h) m_h.destroy();
    }

    bool done() const { return !m_h || m_h.done(); }
    Handle handle() const { return m_h; }

    // co_await: run this task, then continue the awaiting coroutine.
    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        m_h.promise().continuation = caller;
        return m_h;
    }
    T await_resume() { return m_h.promise().take(); }

private:
    Handle m_h;
};

namespace detail {

template <class T>
struct Promise : PromiseBase {
    std::optional<T> value;
    Task<T> get_return_object() { return Task<T>(std::coroutine_handle<Promise>::from_promise(*this)); }
    template <class U>
    void return_value(U&& v) {
        value.emplace(std::forward<U>(v));
    }
    T take() {
        if (exception) std::rethrow_exception(exception);
        return std::move(*value);
    }
};

template <>
struct Promise<void> : PromiseBase {
    Task<void> get_return_object() { return Task<void>(std::coroutine_handle<Promise>::from_promise(*this)); }
    void return_void() {}
    void take() {
        if (exception) std::rethrow_exception(exception);
    }
};

// co_await run(machine, n): suspends the whole job until the driver has made
// the runCycles(n) call; yields its return value.
struct RunAwaiter {
    std::function<std::uint64_t()> call;
    std::uint64_t result = 0;

    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept {
        Pending* p = tl_pending;
        p->run = std::move(call);
        p->result = &result;
        p->resume = h;
    }
    std::uint64_t await_resume() noexcept { return result; }
};

} // namespace detail

// The awaitable form of `machine.runCycles(cycles)`.
template <class Machine>
detail::RunAwaiter run(Machine& machine, std::uint64_t cycles) {
    return detail::RunAwaiter{[&machine, cycles] { return machine.runCycles(cycles); }};
}

// Drives a Task. step() makes run requests until `budget` cycles are spent
// (a request is never split, so it may overshoot) or the task is done.
template <class T = void>
class Job {
public:
    Job() = default;
    explicit Job(Task<T> task) : m_task(std::move(task)) {}

    bool valid() const { return m_task.handle() != nullptr; }
    bool done() const { return m_task.done(); }

    // Returns done(). Exceptions from the task surface in result().
    bool step(std::uint64_t budget) {
        if (done()) return true;
        Pending* const outer = detail::tl_pending;
        detail::tl_pending = &m_pending;
        struct Restore {
            Pending* outer;
            ~Restore() { detail::tl_pending = outer; }
        } restore{outer};

        if (!m_started) {
            m_started = true;
            m_task.handle().resume();
        }
        std::uint64_t spent = 0;
        while (m_pending && spent < budget) {
            const std::uint64_t ran = m_pending.run();
            *m_pending.result = ran;
            spent += ran == 0 ? 1 : ran;  // a stalled machine still makes progress here
            const std::coroutine_handle<> next = m_pending.resume;
            m_pending = Pending{};
            next.resume();
        }
        return done();
    }

    // The task's result (rethrows what it threw). Only once, after done().
    T result() { return m_task.handle().promise().take(); }

private:
    Task<T> m_task;
    Pending m_pending;
    bool m_started = false;
};

// Runs `task` straight through: the blocking form every Core wrapper uses.
template <class T>
T runToCompletion(Task<T> task) {
    Job<T> j(std::move(task));
    while (!j.step(std::numeric_limits<std::uint64_t>::max())) {
    }
    return j.result();
}

} // namespace job
