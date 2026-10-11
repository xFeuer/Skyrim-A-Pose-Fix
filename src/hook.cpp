#include "hook.h"
#include "RE/H/hkbBehaviorReferenceGenerator.h"
#include "converter.h"
#include "paths.h"

namespace APoseFix
{
    RE::hkbBehaviorGraph *LoadBehaviorGraphHook::Load(const char *a_folderRoot, const char *a_behaviorFilePath, uint64_t a_assetLoader, void *a_unk04)
    {
        auto result =  _Load(a_folderRoot, a_behaviorFilePath, a_assetLoader, a_unk04);
        
        if (!result && a_folderRoot && a_behaviorFilePath && a_folderRoot[0] != '\0' && a_behaviorFilePath[0] != '\0')
        {
            std::string convertedFileName = std::format("{}_packed", a_behaviorFilePath);
            std::filesystem::path dataPath = std::filesystem::current_path() / "Data";
            std::filesystem::path hkxcPath = dataPath / "hkxc.exe";
            std::filesystem::path path = dataPath / "Meshes" / a_folderRoot / a_behaviorFilePath;
            std::filesystem::path convertedPath{ path }; 
            convertedPath.replace_filename(std::format("{}_packed.hkx", path.stem().string()));
            if (std::filesystem::exists(path))
            {
                if (Converter::PortHavokFile(path, convertedPath))
                {
                    std::error_code removeErrorCode;
                    // std::filesystem::remove(path, removeErrorCode);
                    std::error_code renameErrorCode;
                    std::filesystem::copy(convertedPath, path, std::filesystem::copy_options::overwrite_existing, renameErrorCode);
                    if (removeErrorCode.value() == 0 && renameErrorCode.value() == 0)
                    {
                        std::filesystem::path behaviorPath(a_behaviorFilePath);
                        std::string convertedBehaviorFileName = std::format("{}\\{}_packed.hkx",
                                                                            behaviorPath.parent_path().string(),
                                                                            behaviorPath.stem().string());
                        result = _Load(a_folderRoot, convertedBehaviorFileName.c_str(), a_assetLoader, a_unk04);
                        if (result)
                        {
                            SKSE::log::info("Behavior Graph {}\\{} loaded after conversion", a_folderRoot, convertedFileName);
                            return result;
                        }
                        SKSE::log::error("Behavior Graph {}\\{} still not loadable after conversion", a_folderRoot, a_behaviorFilePath);
                    }
                    else
                    {
                        SKSE::log::error("Failed to rename converted behavior graph from {} to {}: {}", convertedPath.string(), path.string(), renameErrorCode.message());
                        SKSE::log::error("Or failed to remove original behavior graph {}: {}", path.string(), removeErrorCode.message());
                    }
                }
                else
                {
                    SKSE::log::error("Failed to convert behavior graph {} to {}", path.string(), convertedPath.string());
                }
            }
            SKSE::log::info("Behavior Graph {}\\{} not loadable, redirecting to dummy behavior", a_folderRoot, a_behaviorFilePath);
            a_folderRoot = "Actors\\Character";
            a_behaviorFilePath = "Behaviors\\dummybehavior.hkx";
            SKSE::log::info("Redirected to {}\\{}", a_folderRoot, a_behaviorFilePath);
            return _Load(a_folderRoot, a_behaviorFilePath, a_assetLoader, a_unk04);
        }
        // SKSE::log::info("Behavior Graph {}\\{} loaded successfully", a_folderRoot, a_behaviorFilePath);
        return result;
    }

    RE::hkbBehaviorGraph *LoadBehaviorProjectHook::LoadBehaviorProject(const char *folder, const char *name, RE::hkbCharacter &hchar, uint64_t project_data_handle, RE::BSFixedString &filename, RE::BSScrapArray<RE::hkbBehaviorGraph *> &hgraphs)
    {
        auto result = _LoadBehaviorProject(folder, name, hchar, project_data_handle, filename, hgraphs);
        PathResolver::RegisterParentPath(filename.c_str(), folder);
        return result; 
    }
    bool ValidClipHook::IsValidClip(RE::AnimationFileManagerSingleton *a_manager, const RE::hkbContext &a_context, uint16_t animationBindingIndex, RE::BSSynchronizedClipGenerator *a_synchronizedClipGenerator, void *unk05, uint32_t *a_outUserData)
    {
        auto result =  _IsValidClip(a_manager, a_context, animationBindingIndex, a_synchronizedClipGenerator, unk05, a_outUserData);
        if (*a_outUserData == 0xC)
        {
            return result;
        }
        if (animationBindingIndex < 0)
        {
            return result;
        }
        auto *character = a_context.character;
        if (!character)
        {
            return result;
        }
        const RE::BShkbAnimationGraph *graph = stl::adjust_pointer<RE::BShkbAnimationGraph>(a_context.character, -0xC0);
        if (!graph)
        {
            return result;
        }
        auto setup = character->setup;
        if (!setup)
        {
            return result;
        }
        auto data = setup->data;
        if (!data)
        {
            return result;
        }
        auto stringData = data->stringData;
        if (!stringData)
        {
            return result;
        }
        auto &animationBundleNames = stringData->animationNames;
        if (animationBundleNames.size() <= animationBindingIndex)
        {
            return result;
        }
        auto *animationFilename = animationBundleNames[animationBindingIndex].data();
        std::filesystem::path animationPath;
        if (!PathResolver::GetAbsoluteFolderPath(graph->projectName.c_str(), animationFilename, animationPath))
        {
            return result;
        }
        if (std::filesystem::is_directory(animationPath) || !std::filesystem::exists(animationPath))
        {
            SKSE::log::info("Animation file does not exist: {}", animationPath.string());
            return result;
        }
        if (portedAnimationIndices.contains(animationBindingIndex) )
        {
            SKSE::log::info("Animation file at index {} already converted: {}", animationBindingIndex, animationPath.string());
            return result;
        }
        // to-do: use map of already converted animation indices to skip redundant conversions
        // add ae addresses
        if (!Converter::PortHavokFile(animationPath))
        {
            SKSE::log::info("Failed to convert animation file: {}", animationPath.string());
            return result;
        }
        portedAnimationIndices.emplace(animationBindingIndex);
        SKSE::log::info("Converted animation file: {}", animationPath.string());
        return result;
        // returning true and 0xC might make animation chaining idk
    }
}
