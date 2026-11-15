#include "helper/Records.h"

#include "helper/Process.h"
#include "helper/Util.h"

#include <algorithm>
#include <cstring>

namespace dd::helper
{
    GameLayout LayoutFor(Game game)
    {
        if (game == Game::kFallout4)
            return { 0x70, 0xC0 + 0x20, 0xB8, 0 };
        return { 0x58, 0x40, 0x60, 0x1F0 };
    }

    std::wstring Demangle(const std::string& mangled)
    {
        std::string name = mangled;
        if (name.rfind(".?AV", 0) == 0 || name.rfind(".?AU", 0) == 0)
            name = name.substr(4);
        if (name.size() >= 2 && name.compare(name.size() - 2, 2, "@@") == 0)
            name.resize(name.size() - 2);
        // "Inner@Outer" -> "Outer::Inner"
        std::string out;
        std::size_t end = name.size();
        while (true) {
            const std::size_t at = name.rfind('@', end == 0 ? 0 : end - 1);
            const std::size_t start = at == std::string::npos ? 0 : at + 1;
            if (!out.empty())
                out += "::";
            out += name.substr(start, end - start);
            if (at == std::string::npos || at == 0)
                break;
            end = at;
        }
        return Wide(out);
    }

    bool IsPluginName(const std::string& name)
    {
        if (name.size() < 5 || name.size() > 255)
            return false;
        for (const char c : name) {
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
        }
        std::string extension = name.substr(name.size() - 4);
        for (auto& c : extension)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return extension == ".esm" || extension == ".esp" || extension == ".esl";
    }

    std::string RttiReader::TypeName(std::int32_t descriptorRva)
    {
        char name[160]{};
        if (!ReadMemory(process_, base_ + static_cast<std::uint64_t>(descriptorRva) + 0x10, name, sizeof(name) - 1))
            return {};
        name[sizeof(name) - 1] = 0;
        return name;
    }

