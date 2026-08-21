#include "skate3_release_updater.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <toml++/toml.hpp>

#include "third_party/rexglue-sdk/thirdparty/crypto/sha256.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace skate3 {
namespace {

constexpr std::string_view kDefaultManifestUrl =
    "https://raw.githubusercontent.com/SK8-ENGINE/"
    "SK8-Engine/main/release/update-manifest.toml";
constexpr std::uint64_t kMaximumManifestBytes = 64u * 1024u;
constexpr std::uint64_t kMaximumReleaseBytes =
    2ull * 1024ull * 1024ull * 1024ull;

struct UpdateManifest {
  std::string version;
  std::string asset_url;
  std::string sha256;
  std::uint64_t size = 0;
};

std::string Lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsSha256(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string Sha256OfFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  sha256::SHA256 hasher;
  std::array<char, 1024 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      hasher.add(buffer.data(), static_cast<std::size_t>(count));
    }
  }
  return input.eof() ? hasher.getHash() : std::string{};
}

std::optional<UpdateManifest> ParseManifest(std::string_view text,
                                            std::string &error) {
  try {
    const toml::table root = toml::parse(text);
    UpdateManifest manifest;
    manifest.version = root["version"].value_or("");
    manifest.asset_url = root["asset_url"].value_or("");
    manifest.sha256 = Lower(root["sha256"].value_or(""));
    manifest.size = root["size"].value_or<std::uint64_t>(0);
    if (manifest.version.empty() || manifest.asset_url.empty() ||
        !IsSha256(manifest.sha256) || manifest.size == 0 ||
        manifest.size > kMaximumReleaseBytes) {
      error = "The update manifest is incomplete or invalid.";
      return std::nullopt;
    }
    return manifest;
  } catch (const toml::parse_error &) {
    error = "The update manifest could not be parsed.";
    return std::nullopt;
  }
}

struct Version {
  std::array<std::uint64_t, 4> number{};
  std::string prerelease;
};

std::optional<Version> ParseVersion(std::string_view text) {
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
    text.remove_prefix(1);
  }
  Version result;
  const std::size_t dash = text.find('-');
  std::string_view numeric = text.substr(0, dash);
  if (dash != std::string_view::npos) {
    result.prerelease = Lower(std::string(text.substr(dash + 1)));
  }
  std::size_t part = 0;
  while (!numeric.empty() && part < result.number.size()) {
    const std::size_t dot = numeric.find('.');
    const std::string_view token = numeric.substr(0, dot);
    if (token.empty() ||
        !std::all_of(token.begin(), token.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
      return std::nullopt;
    }
    std::uint64_t value = 0;
    for (char c : token) {
      if (value > (UINT64_MAX - static_cast<unsigned>(c - '0')) / 10u) {
        return std::nullopt;
      }
      value = value * 10u + static_cast<unsigned>(c - '0');
    }
    result.number[part++] = value;
    if (dot == std::string_view::npos) {
      numeric = {};
    } else {
      numeric.remove_prefix(dot + 1);
    }
  }
  if (part == 0 || !numeric.empty()) {
    return std::nullopt;
  }
  return result;
}

