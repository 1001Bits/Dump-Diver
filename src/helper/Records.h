#pragma once

#include "helper/Snapshot.h"

#include <windows.h>

#include <string>
#include <unordered_map>

namespace dd::helper
{
    // Offsets of the few game fields Dump Diver reads. TESForm's header is the same in both
    // games; the rest differs per game and is checked before use (plugin names must look like
    // plugin names), so a layout change yields no records rather than wrong ones.
    struct GameLayout
    {
        std::uint32_t fileName = 0;   // TESFile::fileName
        std::uint32_t refBase = 0;    // TESObjectREFR::data.objectReference
        std::uint32_t refCell = 0;    // TESObjectREFR::parentCell
        std::uint32_t npcFace = 0;    // TESNPC::faceNPC (0 = not checked)
    };
    GameLayout LayoutFor(Game game);

    // ".?AVTESObjectREFR@@" -> "TESObjectREFR", ".?AVInner@Outer@@" -> "Outer::Inner"
    std::wstring Demangle(const std::string& mangled);

    bool IsPluginName(const std::string& name);

    // Reads RTTI through ReadProcessMemory: the class of an object and its base classes.
    class RttiReader
    {
    public:
        struct ClassInfo
        {
            bool         valid = false;
            std::wstring name;
            bool         isForm = false;       // derives from TESForm
            bool         isReference = false;  // derives from TESObjectREFR
        };

        RttiReader(HANDLE process, std::uint64_t imageBase, std::uint64_t imageEnd) :
            process_(process),
            base_(imageBase),
            end_(imageEnd)
        {}

        const ClassInfo& Classify(std::uint64_t vtable);

    private:
        std::string TypeName(std::int32_t descriptorRva);

        HANDLE                                       process_;
        std::uint64_t                                base_;
        std::uint64_t                                end_;
        std::unordered_map<std::uint64_t, ClassInfo> classes_;
    };

    class FormResolver
    {
    public:
        FormResolver(HANDLE process, const Module& exe, Game game) :
            process_(process),
            rtti_(process, exe.base, exe.base + exe.size),
            layout_(LayoutFor(game))
        {}

        // Index into snapshot.forms, or -1 when the address is not a game record.
        int Resolve(std::uint64_t address, Snapshot& snapshot, int depth = 0);
        std::wstring CheckFaceChain(std::uint64_t npc);

    private:
        HANDLE                                 process_;
        RttiReader                             rtti_;
        GameLayout                             layout_;
        std::unordered_map<std::uint64_t, int> known_;
        std::unordered_map<std::uint64_t, std::wstring> fileNames_;
    };
}