    const RttiReader::ClassInfo& RttiReader::Classify(std::uint64_t vtable)
    {
        const auto known = classes_.find(vtable);
        if (known != classes_.end())
            return known->second;
        ClassInfo& info = classes_[vtable];
        if (vtable < base_ + 8 || vtable >= end_)
            return info;

        std::uint64_t locatorAddress = 0;
        if (!ReadMemory(process_, vtable - 8, &locatorAddress, 8) || locatorAddress < base_ || locatorAddress >= end_)
            return info;
        std::int32_t locator[6]{};
        if (!ReadMemory(process_, locatorAddress, locator, sizeof(locator)))
            return info;
        // signature 1 (x64); the self RVA must lead back to this image
        if (locator[0] != 1 || base_ + static_cast<std::uint64_t>(static_cast<std::uint32_t>(locator[5])) != locatorAddress)
            return info;
        const std::string mangled = TypeName(locator[3]);
        if (mangled.rfind(".?A", 0) != 0)
            return info;
        info.valid = true;
        info.name = Demangle(mangled);

        // Base classes from the class hierarchy descriptor.
        std::uint32_t hierarchy[4]{};
        if (!ReadMemory(process_, base_ + static_cast<std::uint32_t>(locator[4]), hierarchy, sizeof(hierarchy)))
            return info;
        const std::uint32_t count = hierarchy[2];
        if (count == 0 || count > 64)
            return info;
        std::int32_t bases[64]{};
        if (!ReadMemory(process_, base_ + hierarchy[3], bases, count * sizeof(std::int32_t)))
            return info;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::int32_t baseDescriptor[1]{};
            if (!ReadMemory(process_, base_ + static_cast<std::uint32_t>(bases[i]), baseDescriptor, sizeof(baseDescriptor)))
                continue;
            const std::string baseName = TypeName(baseDescriptor[0]);
            if (baseName == ".?AVTESForm@@")
                info.isForm = true;
            else if (baseName == ".?AVTESObjectREFR@@")
                info.isReference = true;
        }
        return info;
    }

    // An NPC takes its face from its face NPC, and the game follows that chain to the end; a chain that
    // comes back to an NPC already seen never ends.
    std::wstring FormResolver::CheckFaceChain(std::uint64_t npc)
    {
        std::vector<std::uint64_t> seen{ npc };
        std::uint64_t              current = npc;
        for (int step = 0; step < 16; ++step) {
            std::uint64_t next = 0;
            if (!ReadMemory(process_, current + layout_.npcFace, &next, 8) || !next)
                return {};
            if (std::find(seen.begin(), seen.end(), next) != seen.end()) {
                return next == npc && step == 0 ? L"its face NPC is itself, so loading its face never ends"
                                                : L"its face NPCs form a loop, so loading its face never ends";
            }
            std::uint64_t vtable = 0;
            if (!ReadMemory(process_, next, &vtable, 8) || rtti_.Classify(vtable).name != L"TESNPC")
                return {};
            seen.push_back(next);
            current = next;
        }
        return {};
    }

    int FormResolver::Resolve(std::uint64_t address, Snapshot& snapshot, int depth)
    {
        if (address < 0x10000 || (address & 7))
            return -1;
        const auto known = known_.find(address);
        if (known != known_.end())
            return known->second;
        known_[address] = -1;

        std::uint8_t header[0x20]{};
        if (!ReadMemory(process_, address, header, sizeof(header)))
            return -1;
        std::uint64_t vtable = 0;
        std::memcpy(&vtable, header, 8);
        const auto& cls = rtti_.Classify(vtable);
        if (!cls.valid || !cls.isForm)
            return -1;

        FormInfo form;
        form.address = address;
        std::memcpy(&form.formId, header + 0x14, 4);
        form.formType = header[0x1A];
        form.className = cls.name;
        if (!form.formId)
            return -1;

        std::uint64_t fileArray = 0;
        std::memcpy(&fileArray, header + 0x08, 8);
        if (fileArray) {
            struct
            {
                std::uint64_t data;
                std::uint32_t size;
                std::uint32_t pad;
            } array{};
            if (ReadMemory(process_, fileArray, &array, sizeof(array)) && array.data && array.size && array.size <= 255) {
                std::vector<std::uint64_t> files(array.size);
                if (ReadMemory(process_, array.data, files.data(), files.size() * 8)) {
                    for (const std::uint64_t file : files) {
                        auto cached = fileNames_.find(file);
                        if (cached == fileNames_.end()) {
                            char name[261]{};
                            std::wstring value;
                            if (file && ReadMemory(process_, file + layout_.fileName, name, 260)) {
                                name[260] = 0;
                                if (IsPluginName(name))
                                    value = Wide(name);
                            }
                            cached = fileNames_.emplace(file, value).first;
                        }
                        if (cached->second.empty()) {
                            form.files.clear();  // a file that does not read right: trust none of them
                            break;
                        }
                        form.files.push_back(cached->second);
                    }
                }
            }
        }

        if (layout_.npcFace && form.className == L"TESNPC")
            form.problem = CheckFaceChain(address);

        const int index = static_cast<int>(snapshot.forms.size());
        snapshot.forms.push_back(std::move(form));
        known_[address] = index;

        if (cls.isReference && depth == 0) {
            std::uint64_t base = 0, cell = 0;
            ReadMemory(process_, address + layout_.refBase, &base, 8);
            ReadMemory(process_, address + layout_.refCell, &cell, 8);
            const int baseIndex = base ? Resolve(base, snapshot, depth + 1) : -1;
            const int cellIndex = cell ? Resolve(cell, snapshot, depth + 1) : -1;
            snapshot.forms[static_cast<std::size_t>(index)].baseForm = baseIndex;
            snapshot.forms[static_cast<std::size_t>(index)].cell = cellIndex;
        }
        return index;
    }
}
