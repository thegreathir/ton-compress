/*
 * solution.cpp
 *
 * Example solution.
 * This is (almost) how blocks are actually compressed in TON.
 * Normally, blocks are stored using vm::std_boc_serialize with mode=31.
 * Compression algorithm takes a block, converts it to mode=2 (which has less
 * extra information) and compresses it using lz4.
 */
#include "td/utils/base64.h"
#include "td/utils/lz4.h"
#include "vm/boc.h"
#include <iostream>

#include "libzpaq.h"

class ZpaqReader : public libzpaq::Reader {
public:
  ZpaqReader(td::Slice data) : data_(data) {}
  int read(char *buf, int n) override {
    if (pos_ >= data_.size()) {
      return 0;
    }
    int to_read = std::min(n, static_cast<int>(data_.size() - pos_));
    memcpy(buf, &data_.data()[pos_], to_read);
    pos_ += to_read;
    return to_read;
  }

  int get() override { return -1; }

private:
  td::Slice data_;
  std::size_t pos_ = 0;
};

class ZpaqWriter : public libzpaq::Writer {
public:
  void write(const char *buf, int n) override { data_.append(buf, buf + n); }
  void put(int c) override { data_.push_back(c); }
  const std::string &str() { return data_; }

private:
  std::string data_;
};

void libzpaq::error(const char *msg) { std::cerr << msg; }

td::BufferSlice compress(td::Slice data) {
  td::Ref<vm::Cell> root = vm::std_boc_deserialize(data).move_as_ok();
  td::BufferSlice serialized = vm::std_boc_serialize(root, 2).move_as_ok();
  ZpaqReader reader(serialized.as_slice());
  ZpaqWriter writer{};
  libzpaq::compress(&reader, &writer, "5", 0 ,0 ,false);
  return td::BufferSlice{writer.str().c_str(), writer.str().size()};
}

td::BufferSlice decompress(td::Slice data) {
  ZpaqReader reader(data);
  ZpaqWriter writer{};
  libzpaq::decompress(&reader, &writer);
  td::BufferSlice serialized{writer.str().c_str(), writer.str().size()};
  auto root = vm::std_boc_deserialize(serialized).move_as_ok();
  return vm::std_boc_serialize(root, 31).move_as_ok();
}

int main() {
  std::string mode;
  std::cin >> mode;
  CHECK(mode == "compress" || mode == "decompress");

  std::string base64_data;
  std::cin >> base64_data;
  CHECK(!base64_data.empty());

  td::BufferSlice data(td::base64_decode(base64_data).move_as_ok());

  if (mode == "compress") {
    data = compress(data);
  } else {
    data = decompress(data);
  }

  std::cout << td::base64_encode(data) << std::endl;
}
