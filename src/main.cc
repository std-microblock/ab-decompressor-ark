

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <LzmaLib.h>
#include <lz4.h>

#include "lzham_static_lib.h"

namespace fs = std::filesystem;

enum class CompressionType : uint8_t {
  None = 0,
  Lzma = 1,
  Lz4 = 2,
  Lz4hc = 3,
  Lzham = 4,
};

enum class GameMode { Standard, Arknights };

constexpr uint32_t FLAG_COMPRESSION_MASK = 0x3F;
constexpr uint32_t FLAG_BLOCKS_AND_DIR_COMBINED = 0x40;
constexpr uint32_t FLAG_BLOCK_INFO_AT_END = 0x80;
constexpr uint32_t FLAG_BLOCK_INFO_NEEDS_ALIGNMENT = 0b1000000000;

static auto read_extra_length(std::span<uint8_t> data, size_t &cursor) -> int {
  int length = 0;
  while (cursor < data.size()) {
    uint8_t b = data[cursor];
    length += b;
    cursor++;
    if (b != 0xFF)
      break;
  }
  return length;
}

void hexdump(std::span<const uint8_t> data, size_t max_bytes = 64) {
  size_t to_print = std::min(data.size(), max_bytes);
  for (size_t i = 0; i < to_print; ++i) {
    std::printf("%02X ", data[i]);
    if ((i + 1) % 16 == 0)
      std::printf("\n");
  }
  if (to_print % 16 != 0)
    std::printf("\n");
}

std::vector<uint8_t> decompress_lzak(std::span<const uint8_t> compressed_data,
                                     int uncompressed_size) {
  // hexdump(compressed_data);
  if (compressed_data.empty())
    return {};

  std::vector<uint8_t> fixed_data(compressed_data.begin(),
                                  compressed_data.end());

  size_t ip = 0;
  size_t op = 0;
  size_t size = fixed_data.size();

  while (ip < size) {

    uint8_t token = fixed_data[ip];
    uint8_t literal_len = token & 0x0F;
    uint8_t match_len_nibble = (token >> 4) & 0x0F;

    fixed_data[ip] = (literal_len << 4) | match_len_nibble;
    ip++;

    size_t current_literal_len = literal_len;
    if (literal_len == 0x0F) {
      current_literal_len += read_extra_length(fixed_data, ip);
    }

    ip += current_literal_len;
    op += current_literal_len;

    if (op >= static_cast<size_t>(uncompressed_size)) {
      break;
    }

    if (ip + 2 > size)
      break;

    uint8_t b0 = fixed_data[ip];
    uint8_t b1 = fixed_data[ip + 1];

    fixed_data[ip] = b1;
    fixed_data[ip + 1] = b0;
    ip += 2;

    size_t current_match_len = match_len_nibble;
    if (match_len_nibble == 0x0F) {
      current_match_len += read_extra_length(fixed_data, ip);
    }

    op += (current_match_len + 4);
  }

  std::vector<uint8_t> dest(uncompressed_size);
  int result = LZ4_decompress_safe(
      reinterpret_cast<const char *>(fixed_data.data()),
      reinterpret_cast<char *>(dest.data()),
      static_cast<int>(fixed_data.size()), uncompressed_size);

  if (result < 0) {
    throw std::runtime_error(
        std::format("LZ4AK decompression failed with code: {}", result));
  }

  if (result != uncompressed_size) {
    std::println(stderr, "Warning: LZ4AK expected {} bytes, got {}",
                 uncompressed_size, result);
    dest.resize(result);
  }

  return dest;
}