int CompareVersions(std::string_view left, std::string_view right) {
  const auto a = ParseVersion(left);
  const auto b = ParseVersion(right);
  if (!a || !b) {
    return left == right ? 0 : -1;
  }
  if (a->number != b->number) {
    return a->number < b->number ? -1 : 1;
  }
  if (a->prerelease.empty() != b->prerelease.empty()) {
    return a->prerelease.empty() ? 1 : -1;
  }
  if (a->prerelease == b->prerelease) {
    return 0;
  }
  std::string_view a_pre = a->prerelease;
  std::string_view b_pre = b->prerelease;
  for (;;) {
    const auto a_dot = a_pre.find('.');
    const auto b_dot = b_pre.find('.');
    const std::string_view a_part = a_pre.substr(0, a_dot);
    const std::string_view b_part = b_pre.substr(0, b_dot);
    const bool a_numeric =
        !a_part.empty() &&
        std::all_of(a_part.begin(), a_part.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; });
    const bool b_numeric =
        !b_part.empty() &&
        std::all_of(b_part.begin(), b_part.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; });
    if (a_numeric && b_numeric) {
      const auto trim_zeroes = [](std::string_view value) {
        const auto first = value.find_first_not_of('0');
        return first == std::string_view::npos ? std::string_view("0")
                                               : value.substr(first);
      };
      const auto a_number = trim_zeroes(a_part);
      const auto b_number = trim_zeroes(b_part);
      if (a_number.size() != b_number.size()) {
        return a_number.size() < b_number.size() ? -1 : 1;
      }
      if (a_number != b_number) {
        return a_number < b_number ? -1 : 1;
      }
    } else if (a_numeric != b_numeric) {
      return a_numeric ? -1 : 1;
    } else if (a_part != b_part) {
      return a_part < b_part ? -1 : 1;
    }
    const bool a_done = a_dot == std::string_view::npos;
    const bool b_done = b_dot == std::string_view::npos;
    if (a_done || b_done) {
      if (a_done != b_done) {
        return a_done ? -1 : 1;
      }
      return 0;
    }
    a_pre.remove_prefix(a_dot + 1);
    b_pre.remove_prefix(b_dot + 1);
  }
}

#if defined(_WIN32)

struct WinHttpHandle {
  HINTERNET value = nullptr;
  ~WinHttpHandle() {
    if (value) {
      WinHttpCloseHandle(value);
    }
  }
  WinHttpHandle() = default;
  explicit WinHttpHandle(HINTERNET handle) : value(handle) {}
  WinHttpHandle(const WinHttpHandle &) = delete;
  WinHttpHandle &operator=(const WinHttpHandle &) = delete;
};

std::wstring Utf8ToWide(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(),
                          size) != size) {
    return {};
  }
  return result;
}

bool IsAllowedUrl(std::string_view value, bool manifest) {
  const std::wstring url = Utf8ToWide(value);
  if (url.empty()) {
    return false;
  }
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0,
                       &parts) ||
      parts.nScheme != INTERNET_SCHEME_HTTPS) {
    return false;
  }
  std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::transform(host.begin(), host.end(), host.begin(), towlower);
  if (manifest) {
    return host == L"raw.githubusercontent.com";
  }
  return host == L"github.com" ||
         host == L"release-assets.githubusercontent.com" ||
         host.ends_with(L".githubusercontent.com");
}

using ProgressCallback =
    std::function<void(std::uint64_t received, std::uint64_t total)>;

