#pragma once

#include "editor/AuthoringDocument.hpp"
#include "scene/CookedWorld.hpp"

#include <filesystem>

namespace ve::editor {

[[nodiscard]] CookedWorld
cookAuthoringDocument(const AuthoringDocument &document,
                      const CookedWorldLimits &limits = {});
void cookAuthoringDocumentToFile(const AuthoringDocument &document,
                                 const std::filesystem::path &path,
                                 const CookedWorldLimits &limits = {});

} // namespace ve::editor
