#pragma once
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <unordered_set>
#include <Windows.h>
#include "RE/H/hkbBehaviorReferenceGenerator.h"
namespace APoseFix
{
    // Every hook below replaces one 5-byte `call` inside a game function, at a fixed offset from the start of
    // that function. The offsets were read from one build of the game (SE 1.5.97 / AE 1.6.1170). A game update
    // can recompile the function and move the call; patching the old offset then overwrites the middle of some
    // other instruction and the game crashes the first time that code runs.
    //
    // So before a call is redirected, look at what is actually there. A hook is only installed when its site
    // still is `E8 rel32` to the function the hook wraps, or to the trampoline stub of a plugin that hooked the
    // same call first. Otherwise the game code is left alone and the reason is written to the log.
    namespace detail
    {
        inline constexpr std::uint8_t kCallRel32 = 0xE8;
        inline constexpr std::size_t  kCallSize = 5;

        enum class CallSite
        {
            kExpected,       // call <the function this hook wraps>
            kHookedByOther,  // call <trampoline stub of another plugin>: it got here first, chaining to it is fine
            kMismatch        // anything else: not a call, or a call to something we do not recognise
        };

        [[nodiscard]] inline bool IsGameCode(std::uintptr_t a_address)
        {
            const auto text = REL::Module::get().segment(REL::Segment::textx);
            return a_address >= text.address() && a_address - text.address() < text.size();
        }

        // True when a_size bytes at a_address are committed memory that may be read and executed.
        [[nodiscard]] inline bool IsReadableCode(std::uintptr_t a_address, std::size_t a_size)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (::VirtualQuery(reinterpret_cast<const void*>(a_address), &info, sizeof(info)) == 0) {
                return false;
            }
            const bool readableCode = info.Protect == PAGE_EXECUTE_READ || info.Protect == PAGE_EXECUTE_READWRITE || info.Protect == PAGE_EXECUTE_WRITECOPY;
            if (info.State != MEM_COMMIT || !readableCode) {
                return false;
            }
            const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            return regionEnd - a_address >= a_size;
        }

