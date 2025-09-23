#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ManifestInfo {
  std::filesystem::path out_dir;
  std::filesystem::path vis_dll_path;
  std::filesystem::path runtime_dir;
  std::filesystem::path vis_avs_dat_path;
  std::filesystem::path preset_path;
  std::filesystem::path wav_path;
  int wav_sample_rate = 0;
  int wav_channels = 0;
  int64_t wav_sample_count = 0;
  int width = 0;
  int height = 0;
  int fps = 0;
  int frames = 0;
  std::vector<std::filesystem::path> png_paths;
  std::filesystem::path per_frame_hash_path;
  std::filesystem::path rolling_hash_path;
  std::filesystem::path avi_output_path;
  bool has_vis_avs_dat = false;
  bool has_preset = false;
  bool has_avi_output = false;
};

bool WriteManifest(const ManifestInfo &info);
