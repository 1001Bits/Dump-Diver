#include "plugin/TaskTracking.h"

#include "plugin/Log.h"
#include "plugin/Plugin.h"

#include <intrin.h>

#include <new>

namespace dd::plugin
{
    namespace
    {
        using AddFn = void (*)(void*);

        AddFn g_addTask = nullptr;
        AddFn g_addUITask = nullptr;

        thread_local int t_slot = -2;  // -2 not assigned yet, -1 no slot left

        TaskSlot* ThreadSlot()
        {
            Header* shared = Shared();
            if (!shared)
                return nullptr;
            if (t_slot == -2) {
                const LONG index =
                    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shared->taskSlotCount)) - 1;
                if (index >= 0 && static_cast<std::uint32_t>(index) < kTaskSlots) {
                    t_slot = index;
                    shared->tasks[index].threadId = GetCurrentThreadId();
                } else {
                    t_slot = -1;
                }
            }
            return t_slot >= 0 ? &shared->tasks[t_slot] : nullptr;
        }

        struct SlotValues
        {
            std::uint64_t startQpc;
            std::uint64_t runAddress;
            std::uint64_t callerAddress;
            std::uint64_t queuedQpc;
            std::uint32_t kind;
        };

        void WriteSlot(TaskSlot& slot, const SlotValues& values)
        {
            slot.sequence = slot.sequence + 1;  // odd: being written
            _WriteBarrier();
            slot.startQpc = values.startQpc;
            slot.runAddress = values.runAddress;
            slot.callerAddress = values.callerAddress;
            slot.queuedQpc = values.queuedQpc;
            slot.kind = values.kind;
            _WriteBarrier();
            slot.sequence = slot.sequence + 1;
        }

        std::uint64_t RunAddressOf(xse::Task* task)
        {
            auto* const* vtable = *reinterpret_cast<void* const* const*>(task);
            return reinterpret_cast<std::uint64_t>(vtable[xse::kTaskRunSlot]);
        }

        // Marks the current thread as running a tracked task for the lifetime of the object.
        class RunScope
        {
        public:
            RunScope(const SlotValues& values) :
                slot_(ThreadSlot())
            {
                if (!slot_)
                    return;
                saved_ = { slot_->startQpc, slot_->runAddress, slot_->callerAddress, slot_->queuedQpc, slot_->kind };
                slot_->depth = slot_->depth + 1;
                WriteSlot(*slot_, values);
            }

            ~RunScope()
            {
                if (!slot_)
                    return;
                WriteSlot(*slot_, saved_);
                slot_->depth = slot_->depth - 1;
                slot_->completed = slot_->completed + 1;
            }

            RunScope(const RunScope&) = delete;
            RunScope& operator=(const RunScope&) = delete;

        private:
            TaskSlot*  slot_;
            SlotValues saved_{};
        };

        class TrackedTask final : public xse::Task
        {
        public:
            TrackedTask(xse::Task* inner, void* caller, std::uint32_t kind) :
                inner_(inner),
                runAddress_(RunAddressOf(inner)),
                caller_(reinterpret_cast<std::uint64_t>(caller)),
                queuedQpc_(Qpc()),
                kind_(kind)
            {}

#if defined(DD_GAME_SKYRIM)
            void Run() override
            {
                RunScope scope({ Qpc(), runAddress_, caller_, queuedQpc_, kind_ });
                inner_->Run();
            }

            void Dispose() override
            {
                inner_->Dispose();
                delete this;
            }
#else
            ~TrackedTask() override { delete inner_; }

            void Run() override
            {
                RunScope scope({ Qpc(), runAddress_, caller_, queuedQpc_, kind_ });
                inner_->Run();
            }
#endif

        private:
            xse::Task*    inner_;
            std::uint64_t runAddress_;
            std::uint64_t caller_;
            std::uint64_t queuedQpc_;
            std::uint32_t kind_;
        };

        void Forward(AddFn original, void* task, void* caller, std::uint32_t kind)
        {
            if (!task) {
                original(task);
                return;
            }
            auto* wrapper = new (std::nothrow) TrackedTask(static_cast<xse::Task*>(task), caller, kind);
            if (!wrapper) {
                original(task);
                return;
            }
            if (Header* shared = Shared())
                InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&shared->tasksQueued));
            original(wrapper);
        }

        __declspec(noinline) void HookAddTask(void* task) { Forward(g_addTask, task, _ReturnAddress(), 0); }

        __declspec(noinline) void HookAddUITask(void* task) { Forward(g_addUITask, task, _ReturnAddress(), 1); }
    }

    bool InitTasks(xse::TaskInterface* tasks)
    {
        if (!tasks || !tasks->AddTask || !tasks->AddUITask)
            return false;
        g_addTask = tasks->AddTask;
        g_addUITask = tasks->AddUITask;
        return true;
    }

    bool InstallTaskTracking(xse::TaskInterface* tasks)
    {
        if (!tasks || !g_addTask || tasks->AddTask != g_addTask || tasks->AddUITask != g_addUITask)
            return false;

        DWORD old = 0;
        if (!VirtualProtect(&tasks->AddTask, sizeof(void*) * 2, PAGE_READWRITE, &old)) {
            DD_LOG_WARN("task tracking off: cannot write the task interface (error %lu)", GetLastError());
            return false;
        }
        tasks->AddTask = &HookAddTask;
        tasks->AddUITask = &HookAddUITask;
        VirtualProtect(&tasks->AddTask, sizeof(void*) * 2, old, &old);
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        return true;
    }

    bool QueueUntracked(xse::Task* task)
    {
        if (!g_addTask)
            return false;
        g_addTask(task);
        return true;
    }
}
