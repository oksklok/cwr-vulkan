#pragma once

namespace Poseidon::vk
{
// Backend-local verdict, independent of window-open state and logging thresholds.
inline int SmokeTestExitCode(unsigned frames, bool backendFailed, unsigned validationErrors, bool loggedErrors) noexcept
{
    return frames && !backendFailed && !validationErrors && !loggedErrors ? 0 : 1;
}
} // namespace Poseidon::vk