        // A 5-byte call that another plugin redirected goes to a stub in memory allocated near the game:
        //     FF 25 <disp32>    jmp qword ptr [rip+disp32]
        // which jumps through the 8-byte address of that plugin's hook, stored next to the instruction. SKSE and
        // CommonLib store it right after the instruction (disp32 = 0), DKUtil right before it (disp32 = -14).
        // Checking for the stub matters: an E8 byte in the middle of some other instruction also "calls" an
        // address outside the game's code, but not one that holds such a stub.
        [[nodiscard]] inline bool IsTrampolineStub(std::uintptr_t a_address)
        {
            constexpr std::size_t jmpSize = 6;
            if (IsGameCode(a_address) || !IsReadableCode(a_address, jmpSize)) {
                return false;
            }
            const auto* code = reinterpret_cast<const std::uint8_t*>(a_address);
            if (code[0] != 0xFF || code[1] != 0x25) {
                return false;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement, code + 2, sizeof(displacement));
            const auto slot = a_address + jmpSize + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(displacement));
            // the 8-byte address it jumps through must sit in the same kind of memory as the stub itself
            return IsReadableCode(slot, sizeof(std::uint64_t));
        }

        // Destination of the `call rel32` at a_site. Only meaningful when the byte at a_site is kCallRel32.
        [[nodiscard]] inline std::uintptr_t CallDestination(std::uintptr_t a_site) noexcept
        {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, reinterpret_cast<const void*>(a_site + 1), sizeof(displacement));
            return a_site + kCallSize + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(displacement));
        }

        [[nodiscard]] inline bool IsCall(std::uintptr_t a_site) noexcept
        {
            return *reinterpret_cast<const std::uint8_t*>(a_site) == kCallRel32;
        }

        [[nodiscard]] inline CallSite InspectCallSite(std::uintptr_t a_site, std::uintptr_t a_expectedCallee)
        {
            if (!IsCall(a_site)) {
                return CallSite::kMismatch;
            }
            const auto destination = CallDestination(a_site);
            if (destination == a_expectedCallee) {
                return CallSite::kExpected;
            }
            return IsTrampolineStub(destination) ? CallSite::kHookedByOther : CallSite::kMismatch;
        }

        [[nodiscard]] inline std::string HexDump(std::uintptr_t a_address, std::size_t a_count)
        {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
            std::string text;
            for (std::size_t i = 0; i < a_count; ++i) {
                text += std::format("{:02X} ", bytes[i]);
            }
            return text;
        }

        // Offsets (from a_function) of every `call a_callee` in the a_length bytes that start at a_function, as text.
        // Only used for the log, so that a call that moved can be found again without a disassembler.
        [[nodiscard]] inline std::string FindCallsTo(std::uintptr_t a_function, std::size_t a_length, std::uintptr_t a_callee)
        {
            std::string text;
            for (std::size_t offset = 0; offset + kCallSize <= a_length; ++offset) {
                const auto site = a_function + offset;
                if (IsCall(site) && CallDestination(site) == a_callee) {
                    text += std::format("+0x{:X} ", offset);
                }
            }
            return text.empty() ? std::string("none") : text;
        }

        // True when the call at a_site, inside the game function a_function, may be redirected.
        // Logs the outcome either way.
        [[nodiscard]] inline bool CanHook(std::string_view a_name, std::uintptr_t a_function, std::uintptr_t a_site, std::uintptr_t a_expectedCallee)
        {
            // How far from the start of the function to look for the call when it is not where it should be. This
            // is more than the two shorter hooked functions used to measure (0x170 and 0x1A0 bytes), so a function
            // that grew is still covered; the search can therefore run into the function that follows.
            const std::size_t searchLength = 0x200;

            const std::uintptr_t siteRVA = a_site - REL::Module::get().base();
            const std::uintptr_t siteOffset = a_site - a_function;
            switch (InspectCallSite(a_site, a_expectedCallee)) {
            case CallSite::kExpected:
                SKSE::log::info("{}: call found at SkyrimSE.exe+{:X} (function+0x{:X}), installing hook", a_name, siteRVA, siteOffset);
                return true;
            case CallSite::kHookedByOther:
                SKSE::log::info("{}: call at SkyrimSE.exe+{:X} is already redirected by another plugin, chaining to it", a_name, siteRVA);
                return true;
            default:
                SKSE::log::error("{}: unexpected code at SkyrimSE.exe+{:X} (function+0x{:X}), hook NOT installed: the game changed here, or something this plugin does not recognise patched it", a_name, siteRVA, siteOffset);
                SKSE::log::error("{}: bytes at the expected call: {}", a_name, HexDump(a_site, 16));
                SKSE::log::error("{}: calls to the wrapped function within 0x{:X} bytes of the start of the function: {}", a_name, searchLength, FindCallsTo(a_function, searchLength, a_expectedCallee));
                return false;
            }
        }
    }

    class LoadBehaviorGraphHook
    {
        public:
        static bool Install()
        {
            // Down	p	sub_140B06D20+11B	call    sub_140B06D20
            //	p	sub_140BC9070+10B	call    sub_140BC9070 AE
            // the function calls itself here, so the function the hook wraps is the hooked function
            REL::Relocation<std::uintptr_t> caller{REL::RelocationID(63030, 63948)};
            REL::Relocation<std::uintptr_t> target{REL::RelocationID(63030, 63948), REL::Relocate(0x11B, 0x10B)};
            if (!detail::CanHook("LoadBehaviorGraph", caller.address(), target.address(), caller.address()))
            {
                return false;
            }
            auto &trampoline = SKSE::GetTrampoline();
            _Load = trampoline.write_call<5>(target.address(), Load);
            return true;
        }
        private:
        static RE::hkbBehaviorGraph* Load(const char *a_folderRoot, const char *a_behaviorFilePath, uint64_t a_assetLoader, void* a_unk04);
        static inline REL::Relocation<decltype(Load)> _Load;
    };


    // Since we don't know the project folder paths, we hook project load to map them
    // Up	p	BShkbAnimationGraph__sub_140AEDD10+108	call    sub_140AFFFE0
    //	p	sub_140BB0800:loc_140BB0911	call    sub_140BC1B10
    class LoadBehaviorProjectHook
    {
        public:
        static bool Install()
        {
            // the wrapped function: sub_140AFFFE0 (SE 1.5.97) is id 62921, sub_140BC1B10 (AE 1.6.1170) is id 63844
            REL::Relocation<std::uintptr_t> caller{ REL::RelocationID(62640, 63585) };
            REL::Relocation<std::uintptr_t> target{ REL::RelocationID(62640, 63585), REL::Relocate(0x108, 0x111) };
            REL::Relocation<std::uintptr_t> callee{ REL::RelocationID(62921, 63844) };
            if (!detail::CanHook("LoadBehaviorProject", caller.address(), target.address(), callee.address()))
            {
                return false;
            }
            auto &trampoline = SKSE::GetTrampoline();
            _LoadBehaviorProject = trampoline.write_call<5>(target.address(), LoadBehaviorProject);
            return true;
        }
        private:
        static RE::hkbBehaviorGraph* LoadBehaviorProject(const char* folder, const char* name, RE::hkbCharacter& hchar, uint64_t project_data_handle, RE::BSFixedString& filename, RE::BSScrapArray<RE::hkbBehaviorGraph*>& hgraphs);
        static inline REL::Relocation<decltype(LoadBehaviorProject)> _LoadBehaviorProject;
    };



    /*Up	p	AnimationFileManagerSingleton__Func2_140B0A150+B6	call    AnimationFileManagerSingleton__sub_140B0A5C0
    	p	AnimationFileManagerSingleton__Func1_140B09FB0+89	call    AnimationFileManagerSingleton__sub_140B0A5C0*/
        // Up	p	sub_140BCBB90:loc_140BCBC1B	call    sub_140BCC220
   class ValidClipHook
   {

    public:
    static bool Install()
    {
        // the wrapped function: sub_140B0A5C0 (SE 1.5.97) is id 63075, sub_140BCC220 (AE 1.6.1170) is id 63987
        REL::Relocation<std::uintptr_t> caller{ REL::RelocationID(63069, 63981) };
        REL::Relocation<std::uintptr_t> target{ REL::RelocationID(63069, 63981), REL::Relocate(0x89, 0x8B) };
        REL::Relocation<std::uintptr_t> callee{ REL::RelocationID(63075, 63987) };
        if (!detail::CanHook("IsValidClip", caller.address(), target.address(), callee.address()))
        {
            return false;
        }
        auto &trampoline = SKSE::GetTrampoline();
        _IsValidClip = trampoline.write_call<5>(target.address(), IsValidClip);
        return true;
    }
    private:
    /*
    (unsigned __int8)AnimationFileManagerSingleton::sub_140B0A5C0(
                          a1,
                          (void *)a2,
                          *(_WORD *)&a3[1]._pad_14[4],
                          (BSSynchronizedClipGenerator *)a4,
                          &a2a,
                          &a6)
    */
    static bool IsValidClip(RE::AnimationFileManagerSingleton* a_manager, const RE::hkbContext& a_context, uint16_t animationBindingIndex, RE::BSSynchronizedClipGenerator* a_synchronizedClipGenerator,
    void* unk05, uint32_t *a_outUserData);
    static inline REL::Relocation<decltype(IsValidClip)> _IsValidClip;
    static inline std::unordered_set<uint16_t> portedAnimationIndices; // we don't need a lock because Converter already throttles
   };


    static void InstallHooks()
    {
        SKSE::log::info("game runtime {}", REL::Module::get().version().string("."));
        SKSE::AllocTrampoline(14 * 3);
        int installed = 0;
        installed += ValidClipHook::Install() ? 1 : 0;
        installed += LoadBehaviorGraphHook::Install() ? 1 : 0;
        installed += LoadBehaviorProjectHook::Install() ? 1 : 0;
        if (installed == 3)
        {
            SKSE::log::info("all 3 hooks installed");
        }
        else
        {
            SKSE::log::error("only {} of 3 hooks installed, see the errors above", installed);
        }
    }
}
