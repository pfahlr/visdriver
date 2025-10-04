#include "manifest.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <system_error>
#include <vector>

#include <windows.h>

namespace {

std::string WideToUtf8(const std::wstring &value) {
  if (value.empty()) {
    return std::string();
  }
  const int required =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1,
                          nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return std::string("(conversion error)");
  }
  std::string result(static_cast<size_t>(required - 1), '\0');
  if (!result.empty()) {
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1,
                        result.data(), required, nullptr, nullptr);
  }
  return result;
}

std::string PathToUtf8(const std::filesystem::path &path) {
  if (path.empty()) {
    return std::string();
  }
  return WideToUtf8(path.generic_wstring());
}

bool HasParentTraversal(const std::filesystem::path &path) {
  for (const auto &part : path) {
    if (part == std::filesystem::path(L"..")) {
      return true;
    }
  }
  return false;
}

std::filesystem::path MakeRelativeIfPossible(const std::filesystem::path &path,
                                             const std::filesystem::path &base) {
  if (path.empty() || base.empty()) {
    return path;
  }
  std::error_code ec;
  std::filesystem::path relative = std::filesystem::relative(path, base, ec);
  if (!ec) {
    relative = relative.lexically_normal();
    if (!HasParentTraversal(relative)) {
      return relative;
    }
  }
  return path;
}

class JsonWriter {
 public:
  void BeginObject() {
    WriteValuePrefix();
    buffer_.push_back('{');
    stack_.push_back({ContextType::Object, true, false});
  }

  void EndObject() {
    if (stack_.empty() || stack_.back().type != ContextType::Object) {
      error_ = true;
      return;
    }
    if (stack_.back().expect_value) {
      error_ = true;
      return;
    }
    buffer_.push_back('}');
    stack_.pop_back();
  }

  void BeginArray() {
    WriteValuePrefix();
    buffer_.push_back('[');
    stack_.push_back({ContextType::Array, true, false});
  }

  void EndArray() {
    if (stack_.empty() || stack_.back().type != ContextType::Array) {
      error_ = true;
      return;
    }
    buffer_.push_back(']');
    stack_.pop_back();
  }

  void Key(const std::string &key) {
    if (stack_.empty() || stack_.back().type != ContextType::Object ||
        stack_.back().expect_value) {
      error_ = true;
      return;
    }
    ContextInfo &ctx = stack_.back();
    if (!ctx.first) {
      buffer_.push_back(',');
    }
    ctx.first = false;
    WriteEscapedString(key);
    buffer_.push_back(':');
    ctx.expect_value = true;
  }

  void String(const std::string &value) {
    WriteValuePrefix();
    WriteEscapedString(value);
  }

  void Number(int64_t value) {
    WriteValuePrefix();
    buffer_ += std::to_string(value);
  }

  void Null() {
    WriteValuePrefix();
    buffer_ += "null";
  }

  bool IsOk() const { return !error_ && stack_.empty(); }

  const std::string &Get() const { return buffer_; }

 private:
  enum class ContextType { Object, Array };

  struct ContextInfo {
    ContextType type;
    bool first;
    bool expect_value;
  };

  void WriteValuePrefix() {
    if (stack_.empty()) {
      return;
    }
    ContextInfo &ctx = stack_.back();
    if (ctx.type == ContextType::Array) {
      if (!ctx.first) {
        buffer_.push_back(',');
      }
      ctx.first = false;
    } else {
      if (!ctx.expect_value) {
        error_ = true;
        return;
      }
      ctx.expect_value = false;
    }
  }

  void WriteEscapedString(const std::string &value) {
    buffer_.push_back('"');
    for (const unsigned char ch : value) {
      switch (ch) {
        case '\\':
          buffer_ += "\\\\";
          break;
        case '"':
          buffer_ += "\\\"";
          break;
        case '\b':
          buffer_ += "\\b";
          break;
        case '\f':
          buffer_ += "\\f";
          break;
        case '\n':
          buffer_ += "\\n";
          break;
        case '\r':
          buffer_ += "\\r";
          break;
        case '\t':
          buffer_ += "\\t";
          break;
        default:
          if (ch < 0x20) {
            char buffer[7];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
            buffer_ += buffer;
          } else {
            buffer_.push_back(static_cast<char>(ch));
          }
          break;
      }
    }
    buffer_.push_back('"');
  }