std::vector<uint8_t> decompress_block(CompressionType type,
                                      std::span<const uint8_t> src,
                                      uint32_t decompressed_size,
                                      GameMode mode) {
  if (type == CompressionType::None) {
    return {src.begin(), src.end()};
  }

  std::vector<uint8_t> dst(decompressed_size);

  switch (type) {
  case CompressionType::Lzma: {
    size_t src_len = src.size();
    size_t dst_len = decompressed_size;

    unsigned char props[5];
    if (src.size() < 5)
      throw std::runtime_error("Invalid LZMA data");
    memcpy(props, src.data(), 5);
    src_len -= 5;
    int res = LzmaUncompress(dst.data(), &dst_len, src.data() + 5, &src_len,
                             props, 5);
    if (res != SZ_OK)
      throw std::runtime_error("LZMA Decomp failed");
    break;
  }

  case CompressionType::Lz4:
  case CompressionType::Lz4hc: {
    int res = LZ4_decompress_safe(reinterpret_cast<const char *>(src.data()),
                                  reinterpret_cast<char *>(dst.data()),
                                  static_cast<int>(src.size()),
                                  static_cast<int>(decompressed_size));
    if (res < 0)
      throw std::runtime_error("LZ4 Decomp failed");
    break;
  }

  case CompressionType::Lzham: {
    if (mode == GameMode::Arknights) {
      return decompress_lzak(src, decompressed_size);
    } else {

      lzham_decompress_params params{};
      params.m_struct_size = sizeof(lzham_decompress_params);
      params.m_dict_size_log2 = 29;

      size_t dst_len = decompressed_size;
      size_t src_len = src.size();

      int status = lzham_decompress_memory(&params, dst.data(), &dst_len,
                                           src.data(), src_len, nullptr);
      if (status != LZHAM_COMP_STATUS_SUCCESS) {
        throw std::runtime_error(
            std::format("LZHAM Decomp failed: {}", status));
      }
    }
    break;
  }
  default:
    throw std::runtime_error("Unknown compression type");
  }
  return dst;
}

template <typename T> T swap_endian(T u) {
  if constexpr (sizeof(T) == 1)
    return u;
  union {
    T u;
    unsigned char u8[sizeof(T)];
  } source, dest;
  source.u = u;
  for (size_t k = 0; k < sizeof(T); k++)
    dest.u8[k] = source.u8[sizeof(T) - k - 1];
  return dest.u;
}

class BinaryReader {
  const std::vector<uint8_t> &data_;
  size_t pos_ = 0;

public:
  explicit BinaryReader(const std::vector<uint8_t> &data) : data_(data) {}

  template <typename T> T read_be() {
    if (pos_ + sizeof(T) > data_.size())
      throw std::out_of_range("buffer overflow");
    T val;
    std::memcpy(&val, &data_[pos_], sizeof(T));
    pos_ += sizeof(T);
    return swap_endian(val);
  }

  std::string read_string() {
    std::string s;
    while (pos_ < data_.size() && data_[pos_] != 0) {
      s += static_cast<char>(data_[pos_++]);
    }
    pos_++;
    return s;
  }

  std::vector<uint8_t> read_bytes(size_t n) {
    if (pos_ + n > data_.size())
      throw std::out_of_range(std::format(
          "buffer overflow: pos {} + n {} > size {}", pos_, n, data_.size()));
    std::vector<uint8_t> d(data_.begin() + pos_, data_.begin() + pos_ + n);
    pos_ += n;
    return d;
  }

  std::span<const uint8_t> get_span(size_t n) {
    if (pos_ + n > data_.size())
      throw std::out_of_range(std::format(
          "buffer overflow: pos {} + n {} > size {}", pos_, n, data_.size()));
    auto s = std::span<const uint8_t>(data_.data() + pos_, n);
    pos_ += n;
    return s;
  }

  void seek(size_t p) { pos_ = p; }
  size_t tell() const { return pos_; }
  void align(size_t alignment) {
    while (pos_ % alignment != 0)
      pos_++;
  }
};

class BinaryWriter {
  std::ofstream &ofs_;

public:
  explicit BinaryWriter(std::ofstream &ofs) : ofs_(ofs) {}

  template <typename T> void write_be(T val) {
    T swapped = swap_endian(val);
    ofs_.write(reinterpret_cast<const char *>(&swapped), sizeof(T));
  }

  void write_bytes(const void *data, size_t size) {
    ofs_.write(reinterpret_cast<const char *>(data), size);
  }

  void write_string(const std::string &s) {
    ofs_.write(s.c_str(), s.size() + 1);
  }

  void align(size_t alignment) {
    auto pos = ofs_.tellp();
    size_t pad = (alignment - (pos % alignment)) % alignment;
    for (size_t i = 0; i < pad; ++i)
      ofs_.put(0);
  }

  size_t tell() { return ofs_.tellp(); }
};

struct ArchiveBlockInfo {
  uint32_t uncompressed_size;
  uint32_t compressed_size;
  uint16_t flags;