bool Download(std::string_view url, std::uint64_t maximum_bytes,
              std::vector<std::uint8_t> *memory,
              const std::filesystem::path *output_path,
              ProgressCallback progress, std::string &error) {
  const std::wstring wide_url = Utf8ToWide(url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (wide_url.empty() ||
      !WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0,
                       &parts) ||
      parts.nScheme != INTERNET_SCHEME_HTTPS) {
    error = "The update URL is invalid.";
    return false;
  }

  WinHttpHandle session(WinHttpOpen(L"Skate3CustomEngineLayer-Updater/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session.value) {
    error = "Windows could not initialize the update connection.";
    return false;
  }
  WinHttpSetTimeouts(session.value, 10000, 10000, 30000, 30000);
  std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  WinHttpHandle connection(
      WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
  if (!connection.value) {
    error = "The update server could not be reached.";
    return false;
  }
  std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength) {
    target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }
  WinHttpHandle request(WinHttpOpenRequest(
      connection.value, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request.value ||
      !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.value, nullptr)) {
    error = "The update download could not be started.";
    return false;
  }
  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(
          request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
          WINHTTP_NO_HEADER_INDEX) ||
      status != 200) {
    error = "The update server returned HTTP " + std::to_string(status) + ".";
    return false;
  }
  std::uint64_t total = 0;
  wchar_t length_buffer[64] = {};
  DWORD length_size = sizeof(length_buffer);
  if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH,
                          WINHTTP_HEADER_NAME_BY_INDEX, length_buffer,
                          &length_size, WINHTTP_NO_HEADER_INDEX)) {
    try {
      total = std::stoull(length_buffer);
    } catch (...) {
      total = 0;
    }
  }
  if (total > maximum_bytes) {
    error = "The update download exceeds the safety limit.";
    return false;
  }
  std::ofstream file;
  if (output_path) {
    file.open(*output_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      error = "The update archive could not be created.";
      return false;
    }
  }
  if (memory) {
    memory->clear();
    memory->reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(total, maximum_bytes)));
  }
  std::array<std::uint8_t, 64 * 1024> buffer{};
  std::uint64_t received = 0;
  for (;;) {
    DWORD read = 0;
    if (!WinHttpReadData(request.value, buffer.data(),
                         static_cast<DWORD>(buffer.size()), &read)) {
      error = "The update download was interrupted.";
      return false;
    }
    if (read == 0) {
      break;
    }
    if (received > maximum_bytes - read) {
      error = "The update download exceeds the safety limit.";
      return false;
    }
    received += read;
    if (memory) {
      memory->insert(memory->end(), buffer.begin(), buffer.begin() + read);
    }
    if (output_path) {
      file.write(reinterpret_cast<const char *>(buffer.data()), read);
      if (!file) {
        error = "The update archive could not be written.";
        return false;
      }
    }
    if (progress) {
      progress(received, total);
    }
  }
  if (total != 0 && received != total) {
    error = "The update download ended before it was complete.";
    return false;
  }
  return true;
}