  std::string buffer_;
  std::vector<ContextInfo> stack_;
  bool error_ = false;
};

bool WriteAllBytes(HANDLE handle, const std::string &data) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  if (data.empty()) {
    return true;
  }
  const char *ptr = data.data();
  size_t remaining = data.size();
  while (remaining > 0) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(handle, ptr, chunk, &written, nullptr)) {
      return false;
    }
    if (written == 0) {
      return false;
    }
    ptr += written;
    remaining -= written;
  }
  return true;
}

}  // namespace

bool WriteManifest(const ManifestInfo &info) {
  if (info.out_dir.empty()) {
    std::wcerr << L"ERROR: Manifest output directory is empty.\n";
    return false;
  }

  JsonWriter writer;
  writer.BeginObject();
  writer.Key("tool_version");
  writer.String("visdriver+verify/0.1");

  writer.Key("inputs");
  writer.BeginObject();
  writer.Key("vis_dll");
  writer.String(PathToUtf8(info.vis_dll_path));
  writer.Key("out_dll");
  if (info.has_out_dll) {
    writer.String(PathToUtf8(info.out_dll_path));
  } else {
    writer.Null();
  }
  writer.Key("runtime_dir");
  writer.String(PathToUtf8(info.runtime_dir));
  writer.Key("vis_avs_dat");
  if (info.has_vis_avs_dat) {
    writer.String(PathToUtf8(info.vis_avs_dat_path));
  } else {
    writer.Null();
  }
  writer.Key("preset");
  if (info.has_preset) {
    writer.String(PathToUtf8(info.preset_path));
  } else {
    writer.Null();
  }
  writer.Key("wav");
  writer.BeginObject();
  writer.Key("path");
  writer.String(PathToUtf8(info.wav_path));
  writer.Key("sample_rate");
  writer.Number(info.wav_sample_rate);
  writer.Key("channels");
  writer.Number(info.wav_channels);
  writer.Key("sample_count");
  writer.Number(info.wav_sample_count);
  writer.EndObject();
  writer.EndObject();

  writer.Key("render");
  writer.BeginObject();
  writer.Key("width");
  writer.Number(info.width);
  writer.Key("height");
  writer.Number(info.height);
  writer.Key("fps");
  writer.Number(info.fps);
  writer.Key("frames");
  writer.Number(info.frames);
  writer.EndObject();

  writer.Key("outputs");
  writer.BeginObject();
  writer.Key("png_frames");
  writer.BeginArray();
  for (const auto &png_path : info.png_paths) {
    const std::filesystem::path relative =
        MakeRelativeIfPossible(png_path, info.out_dir);
    writer.String(PathToUtf8(relative));
  }
  writer.EndArray();
  writer.Key("per_frame_hashes");
  writer.String(PathToUtf8(
      MakeRelativeIfPossible(info.per_frame_hash_path, info.out_dir)));
  writer.Key("rolling_hash");
  writer.String(PathToUtf8(
      MakeRelativeIfPossible(info.rolling_hash_path, info.out_dir)));
  writer.Key("avi");
  if (info.has_avi_output) {
    writer.String(PathToUtf8(
        MakeRelativeIfPossible(info.avi_output_path, info.out_dir)));
  } else {
    writer.Null();
  }
  writer.EndObject();

  writer.EndObject();

  if (!writer.IsOk()) {
    std::wcerr << L"ERROR: Failed to encode manifest JSON.\n";
    return false;
  }

  const std::string json = writer.Get();
  const std::filesystem::path manifest_path = info.out_dir / L"manifest.json";
  HANDLE handle = CreateFileW(manifest_path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to create manifest '"
               << manifest_path.wstring() << L"' (error " << error << L").\n";
    return false;
  }

  bool ok = WriteAllBytes(handle, json);
  if (ok) {
    if (!FlushFileBuffers(handle)) {
      ok = false;
    }
  }

  CloseHandle(handle);

  if (!ok) {
    std::wcerr << L"ERROR: Failed to write manifest file.\n";
  }

  return ok;
}