  [[nodiscard]] CompressionType get_compression() const {
    return static_cast<CompressionType>(flags & FLAG_COMPRESSION_MASK);
  }
};

struct ArchiveNode {
  uint64_t offset;
  uint64_t size;
  uint32_t status;
  std::string path;
};

void process_file(const fs::path &input_path, const fs::path &output_path,
                  GameMode game_mode) {
  if (!fs::exists(input_path)) {
    throw std::runtime_error("Input file not found");
  }

  std::ifstream ifs(input_path, std::ios::binary | std::ios::ate);
  size_t file_size = ifs.tellg();
  ifs.seekg(0);
  std::vector<uint8_t> raw_file(file_size);
  ifs.read(reinterpret_cast<char *>(raw_file.data()), file_size);
  ifs.close();

  BinaryReader reader(raw_file);

  std::string signature = reader.read_string();
  uint32_t version = reader.read_be<uint32_t>();
  std::string unity_ver = reader.read_string();
  std::string unity_rev = reader.read_string();

  if (signature != "UnityFS") {
    throw std::runtime_error("Only UnityFS format supported");
  }

  int64_t bundle_size = reader.read_be<int64_t>();
  uint32_t compressed_blocks_info_size = reader.read_be<uint32_t>();
  uint32_t uncompressed_blocks_info_size = reader.read_be<uint32_t>();
  uint32_t flags = reader.read_be<uint32_t>();

  if (version >= 7)
    reader.align(16);

  auto raw_block_info = reader.get_span(compressed_blocks_info_size);

  CompressionType header_comp =
      static_cast<CompressionType>(flags & FLAG_COMPRESSION_MASK);

  auto block_info_data =
      decompress_block(header_comp, raw_block_info,
                       uncompressed_blocks_info_size, GameMode::Standard);

  BinaryReader bi_reader(block_info_data);

  bi_reader.read_bytes(16);

  uint32_t blocks_count = bi_reader.read_be<uint32_t>();
  std::vector<ArchiveBlockInfo> blocks(blocks_count);
  for (auto &b : blocks) {
    b.uncompressed_size = bi_reader.read_be<uint32_t>();
    b.compressed_size = bi_reader.read_be<uint32_t>();
    b.flags = bi_reader.read_be<uint16_t>();
  }

  uint32_t nodes_count = bi_reader.read_be<uint32_t>();
  std::vector<ArchiveNode> nodes(nodes_count);
  for (auto &n : nodes) {
    n.offset = bi_reader.read_be<int64_t>();
    n.size = bi_reader.read_be<int64_t>();
    n.status = bi_reader.read_be<uint32_t>();
    n.path = bi_reader.read_string();
  }

  std::ofstream ofs(output_path, std::ios::binary);
  BinaryWriter writer(ofs);

  std::vector<uint8_t> all_decompressed_data;

  if (flags & FLAG_BLOCK_INFO_AT_END) {

  } else {
  }

  if (flags & FLAG_BLOCKS_AND_DIR_COMBINED) {
  }

  std::cout << std::format("Decompressing {} blocks...\n", blocks.size());

  std::vector<ArchiveBlockInfo> new_blocks;

  if (flags & FLAG_BLOCK_INFO_NEEDS_ALIGNMENT)
    reader.align(16);
  for (size_t i = 0; i < blocks.size(); ++i) {
    auto &old_blk = blocks[i];
    auto compressed_bytes = reader.get_span(old_blk.compressed_size);

    std::vector<uint8_t> raw =
        decompress_block(old_blk.get_compression(), compressed_bytes,
                         old_blk.uncompressed_size, game_mode);

    size_t offset_in_new_stream = all_decompressed_data.size();
    all_decompressed_data.insert(all_decompressed_data.end(), raw.begin(),
                                 raw.end());

    ArchiveBlockInfo new_blk;
    new_blk.uncompressed_size = static_cast<uint32_t>(raw.size());
    new_blk.compressed_size = static_cast<uint32_t>(raw.size());
    new_blk.flags = 0;
    new_blocks.push_back(new_blk);

    std::cout << std::format("\rBlock {}/{} ({} -> {})", i + 1, blocks.size(),
                             old_blk.compressed_size, raw.size())
              << std::flush;
  }
  std::cout << "\nBlocks decompressed. Rebuilding header...\n";

  std::vector<uint8_t> new_block_info_blob;

  auto push_u32_be = [&](uint32_t v) {
    v = swap_endian(v);
    uint8_t *p = reinterpret_cast<uint8_t *>(&v);
    new_block_info_blob.insert(new_block_info_blob.end(), p, p + 4);
  };
  auto push_s64_be = [&](int64_t v) {
    v = swap_endian(v);
    uint8_t *p = reinterpret_cast<uint8_t *>(&v);
    new_block_info_blob.insert(new_block_info_blob.end(), p, p + 8);
  };
  auto push_u16_be = [&](uint16_t v) {
    v = swap_endian(v);
    uint8_t *p = reinterpret_cast<uint8_t *>(&v);
    new_block_info_blob.insert(new_block_info_blob.end(), p, p + 2);
  };
  auto push_bytes = [&](const void *d, size_t s) {
    const uint8_t *p = static_cast<const uint8_t *>(d);
    new_block_info_blob.insert(new_block_info_blob.end(), p, p + s);
  };

  uint8_t null_hash[16] = {0};
  push_bytes(null_hash, 16);

  push_u32_be(static_cast<uint32_t>(new_blocks.size()));
  for (const auto &b : new_blocks) {
    push_u32_be(b.uncompressed_size);
    push_u32_be(b.compressed_size);
    push_u16_be(b.flags);
  }

  push_u32_be(static_cast<uint32_t>(nodes.size()));
  for (const auto &n : nodes) {
    push_s64_be(n.offset);
    push_s64_be(n.size);
    push_u32_be(n.status);
    push_bytes(n.path.c_str(), n.path.length() + 1);
  }

  writer.write_string("UnityFS");
  writer.write_be<uint32_t>(version);
  writer.write_string(unity_ver);
  writer.write_string(unity_rev);

  int64_t header_min_size = writer.tell() + 8 + 4 + 4 + 4;

  size_t header_end_pos_approx = header_min_size;
  if (version >= 7) {
    size_t rem = header_end_pos_approx % 16;
    if (rem != 0)
      header_end_pos_approx += (16 - rem);
  }

  int64_t total_file_size = header_end_pos_approx + new_block_info_blob.size() +
                            all_decompressed_data.size();

  writer.write_be<int64_t>(total_file_size);

  writer.write_be<uint32_t>(static_cast<uint32_t>(new_block_info_blob.size()));
  writer.write_be<uint32_t>(static_cast<uint32_t>(new_block_info_blob.size()));

  uint32_t new_flags = FLAG_BLOCKS_AND_DIR_COMBINED;
  writer.write_be<uint32_t>(new_flags);

  if (version >= 7)
    writer.align(16);

  writer.write_bytes(new_block_info_blob.data(), new_block_info_blob.size());

  writer.write_bytes(all_decompressed_data.data(),
                     all_decompressed_data.size());

  std::cout << "Success. Output written to " << output_path.string() << "\n";
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::println(
        stderr,
        "Usage: UnpackAB --game [std|arknights] <input.ab> [output.ab]");
    return 1;
  }

  try {
    lzham_z_stream stream_init_dummy;
    (void)stream_init_dummy;

    fs::path input_path;
    fs::path output_path;
    GameMode mode = GameMode::Standard;

    int arg_idx = 1;
    if (std::string(argv[arg_idx]) == "--game") {
      if (argc <= arg_idx + 1)
        throw std::runtime_error("Missing game argument");
      std::string g = argv[arg_idx + 1];
      if (g == "arknights")
        mode = GameMode::Arknights;
      else if (g == "std")
        mode = GameMode::Standard;
      else
        throw std::runtime_error("Unknown game mode");
      arg_idx += 2;
    }

    if (arg_idx >= argc)
      throw std::runtime_error("Missing input file");
    input_path = argv[arg_idx++];

    if (arg_idx < argc) {
      output_path = argv[arg_idx];
    } else {

      output_path =
          input_path.parent_path() / (input_path.stem().string() + "_unpacked" +
                                      input_path.extension().string());
    }

    if (input_path == output_path) {

      fs::path temp = output_path;
      temp += ".tmp";
      process_file(input_path, temp, mode);
      fs::rename(temp, output_path);
    } else {
      process_file(input_path, output_path, mode);
    }

  } catch (const std::exception &e) {
    std::println(stderr, "Error: {}", e.what());
    return 1;
  }

  return 0;
}