std::wstring QuoteArgument(std::wstring_view value) {
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (wchar_t c : value) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    if (c == L'"') {
      result.append(slashes * 2 + 1, L'\\');
      result.push_back(L'"');
      slashes = 0;
      continue;
    }
    result.append(slashes, L'\\');
    slashes = 0;
    result.push_back(c);
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

bool WriteInstallerScript(const std::filesystem::path &script_path,
                          std::string &error) {
  static constexpr std::string_view kScript = R"PS1(
param(
  [Parameter(Mandatory=$true)][int]$ParentPid,
  [Parameter(Mandatory=$true)][string]$InstallRoot,
  [Parameter(Mandatory=$true)][string]$Archive,
  [Parameter(Mandatory=$true)][string]$ExpectedSha256
)
$ErrorActionPreference = 'Stop'
$work = Join-Path $InstallRoot '.cel-update'
$log = Join-Path $work 'update.log'
Start-Transcript -LiteralPath $log -Append | Out-Null
try {
  $deadline = [DateTime]::UtcNow.AddSeconds(90)
  while ((Get-Process -Id $ParentPid -ErrorAction SilentlyContinue) -and
         [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 250
  }
  if (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue) {
    throw 'The running game did not close in time.'
  }
  $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Archive).Hash
  if (-not $actual.Equals($ExpectedSha256,
      [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'The downloaded release failed its final integrity check.'
  }
  $stage = Join-Path $work 'stage'
  if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
  }
  Expand-Archive -LiteralPath $Archive -DestinationPath $stage -Force
  $roots = @(Get-ChildItem -LiteralPath $stage -Directory)
  if ($roots.Count -ne 1) {
    throw 'The release archive has an unexpected directory layout.'
  }
  $payload = $roots[0].FullName
  foreach ($required in @(
      'skate3.exe',
      'rexruntime.dll',
      'Blender Map Tools\owned_world_material_addon.zip')) {
    $requiredPath = Join-Path $payload $required
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
      throw "The release is missing $required."
    }
  }
  foreach ($item in Get-ChildItem -LiteralPath $payload) {
    if ($item.Name -in @('game', 'saves', 'settings.toml',
                         'active_map.txt', 'maps')) {
      continue
    }
    Copy-Item -LiteralPath $item.FullName -Destination $InstallRoot `
      -Recurse -Force
  }
  $mapSource = Join-Path $payload 'maps'
  $mapTarget = Join-Path $InstallRoot 'maps'
  if (Test-Path -LiteralPath $mapSource -PathType Container) {
    New-Item -ItemType Directory -Path $mapTarget -Force | Out-Null
    foreach ($name in @('README.txt', 'blender_bake_showcase.skate',
                        'blender_bake_showcase.blend')) {
      $source = Join-Path $mapSource $name
      if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $mapTarget $name) `
          -Force
      }
    }
  }
  Start-Process -FilePath (Join-Path $InstallRoot 'skate3.exe') `
    -WorkingDirectory $InstallRoot
} catch {
  $_ | Out-String | Add-Content -LiteralPath $log
} finally {
  Stop-Transcript -ErrorAction SilentlyContinue | Out-Null
}
)PS1";
  std::ofstream output(script_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "The update installer script could not be created.";
    return false;
  }
  output.write(kScript.data(), static_cast<std::streamsize>(kScript.size()));
  if (!output) {
    error = "The update installer script could not be written.";
    return false;
  }
  return true;
}

bool LaunchInstaller(const std::filesystem::path &script,
                     const std::filesystem::path &install_root,
                     const std::filesystem::path &archive,
                     std::string_view sha256, std::string &error) {
  const std::wstring command =
      L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
      L"-File " +
      QuoteArgument(script.wstring()) + L" -ParentPid " +
      std::to_wstring(GetCurrentProcessId()) + L" -InstallRoot " +
      QuoteArgument(install_root.wstring()) + L" -Archive " +
      QuoteArgument(archive.wstring()) + L" -ExpectedSha256 " +
      QuoteArgument(Utf8ToWide(sha256));
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
                      install_root.c_str(), &startup, &process)) {
    error = "Windows could not start the update installer.";
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

#endif

} // namespace

class ReleaseUpdater::Impl {
public:
  Impl(std::filesystem::path executable_path, std::string current_version,
       RestartCallback restart_callback)
      : executable_path_(std::move(executable_path)),
        install_root_(executable_path_.parent_path()),
        restart_callback_(std::move(restart_callback)) {
    state_.current_version = std::move(current_version);
#if !defined(_WIN32)
    state_.phase = ReleaseUpdatePhase::kUnsupported;
    state_.status =
        "In-game updating is currently available in Windows releases.";
#else
    state_.status =
        "Downloads the newest release and refreshes the bundled Blender "
        "tools without installing them into Blender.";
#endif
  }

  ~Impl() {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  ReleaseUpdateState state() const {
    std::scoped_lock lock(mutex_);
    return state_;
  }

  void Start() {
#if defined(_WIN32)
    {
      std::scoped_lock lock(mutex_);
      if (state_.phase == ReleaseUpdatePhase::kChecking ||
          state_.phase == ReleaseUpdatePhase::kDownloading ||
          state_.phase == ReleaseUpdatePhase::kInstalling) {
        return;
      }
      state_.phase = ReleaseUpdatePhase::kChecking;
      state_.latest_version.clear();
      state_.progress = 0.0f;
      state_.status = "Checking the official release channel...";
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    worker_ = std::thread([this] { Run(); });
#endif
  }

private:
  void SetFailure(std::string message) {
    REXLOG_WARN("Custom Engine Layer updater: {}", message);
    std::scoped_lock lock(mutex_);
    state_.phase = ReleaseUpdatePhase::kFailed;
    state_.status = std::move(message);
    state_.progress = 0.0f;
  }

#if defined(_WIN32)
  void Run() {
    const std::string manifest_url(kDefaultManifestUrl);
    if (!IsAllowedUrl(manifest_url, true)) {
      SetFailure("The configured update manifest URL is not trusted.");
      return;
    }
    std::vector<std::uint8_t> manifest_bytes;
    std::string error;
    if (!Download(manifest_url, kMaximumManifestBytes, &manifest_bytes, nullptr,
                  nullptr, error)) {
      SetFailure(std::move(error));
      return;
    }
    const std::string manifest_text(manifest_bytes.begin(),
                                    manifest_bytes.end());
    auto manifest = ParseManifest(manifest_text, error);
    if (!manifest) {
      SetFailure(std::move(error));
      return;
    }
    if (!IsAllowedUrl(manifest->asset_url, false)) {
      SetFailure("The release asset URL is not hosted by GitHub.");
      return;
    }
    {
      std::scoped_lock lock(mutex_);
      state_.latest_version = manifest->version;
      if (CompareVersions(state_.current_version, manifest->version) >= 0) {
        state_.phase = ReleaseUpdatePhase::kUpToDate;
        state_.status = "This is already the newest available release.";
        return;
      }
      state_.phase = ReleaseUpdatePhase::kDownloading;
      state_.status = "Downloading and verifying the new release...";
    }

    const auto update_root = install_root_ / ".cel-update";
    std::error_code ec;
    std::filesystem::create_directories(update_root, ec);
    if (ec) {
      SetFailure("The update staging folder could not be created.");
      return;
    }
    const auto archive = update_root / "release.zip";
    if (!Download(
            manifest->asset_url, kMaximumReleaseBytes, nullptr, &archive,
            [this, expected = manifest->size](std::uint64_t received,
                                              std::uint64_t total) {
              const std::uint64_t denominator = total ? total : expected;
              std::scoped_lock lock(mutex_);
              state_.progress =
                  denominator
                      ? std::clamp(static_cast<float>(
                                       static_cast<double>(received) /
                                       static_cast<double>(denominator)),
                                   0.0f, 1.0f)
                      : 0.0f;
            },
            error)) {
      SetFailure(std::move(error));
      return;
    }
    const auto downloaded_size = std::filesystem::file_size(archive, ec);
    if (ec || downloaded_size != manifest->size) {
      SetFailure("The downloaded release has the wrong size.");
      return;
    }
    if (Lower(Sha256OfFile(archive)) != manifest->sha256) {
      SetFailure("The downloaded release failed its SHA-256 integrity check.");
      return;
    }
    const auto script = update_root / "apply-update.ps1";
    if (!WriteInstallerScript(script, error)) {
      SetFailure(std::move(error));
      return;
    }
    {
      std::scoped_lock lock(mutex_);
      state_.phase = ReleaseUpdatePhase::kInstalling;
      state_.progress = 1.0f;
      state_.status =
          "Update verified. Closing the game to install and restart...";
    }
    if (!LaunchInstaller(script, install_root_, archive, manifest->sha256,
                         error)) {
      SetFailure(std::move(error));
      return;
    }
    REXLOG_INFO("Custom Engine Layer update {} verified and staged",
                manifest->version);
    if (restart_callback_) {
      restart_callback_();
    }
  }
#endif

  std::filesystem::path executable_path_;
  std::filesystem::path install_root_;
  RestartCallback restart_callback_;
  mutable std::mutex mutex_;
  ReleaseUpdateState state_;
  std::thread worker_;
};

ReleaseUpdater::ReleaseUpdater(std::filesystem::path executable_path,
                               std::string current_version,
                               RestartCallback restart_callback)
    : impl_(std::make_unique<Impl>(std::move(executable_path),
                                   std::move(current_version),
                                   std::move(restart_callback))) {}

ReleaseUpdater::~ReleaseUpdater() = default;

ReleaseUpdateState ReleaseUpdater::state() const { return impl_->state(); }

void ReleaseUpdater::Start() { impl_->Start(); }

} // namespace skate3
